#!/usr/bin/env python3
"""Differential test: translated C vs Unicorn for instruction forms taken from game.EXE.

usage: difftest.py <game.EXE> <work_dir> [--trials N] [--filter MNEM,...] [--seed S]

Each test is a tiny guest function at 0x100000+k*0x80:
    [x87 prologue]  insn  [epilogue]  pushfd  ret
The same bytes run in Unicorn (reference) and through our translator (C, compiled into a
dylib). Registers, arithmetic flags, guest memory and the x87 stack are compared.
"""
import argparse
import ctypes
import os
import random
import struct
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))

import capstone  # noqa: E402
import unicorn  # noqa: E402
import unicorn.x86_const as U  # noqa: E402

from recomp.discover import Discovery  # noqa: E402
from recomp.pe_image import Image, Section  # noqa: E402
from recomp.translate import Translator, fn_name  # noqa: E402

CODE_BASE = 0x100000
CODE_SIZE = 0x100000
DATA_LO, DATA_HI = 0x400000, 0x800000   # covers the game's absolute data addresses
STACK_TOP = 0x7F0000
FPU_IN = 0x7E0000                        # 4 input doubles + control word
FPU_OUT = 0x7E1000                       # status word + 8 dumped doubles
FLAGS_IN = 0x7E2000                      # incoming EFLAGS, loaded with push [FLAGS_IN]; popfd
SLOT = 0x100

REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
UREG = {r: getattr(U, f"UC_X86_REG_{r.upper()}") for r in REGS}
SKIP = {"call", "jmp", "ret", "int", "int3", "hlt", "in", "out", "cli", "sti", "cpuid", "rdtsc",
        "loop", "jecxz", "fnstenv", "fldenv", "wait", "nop"}
EDGE = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
        0xFFFFFFFE, 0x12345678]

# Flags that are architecturally undefined after an instruction: never compared.
UNDEF = {"and": "a", "or": "a", "xor": "a", "test": "a", "shl": "ao", "sal": "ao", "shr": "ao",
         "sar": "ao", "rol": "o", "ror": "o", "shld": "ao", "shrd": "ao", "mul": "szap",
         "imul": "szap", "div": "csozap", "idiv": "csozap"}
FLAG_BITS = {"c": 0, "p": 2, "a": 4, "z": 6, "s": 7, "o": 11}


class TestImage:
    """Just enough of pe_image.Image for Discovery/Translator."""

    def __init__(self, code):
        self.base = CODE_BASE
        self.size = CODE_SIZE
        self.mem = bytearray(CODE_SIZE)
        self.mem[:len(code)] = code
        self.text = Section(".text", CODE_BASE, CODE_SIZE, CODE_SIZE, 0x60000020)
        self.sections = [self.text]
        self.relocs = set()
        self.imports = {}
        self.entry = CODE_BASE

    def in_text(self, va):
        return CODE_BASE <= va < CODE_BASE + CODE_SIZE

    def contains(self, va):
        return self.in_text(va)

    def u8(self, va):
        return self.mem[va - CODE_BASE]

    def u32(self, va):
        return struct.unpack_from("<I", self.mem, va - CODE_BASE)[0]

    def bytes(self, va, n):
        o = va - CODE_BASE
        return bytes(self.mem[o:o + n])


def is_x87(insn):
    return insn.mnem.startswith("f")


def uses_mem(insn):
    return any(k == "mem" for k, _, _ in insn.ops) or insn.mnem in (
        "push", "pop", "pushal", "popal", "pushfd", "popfd", "pushf", "popf", "leave", "xlatb") \
        or insn.mnem.split(" ")[-1][:4] in ("movs", "stos", "scas", "cmps", "lods")


def collect_forms(img, d, per_form=2, filt=None):
    smc = Translator(img, d).smc_bytes
    seen = defaultdict(list)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for e, f in d.functions.items():
        for va in f["insns"]:
            insn = d.insns[va]
            m = insn.mnem
            if m in SKIP or m.startswith("j"):
                continue
            if filt and m not in filt:
                continue
            if any(b in smc for b in range(va, insn.end)):
                continue
            if any(k == "mem" and v[4] == "fs" for k, v, _ in insn.ops):
                continue
            shape = (m, tuple((k, osz) if k != "reg" else (k, v) for k, v, osz in insn.ops),
                     insn.raw[:1])
            if len(seen[shape]) < per_form:
                seen[shape].append(insn.raw)
    return seen


def synth_cond_tests():
    """cmp + setcc / jcc over every condition."""
    out = []
    for cc in range(16):
        out.append(("setcc", bytes([0x39, 0xD8, 0x0F, 0x90 + cc, 0xC1])))           # cmp eax,ebx; setcc cl
        out.append(("jcc", bytes([0x39, 0xD8, 0x70 + cc, 0x03, 0x8D, 0x49, 0x01])))   # cmp; jcc +3; lea ecx,[ecx+1]
        out.append(("jcc32", bytes([0x85, 0xC3, 0x0F, 0x80 + cc, 0x03, 0, 0, 0, 0x8D, 0x49, 0x01])))  # test
    return out


X87_PUSHES = ("fld", "fild", "fldz", "fld1", "fldpi", "fldl2t", "fldl2e", "fldlg2", "fldln2",
              "fptan", "fsincos")
X87_CC_DEFINED = ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ftst", "fxam", "fprem",
                  "fprem1", "ficom", "ficomp")


def depth_of(m):
    return 7 if m in X87_PUSHES else 8


def build_case(body, x87, depth=8):
    pre = b"\xff\x35" + struct.pack("<I", FLAGS_IN) + b"\x9d"   # push [FLAGS_IN]; popfd
    post = b""
    if x87:
        # fninit; fldcw [FPU_IN+32]; fld qword [FPU_IN+24..0]
        pre += b"\xdb\xe3" + b"\xd9\x2d" + struct.pack("<I", FPU_IN + 64)
        for k in reversed(range(depth)):
            pre += b"\xdd\x05" + struct.pack("<I", FPU_IN + 8 * k)
        # fnstsw [FPU_OUT]; 8x fstp qword [FPU_OUT+8+8k]
        post = b"\xdd\x3d" + struct.pack("<I", FPU_OUT)
        for k in range(8):
            post += b"\xdd\x1d" + struct.pack("<I", FPU_OUT + 8 + 8 * k)
    return pre + body + post + b"\x9c\xc3"   # pushfd; ret


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("work")
    ap.add_argument("--trials", type=int, default=12)
    ap.add_argument("--filter", default=None)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--per-form", type=int, default=2)
    args = ap.parse_args()
    rnd = random.Random(args.seed)
    filt = set(args.filter.split(",")) if args.filter else None

    img = Image(args.exe)
    d = Discovery(img).run().partition().mark_text_data()
    forms = collect_forms(img, d, args.per_form, filt)

    cases = []  # (label, code bytes, x87, mnemonic)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for shape, raws in forms.items():
        for raw in raws:
            ins = next(md.disasm(raw, 0))
            cases.append((f"{ins.mnemonic} {ins.op_str}", raw, shape[0].startswith("f"), shape[0]))
    if not filt:
        for label, raw in synth_cond_tests():
            ins = next(md.disasm(raw[2:], 0))
            cases.append((f"{label}: {ins.mnemonic}", raw, False, label))

    code = bytearray()
    starts = []
    for label, raw, x87, m in cases:
        blob = build_case(raw, x87, depth_of(m))
        assert len(blob) <= SLOT, label
        starts.append(CODE_BASE + len(code))
        code += blob + b"\xcc" * (SLOT - len(blob))

    timg = TestImage(bytes(code))
    td = Discovery(timg)
    from collections import deque
    q = deque()
    for s in starts:
        td.add_entry(s, "test")
        q.append(s)
    while q:
        td.walk(q.popleft(), q)
    td.partition()
    tr = Translator(timg, td)

    os.makedirs(args.work, exist_ok=True)
    src = ['#define RECOMP_GENERATED  /* as in the game build (local memory base) */', '#include "recomp.h"']
    src += [f"void {fn_name(s)}(void);" for s in starts]
    for s in starts:
        src.append(tr.translate_function(s))
    open(os.path.join(args.work, "tests.c"), "w").write("\n".join(src))
    lib = os.path.join(args.work, "libdifftest.dylib")
    subprocess.check_call([
        "clang", "-std=c11", "-O1", "-shared", "-fPIC", "-w", f"-I{ROOT}/runtime/include",
        "-o", lib, os.path.join(args.work, "tests.c"),
        f"{ROOT}/runtime/src/x86_helpers.c", f"{ROOT}/runtime/tests/difftest_stubs.c"])
    so = ctypes.CDLL(lib)
    so.tr_init()

    class RegsT(ctypes.Structure):
        _fields_ = [(r, ctypes.c_uint32) for r in REGS] + [("df", ctypes.c_uint32), ("fs_base", ctypes.c_uint32)]

    class FpuT(ctypes.Structure):
        _fields_ = [("st", ctypes.c_double * 8), ("top", ctypes.c_uint32), ("cw", ctypes.c_uint16),
                    ("sw", ctypes.c_uint16), ("tag", ctypes.c_uint16), ("mm", ctypes.c_uint64 * 8)]

    Rg = RegsT.in_dll(so, "R")
    Fp = FpuT.in_dll(so, "F")
    Mp = ctypes.c_void_p.in_dll(so, "M").value
    trap = ctypes.c_int.in_dll(so, "tr_trap")

    uc = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
    uc.mem_map(CODE_BASE, CODE_SIZE)
    uc.mem_map(DATA_LO, DATA_HI - DATA_LO)
    uc.mem_write(CODE_BASE, bytes(code))
    ctypes.memmove(Mp + CODE_BASE, bytes(code), len(code))
    so.tr_set_pristine.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p]
    so.tr_poke.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32]
    so.tr_collect.restype = ctypes.c_uint32

    fails = defaultdict(list)
    ran = skipped = 0
    pristine = bytes(rnd.getrandbits(8) for _ in range(4096)) * ((DATA_HI - DATA_LO) // 4096)
    uc.mem_write(DATA_LO, pristine)
    so.tr_set_pristine(DATA_LO, len(pristine), pristine)
    pristine = bytearray(pristine)
    written = []
    uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, lambda uc_, acc, addr, size, val, ud: written.append((addr, size)))
    MAXD = 1 << 16
    d_addrs = (ctypes.c_uint32 * MAXD)()
    d_vals = (ctypes.c_uint8 * MAXD)()

    def poke(addr, data):
        uc.mem_write(addr, bytes(data))
        so.tr_poke(addr, bytes(data), len(data))
        pristine[addr - DATA_LO:addr - DATA_LO + len(data)] = data

    for (label, raw, x87, m), start in zip(cases, starts):
        fn = getattr(so, fn_name(start))
        ret_va = start + len(build_case(raw, x87, depth_of(m))) - 1
        memcase = any(c in label for c in ("ptr", "push", "pop", "leave", "movs", "stos", "scas",
                                           "cmps", "lods", "xlat", "pusha", "popa"))
        for t in range(args.trials):
            regs = {}
            for r in REGS:
                if memcase:
                    regs[r] = rnd.randrange(0x500000, 0x700000) if rnd.random() < 0.8 else rnd.randrange(0, 0x40)
                else:
                    regs[r] = rnd.choice(EDGE) if rnd.random() < 0.4 else rnd.getrandbits(32)
            if memcase and "*" in label:
                for r in REGS:  # scaled index registers must stay small
                    if f"{r}*" in label:
                        regs[r] = rnd.randrange(0, 0x100)
            if "rep" in label:
                regs["ecx"] = rnd.randrange(0, 40)
            regs["esp"] = STACK_TOP
            if "leave" in label:
                regs["ebp"] = STACK_TOP - 0x100
            df = 1 if (rnd.random() < 0.2 and "rep" not in label) else 0
            flags_in = (rnd.getrandbits(32) & 0x8D5) | 0x202 | (df << 10)
            poke(FLAGS_IN, struct.pack("<I", flags_in))
            if x87:
                vals = [rnd.choice([0.0, -0.0, 1.0, -1.5, 3.25, 1e10, -7e-5, 2.0 ** 31, 0.5, 100.0])
                        if rnd.random() < 0.5 else rnd.uniform(-1e4, 1e4) for _ in range(8)]
                poke(FPU_IN, b"".join(struct.pack("<d", v) for v in vals) + struct.pack("<H", 0x27F))

            # reference
            for r in REGS:
                uc.reg_write(UREG[r], regs[r])
            uc.reg_write(U.UC_X86_REG_EFLAGS, 0x2)
            written.clear()
            try:
                uc.emu_start(start, ret_va, count=10000)
                fault = None if uc.reg_read(U.UC_X86_REG_EIP) == ret_va else "stopped early"
            except unicorn.UcError as e:
                fault = e
            ref_changes = {}
            for a, n in written:
                for b in range(a, a + n):
                    if DATA_LO <= b < DATA_HI:
                        v = uc.mem_read(b, 1)[0]
                        if v != pristine[b - DATA_LO]:
                            ref_changes[b] = v
            for a, n in written:
                lo, hi = max(a, DATA_LO), min(a + n, DATA_HI)
                if lo < hi:
                    uc.mem_write(lo, bytes(pristine[lo - DATA_LO:hi - DATA_LO]))
            if fault:
                skipped += 1
                so.tr_collect(d_addrs, d_vals, MAXD)
                continue
            ref = {r: uc.reg_read(UREG[r]) for r in REGS}
            ref_flags = struct.unpack("<I", bytes(
                ref_changes.get(ref["esp"] + i, pristine[ref["esp"] + i - DATA_LO]) for i in range(4)))[0]

            # ours
            for r in REGS:
                setattr(Rg, r, regs[r])
            Rg.df = 0
            Fp.top, Fp.cw, Fp.sw = 0, 0x27F, 0
            trap.value = 0
            fn()
            ran += 1
            n = so.tr_collect(d_addrs, d_vals, MAXD)
            our_changes = {d_addrs[i]: d_vals[i] for i in range(min(n, MAXD))}
            if trap.value:
                fails[label].append(f"trap {trap.value} in={ {k: hex(v) for k, v in regs.items()} } ref_out={ {k: hex(v) for k, v in ref.items()} }")
                continue
            ours = {r: getattr(Rg, r) for r in REGS}
            ours["esp"] = (ours["esp"] - 4) & 0xFFFFFFFF   # undo the test's ret
            our_flags = struct.unpack("<I", bytes(
                our_changes.get(ours["esp"] + i, pristine[ours["esp"] + i - DATA_LO]) for i in range(4)))[0]

            errs = []
            for r in REGS:
                if ref[r] != ours[r]:
                    errs.append(f"{r}: ref {ref[r]:#x} ours {ours[r]:#x}")
            undef = UNDEF.get(m, "")
            if m in ("shl", "sal", "shr", "sar", "rol", "ror") and label.split(",")[-1].strip() in ("1", "0x1"):
                undef = undef.replace("o", "")
            for fl, bit in FLAG_BITS.items():
                if fl in undef:
                    continue
                if (ref_flags >> bit) & 1 != (our_flags >> bit) & 1:
                    errs.append(f"{fl}f: ref {(ref_flags >> bit) & 1} ours {(our_flags >> bit) & 1}")
            fpu_lo, fpu_hi = FPU_OUT, FPU_OUT + 72
            def keep(a, esp):
                return not (x87 and fpu_lo <= a < fpu_hi) and not (esp <= a < esp + 4)
            mem_ref = {a: v for a, v in ref_changes.items() if keep(a, ref["esp"])}
            mem_our = {a: v for a, v in our_changes.items() if keep(a, ours["esp"])}
            if mem_ref != mem_our:
                bad = sorted(set(mem_ref) ^ set(mem_our) | {a for a in mem_ref if mem_our.get(a) != mem_ref[a]})
                a = bad[0]
                errs.append(f"mem @{a:#x}: ref {mem_ref.get(a)} ours {mem_our.get(a)} ({len(bad)} bytes)")
            if x87:
                def fpu_bytes(changes, a, k):
                    return bytes(changes.get(a + i, pristine[a + i - DATA_LO]) for i in range(k))
                rsw = struct.unpack("<H", fpu_bytes(ref_changes, FPU_OUT, 2))[0]
                osw = struct.unpack("<H", fpu_bytes(our_changes, FPU_OUT, 2))[0]
                swmask = 0x7D00 if m in X87_CC_DEFINED else 0x3800
                if (rsw & swmask) != (osw & swmask):
                    errs.append(f"fsw: ref {rsw:#06x} ours {osw:#06x}")
                top = (rsw >> 11) & 7
                depth = 8 - top if top else 0
                for k in range(depth):
                    a = struct.unpack("<d", fpu_bytes(ref_changes, FPU_OUT + 8 + 8 * k, 8))[0]
                    b = struct.unpack("<d", fpu_bytes(our_changes, FPU_OUT + 8 + 8 * k, 8))[0]
                    same = (a == b) or (a != a and b != b) or abs(a - b) <= 1e-12 * max(abs(a), abs(b))
                    if not same:
                        errs.append(f"st{k}: ref {a!r} ours {b!r}")
            if errs:
                fails[label].append("; ".join(errs[:4]))

    total = len(cases)
    print(f"cases {total}, trials run {ran}, reference faults skipped {skipped}, failing cases {len(fails)}")
    for label, errs in sorted(fails.items()):
        print(f"FAIL {label}: {len(errs)}/{args.trials}  e.g. {errs[0]}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

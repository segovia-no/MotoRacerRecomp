"""x86 (i386 + x87 + a little MMX) -> C translator.

One C function per discovered guest function. Guest registers live in the global `R`,
arithmetic flags are C locals (they never cross call boundaries in compiled MSVC code), and
x87 state is the global `F`. See docs/07-translator.md for the model.
"""
import re

from capstone import x86

from .discover import full_reg

REG32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx",
         "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

CTYPE = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
STYPE = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}
SIGN = {1: "0x80u", 2: "0x8000u", 4: "0x80000000u"}
BITS = {1: 8, 2: 16, 4: 32}

COND = {
    "o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf",
    "be": "(cf | zf)", "a": "(!cf && !zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf",
    "l": "(sf != of)", "ge": "(sf == of)", "le": "(zf || sf != of)", "g": "(!zf && sf == of)",
}

PREFIXES = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x9B}

# MSVC CRT jumps that resume execution in an *outer* active frame with esp/ebp restored:
# _JumpToContinuation (after a C++ catch block) and the SE-translator guard handler.
# See docs/09-ddraw-and-eh.md.
NONLOCAL_JUMPS = {0x4B283C, 0x4B2AFF}


# Static tables the 3D engine sizes for at most 480 screen rows; taller viewports overflow them
# into neighbouring globals (row 481 of the row-offset table overwrites the frame pitch at
# 0x62ebec) and every 3D triangle is culled. References are relocated to larger buffers in an
# unused part of the guest address space (runtime.h: EXT_LO). Entries: (lo, hi, new_lo, slack):
# an absolute address in [lo, hi) becomes new_lo + (addr - lo); an indexed operand ([reg + disp])
# may also start up to `slack` bytes below lo (the rasterizer reads rows y-1 and y-2 that way,
# while e.g. [0x62e460] on its own is an unrelated byte global). See docs/11-d3d-plan.md.
MAX_ROWS = 4096
RELOCATED_TABLES = [
    # per-row span-list heads: 480 dwords at 0x623bd8 (0x624358 is the next global)
    (0x623BD8, 0x624358, 0x00800010, 4),
    # per-row framebuffer offsets (y * pitch): 480 dwords at 0x62e468 (0x62ebe8 is the next global)
    (0x62E468, 0x62EBE8, 0x00800010 + 4 * MAX_ROWS + 16, 8),
]


def relocate(value, indexed=False):
    for lo, hi, new, slack in RELOCATED_TABLES:
        if (lo - slack if indexed else lo) <= value < hi:
            return (new + value - lo) & 0xFFFFFFFF
    return value


# Runtime callbacks run just before the instruction at a VA (runtime/src/game_hooks.cpp).
GAME_HOOKS = {
    # Options load: "ScreenMode" (only the size class 0..2) has just been mapped to a mode area
    # at 0x4dc0cc. The runtime restores a remembered mode above 640x480 here.
    0x42B0A3: "game_hook_screen_mode_loaded",
    # Options page: sprintf(label, "%d*%d", w, h) for SCREEN MODE (two call sites); the runtime
    # shows the 16:9 band size while 16/9 MODE is on.
    0x443BD9: "game_hook_mode_label",
    0x4447E8: "game_hook_mode_label",
    # Track config load: the per-detail-level table at 0x51d1b8 (LenBPlane, FlatZ, LOD zones...)
    # has just been read; the runtime extends draw and LOD distances (DrawDistance setting).
    0x4287EA: "game_hook_detail_loaded",
    # Options load: "InputDevice" (CONTROLLER) was just read into 0x4dc11c; on a first run with a
    # gamepad connected the runtime selects JOYSTICK before the game initialises its input device.
    0x42B37D: "game_hook_input_loaded",
    # Opponent bike LOD (0x45f160): ST(0) = distance / FOV, about to be compared with the
    # hardcoded thresholds 1000 / 2500 / 3000 (full model / reduced models / sprite).
    0x45F1E0: "game_hook_bike_lod",
    # Viewport setup (0x499b80), 16/9 MODE: eax = the rounded band height (W/16*9 scaled by H/W*4/3)
    # just returned by ftol, on both paths; the runtime makes it W*10/16 for its 16:10 modes.
    0x499C8D: "game_hook_band",
    0x499CF8: "game_hook_band",
    # Race HUD, speedometer (0x484810): runs every race frame just before the "ShowCounter" test;
    # the runtime knows a race is on (gamepad Back cycles the speedometer like F7).
    0x484833: "game_hook_counter",
    # Player bike lean (0x469c60): [esp+0x10] is the frame-time divisor of the lean, max(smoothed
    # frame time, 20 ms); the runtime uses this frame's time instead (correct at high frame rates).
    0x46AC41: "game_hook_lean",
    # Options page (11) init: the in-game Recomp settings page starts closed (recomp_menu.cpp).
    0x443000: "game_hook_options_init",
    # Menu button sprite just unpacked (0x43eb00 -> eax = 16-bit pixels, ebx = sprite object): the
    # Info button's label becomes "Recomp" (recomp_menu.cpp).
    0x40BF5E: "game_hook_button_unpacked",
    # Options page: the Accept / Info / Data disk button registrations (0x40bef0, nav mask at
    # [esp+8]); the runtime makes left/right move along the row (the game's masks skip buttons).
    0x444405: "game_hook_options_button_nav",
    0x44443B: "game_hook_options_button_nav",
    0x44448A: "game_hook_options_button_nav",
    # Main menu page (3) init (dev unlock override) and its six button registrations (0x40bef0, nav
    # mask at [esp+8]): the unlocked Pocket / Reverse toggles get keyboard navigation (recomp_menu.cpp).
    0x4422C0: "game_hook_main_menu_init",
    0x442522: "game_hook_main_menu_button_nav",
    0x44254D: "game_hook_main_menu_button_nav",
    0x442572: "game_hook_main_menu_button_nav",
    0x44259D: "game_hook_main_menu_button_nav",
    0x4425F4: "game_hook_main_menu_button_nav",
    0x442640: "game_hook_main_menu_button_nav",
    # AI bike speed integration (0x467930, bikes with +8 = 0): eax = old speed + trunc(accel * dt *
    # 0.01) about to be stored; the runtime carries the fraction so it works above ~40 fps.
    0x467A37: "game_hook_ai_speed",
    # Race setup (0x47b542): the opponent count 0x542874 has just been chosen (23, 1 with OPPONENTS
    # off, 11 in one mode; -NbComp only applies to another start path); dev override.
    0x47B5AF: "game_hook_opponents",
    # Render-list flush 0x4af570: where both per-vertex loops end (check mode of the native loops).
    0x4AF9FE: "native_vertices_check",
}

# Guest functions with a native version (runtime/src/native_render.cpp, docs/24-native-render.md):
# f_<va> calls the native function, which returns 0 to run the translated body (kept as f_<va>_t).
# (0x419d60, the triangle emitter, was tried: no gain over the translated code, docs/24.)
NATIVE_FUNCS = {}

# Hooks that can redirect control: `if (hook()) goto target;` (target in the same function).
GAME_JUMP_HOOKS = {
    # Options page per-frame function 0x443200 (recomp_menu.cpp):
    # first row about to be drawn -> the Recomp page draws its own rows and the Back button, then
    # continues after the game's last button
    0x44329A: ("game_hook_options_rows", 0x44448F),
    # dispatch of the activated item (esi) -> the Recomp page handles its items
    0x444504: ("game_hook_options_action", 0x4449A2),
    # Info button (item 21, would open the DIAGNOSTIC page 13) -> opens the Recomp page
    0x444983: ("game_hook_options_info", 0x4449A2),
    # Render-list flush 0x4af570: native per-vertex loops (untextured / textured), continuing after them
    0x4AF89B: ("native_flat_vertices", 0x4AF9FE),
    0x4AF9AF: ("native_tex_vertices", 0x4AF9FE),
}


def is_fs0(op):
    kind, val, _ = op
    return kind == "mem" and val[4] == "fs" and val[0] is None and val[1] is None and val[3] == 0


def fn_name(va):
    return f"f_{va:08x}"


def imp_name(dll, name):
    dll = dll.split(".")[0].upper()
    name = name.replace("#", "ord")
    return f"imp_{dll}_{name}"


class Unsupported(Exception):
    pass


# ---------- register caching ----------
# Inside a function the guest registers eax..edi live in C locals (REGS_LOCAL in recomp.h), so the
# compiler keeps them in host registers: guest memory stores (char-typed, may alias anything) no
# longer force R to be reloaded. R is written back (SPILL) right before every call that can read or
# write registers (guest functions, imports, call_indirect, runtime helpers, hooks) and before every
# return, and read back (RELOAD) after the call. Calls to the helpers below never touch R.
PURE_CALLS = {
    "IC", "RD8", "RD16", "RD32", "RD64", "WR8", "WR16", "WR32", "WR64", "RDF32", "RDF64", "RDF80",
    "WRF32", "WRF64", "WRF80", "PUSH32", "PUSH16", "POP32", "POP16", "PARITY", "EFLAGS_PACK",
    "EFLAGS_UNPACK", "ST", "FPUSH", "FPOP", "FSTSW", "FCOM", "FXAM", "FPREM", "F_ROUND", "F_TO_I16",
    "F_TO_I32", "F_TO_I64", "FNSTENV", "FLDENV", "FM", "trunc", "if", "switch", "while", "sizeof",
}
REG_RE = re.compile(r"\bR\.(eax|ecx|edx|ebx|esp|ebp|esi|edi)\b")
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\(")
STACK_RE = re.compile(r"\b(PUSH32|PUSH16|POP32|POP16)\(")


def _wrap_calls(line):
    """Wrap each `name(args);` statement of a non-pure function in SPILL/RELOAD. Returns None if a
    non-pure call is not a whole statement."""
    out, pos = [], 0
    while True:
        m = CALL_RE.search(line, pos)
        if not m:
            out.append(line[pos:])
            return "".join(out)
        name = m.group(1)
        if name in PURE_CALLS:
            out.append(line[pos:m.end()])
            pos = m.end()
            continue
        depth, i = 1, m.end()
        while depth:
            if i >= len(line):
                return None
            depth += (line[i] == "(") - (line[i] == ")")
            i += 1
        before = line[:m.start()].rstrip()
        # a statement: starts the line or follows ';', '{' or '}' (or `h = ` / `v = ` assignment)
        assign = re.search(r"([;{}]|^)\s*[hv] = $", line[:m.start()])
        if not (assign or before == "" or before.endswith((";", "{", "}", ":"))):
            return None
        if i >= len(line) or line[i] != ";":
            return None
        start = assign.start() + len(assign.group(1)) if assign else m.start()
        out.append(line[pos:start])
        out.append(f"SPILL(); {line[start:i + 1]} RELOAD();")
        pos = i + 1


def cache_registers(out):
    body = []
    for ln in out:
        if ln.startswith("__LABEL_") or ln.lstrip().startswith("/*"):
            body.append(ln)
            continue
        ln = _wrap_calls(ln)
        if ln is None:
            return None
        ln = REG_RE.sub(r"\1", ln)
        ln = STACK_RE.sub(r"L\1(", ln)
        ln = ln.replace("return;", "SPILL(); return;")
        body.append(ln)
    return body


# ---------- x87 stack in locals ----------
# MSVC code keeps the x87 stack empty at calls and returns (except a float result in ST0), so the
# stack depth relative to function entry is known statically at every instruction. Where it is,
# ST(i) at depth d becomes the local x{d-1-i} (the compiler keeps these in host FP registers) and
# the status word the local fsw. The global stack F (top unchanged inside the function) is only used
# across calls: before a call the live locals are pushed onto it (X87 spill), after it the callee's
# net effect (its "delta": +1 for a float result, -1 for e.g. _ftol, which pops its argument) is
# applied and the values are read back. Deltas come from a pre-pass over all functions
# (Translator.analyze). A function keeps the global form if anything is not provable: inconsistent
# depths, underflow below entry, more than 8 values, unknown callee delta, an indirect call with
# values on the stack, or direct F.top / environment access.
X87_TOK = re.compile(r"\bST\((\d)\)|\bFPUSH\(|\bFPOP\(\)|\bSPILL\(\);|\bRELOAD\(\);|\bFSTSW\(\)|"
                     r"\bFCOM\(|\bF\.sw\b|\bFXAM\(\);|\bFPREM\(\);|\bf_([0-9a-f]{8})\(|\bcall_indirect\(|"
                     r"\breturn;|\bF\.(top|tag|st)\b|\bFNSTENV\(|\bFLDENV\(")
X87_USE = re.compile(r"\bST\(\d\)|\bFPUSH\(|\bFPOP\(\)|\bFSTSW\(\)|\bFCOM\(|\bF\.sw\b|\bFXAM\(|\bFPREM\(")


class X87Fail(Exception):
    pass


def x87_spill(d):
    s = "F.sw = (uint16_t)fsw;"
    if d:
        s += f" F.top = (F.top - {d}u) & 7u;" + "".join(
            f" F.st[(F.top + {i}u) & 7u] = x{d - 1 - i};" for i in range(d))
    return s


def x87_reload(d):
    s = "".join(f" x{d - 1 - i} = F.st[(F.top + {i}u) & 7u];" for i in range(d))
    if d:
        s += f" F.top = (F.top + {d}u) & 7u;"
    return s + " fsw = F.sw;"


def x87_walk(text, d, deltas, check, out=None, exits=None):
    """Simulate (and with `out`, rewrite) one piece of code starting at x87 depth d; returns the
    depth after it. check=True rejects anything the local form can't express."""
    pos = 0
    while True:
        m = X87_TOK.search(text, pos)
        if not m:
            if out is not None:
                out.append(text[pos:])
            return d
        if out is not None:
            out.append(text[pos:m.start()])
        tok, pos = m.group(0), m.end()
        if m.group(1) is not None:                                   # ST(i)
            i = int(m.group(1))
            if check and i >= d:
                raise X87Fail("underflow")
            if out is not None:
                out.append(f"x{d - 1 - i}")
        elif tok == "FPUSH(":
            depth, j = 1, pos
            while depth:
                if j >= len(text):
                    raise X87Fail("parse")
                depth += (text[j] == "(") - (text[j] == ")")
                j += 1
            inner = []
            if x87_walk(text[pos:j - 1], d, deltas, check, inner if out is not None else None) != d:
                raise X87Fail("nested push")
            if out is not None:
                out.append(f"x{d} = ({''.join(inner)})")
            d += 1
            if check and d > 8:
                raise X87Fail("overflow")
            pos = j
        elif tok == "FPOP()":
            if check and d < 1:
                raise X87Fail("underflow")
            d -= 1
            if out is not None:
                out.append("((void)0)")
        elif tok == "SPILL();":
            if out is not None:
                out.append(f"SPILL(); {x87_spill(d)}")
        elif tok == "RELOAD();":
            if out is not None:
                out.append(f"RELOAD();{x87_reload(d)}")
        elif tok == "FSTSW()":
            if out is not None:
                out.append(f"((uint16_t)((fsw & ~0x3800u) | (((F.top - {d}u) & 7u) << 11)))")
        elif tok == "FCOM(":
            if out is not None:
                out.append("FCOM_L(fsw, ")
        elif tok == "F.sw":
            if out is not None:
                out.append("fsw")
        elif tok in ("FXAM();", "FPREM();"):
            if out is not None:
                out.append(f"{{ {x87_spill(d)} {tok}{x87_reload(d)} }}")
        elif m.group(2) is not None:                                 # f_xxxxxxxx(
            delta = deltas.get(int(m.group(2), 16))
            if delta is None:
                if check:
                    raise X87Fail("unknown callee delta")
                delta = 0
            d += delta
            if check and (d < 0 or d > 8):
                raise X87Fail("call depth")
            if out is not None:
                out.append(tok)
        elif tok == "call_indirect(":
            if check and d != 0:
                raise X87Fail("indirect call with values on the stack")
            if out is not None:
                out.append(tok)
        elif tok == "return;":
            if exits is not None:
                exits.add(d)
            if out is not None:
                out.append(tok)
        else:                                                        # F.top / F.tag / F.st / env
            if check:
                raise X87Fail(tok)
            if out is not None:
                out.append(tok)


def x87_delta(ft, deltas):
    """Net x87 stack effect of a function (depth at its returns), None if not consistent."""
    spans = ft.spans(ft.out)
    depth_in = {ft.entry: 0}
    work = [ft.entry]
    exits = set()
    while work:
        va = work.pop()
        if va not in spans:
            continue
        a, b = spans[va]
        d = x87_walk("\n".join(ft.out[a:b]), depth_in[va], deltas, False, exits=exits)
        for t in ft.succ.get(va, ()):
            if t not in depth_in:
                depth_in[t] = d
                work.append(t)
            elif depth_in[t] != d:
                return None
    if len(exits) > 1:
        return None
    return exits.pop() if exits else 0


def x87_locals(ft, body):
    """Rewrite a register-cached body to keep the x87 stack in locals; (body, max depth) or None."""
    text = "\n".join(body)
    if not X87_USE.search(text):
        return None
    deltas = ft.tr.x87_deltas
    spans = ft.spans(body)
    depth_in = {ft.entry: 0}
    work = [ft.entry]
    try:
        while work:
            va = work.pop()
            if va not in spans:
                continue
            a, b = spans[va]
            d = x87_walk("\n".join(body[a:b]), depth_in[va], deltas, True)
            for t in ft.succ.get(va, ()):
                if t not in depth_in:
                    depth_in[t] = d
                    work.append(t)
                elif depth_in[t] != d:
                    raise X87Fail("inconsistent depth")
        out_body = list(body)
        max_d = 0
        for va, (a, b) in spans.items():
            seg = "\n".join(body[a:b])
            if va not in depth_in:
                if X87_USE.search(seg):
                    raise X87Fail("unreachable x87 code")
                d0 = 0
            else:
                d0 = depth_in[va]
            out = []
            x87_walk(seg, d0, deltas, True, out)
            new = "".join(out).split("\n")
            if len(new) != b - a:
                raise X87Fail("line count")
            out_body[a:b] = new
            max_d = max([max_d] + [int(x) + 1 for x in re.findall(r"\bx(\d)\b", "".join(out))])
        # code before the first label (e.g. `goto L_entry;`) has no x87 content
        return out_body, max_d
    except X87Fail as e:
        ft.tr.x87_fail[str(e)] = ft.tr.x87_fail.get(str(e), 0) + 1
        return None


class FunctionTranslator:
    def __init__(self, tr, entry, body):
        self.tr = tr
        self.d = tr.d
        self.img = tr.img
        self.entry = entry
        self.body = body              # sorted insn VAs
        self.body_set = set(body)
        self.out = []
        self.labels = set()
        self.cur = None
        self.uses_eh = False
        self.succ = {}                # insn va -> successor vas inside the function
        self.emitted = False

    # ---------- operands ----------
    def smc_field(self, insn, kind):
        off, size = (insn.imm_off, insn.imm_size) if kind == "imm" else (insn.disp_off, insn.disp_size)
        if not size:
            return None
        slot = insn.va + off
        if any(b in self.tr.smc_bytes for b in range(slot, slot + size)):
            return slot, size
        return None

    def imm(self, insn, value, size):
        smc = self.smc_field(insn, "imm")
        if smc:
            slot, isz = smc
            self.tr.smc_uses += 1
            if isz == size:
                return f"RD{BITS[isz]}(0x{slot:x}u)"
            return f"(({CTYPE[size]})({STYPE[isz]})RD{BITS[isz]}(0x{slot:x}u))"
        if size == 4:
            value = relocate(value & 0xFFFFFFFF)
        return f"0x{value & ((1 << (8 * size)) - 1):x}u"

    def addr(self, insn, mem):
        base, index, scale, disp, seg = mem
        parts = []
        if base:
            parts.append(f"R.{full_reg(base)}" if base in REG32 else self.reg_read(base))
        if index:
            parts.append(f"R.{index}*{scale}u" if scale != 1 else f"R.{index}")
        smc = self.smc_field(insn, "disp")
        if smc:
            slot, dsz = smc
            self.tr.smc_uses += 1
            parts.append(f"RD32(0x{slot:x}u)" if dsz == 4 else f"(uint32_t)(int8_t)RD8(0x{slot:x}u)")
        elif disp or not parts:
            parts.append(f"0x{relocate(disp & 0xFFFFFFFF, bool(base or index)):x}u")
        if seg == "fs":
            parts.insert(0, "R.fs_base")
        elif seg not in (None, "es", "ds", "ss", "cs"):
            raise Unsupported(f"segment {seg}")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    @staticmethod
    def reg_read(r):
        if r in REG32:
            return f"R.{r}"
        if r in REG16:
            return f"(uint16_t)R.{REG16[r]}"
        if r in REG8L:
            return f"(uint8_t)R.{REG8L[r]}"
        if r in REG8H:
            return f"(uint8_t)(R.{REG8H[r]} >> 8)"
        raise Unsupported(f"register {r}")

    @staticmethod
    def reg_write(r, v):
        if r in REG32:
            return f"R.{r} = {v};"
        if r in REG16:
            return f"R.{REG16[r]} = (R.{REG16[r]} & 0xffff0000u) | (uint16_t)({v});"
        if r in REG8L:
            return f"R.{REG8L[r]} = (R.{REG8L[r]} & 0xffffff00u) | (uint8_t)({v});"
        if r in REG8H:
            return f"R.{REG8H[r]} = (R.{REG8H[r]} & 0xffff00ffu) | ((uint32_t)(uint8_t)({v}) << 8);"
        raise Unsupported(f"register {r}")

    def rd(self, insn, k, size=None):
        kind, val, osz = insn.ops[k]
        size = size or osz
        if kind == "reg":
            return self.reg_read(val)
        if kind == "imm":
            return self.imm(insn, val, size)
        return f"RD{BITS[size]}({self.addr(insn, val)})"

    def wr(self, insn, k, v, size=None):
        kind, val, osz = insn.ops[k]
        size = size or osz
        if kind == "reg":
            return self.reg_write(val, v)
        if kind == "mem":
            return f"WR{BITS[size]}({self.addr(insn, val)}, ({CTYPE[size]})({v}));"
        raise Unsupported("write to immediate")

    def op_size(self, insn, k=0):
        return insn.ops[k][2]

    # ---------- emission helpers ----------
    def emit(self, s):
        self.out.append("    " + s)

    def goto(self, target):
        if target in self.body_set:
            self.labels.add(target)
            if self.cur is not None:
                self.succ.setdefault(self.cur.va, set()).add(target)
            return f"goto L_{target:08x};"
        if target in self.d.entries:
            return f"{{ {fn_name(target)}(); return; }}"
        return f"{{ x86_unimpl(0x{self.cur.va:x}u, \"branch to undecoded 0x{target:x}\"); return; }}"

    def flags_szp(self, r, size):
        return (f"zf = ({r}) == 0; sf = (({r}) & {SIGN[size]}) != 0; pf = PARITY({r});")

    # ---------- main ----------
    def translate(self):
        if not self.emitted:
            self.emit_body()
        return self.assemble()

    def emit_body(self):
        tr = self.tr
        self.emitted = True
        # Bodies are emitted in address order; a function may own code *below* its entry
        # (e.g. an EH funclet that tail-jumps into a destructor nobody else reaches).
        if self.body and self.body[0] != self.entry:
            self.labels.add(self.entry)
            self.out.append(f"    goto L_{self.entry:08x};")
        for i, va in enumerate(self.body):
            insn = self.d.insns[va]
            self.cur = insn
            nxt = self.body[i + 1] if i + 1 < len(self.body) else None
            self.out.append(f"__LABEL_{va:08x}__")
            self.out.append(f"    /* {va:08x}: {insn.mnem} {insn.op_str} */")
            self.out.append(f"    IC(0x{self.entry:x});")
            if va in GAME_HOOKS:
                self.emit(f"{GAME_HOOKS[va]}();")
            if va in GAME_JUMP_HOOKS:
                name, target = GAME_JUMP_HOOKS[va]
                self.emit(f"{{ int h; h = {name}(); if (h) {self.goto(target)} }}")
            try:
                falls = self.insn(insn)
            except Unsupported as e:
                tr.unsupported[insn.mnem] = tr.unsupported.get(insn.mnem, 0) + 1
                self.emit(f'x86_unimpl(0x{va:x}u, "{insn.mnem} {insn.op_str}: {e}"); return;')
                falls = False
            if falls:
                end = insn.end
                if end in self.body_set:
                    self.succ.setdefault(va, set()).add(end)
                if end != nxt:
                    if end in self.body_set or end in self.d.entries:
                        self.emit(self.goto(end))
                    else:
                        self.emit("return; /* falls off decoded code */")
        self.cur = None

    def spans(self, lines):
        """insn va -> (first, last+1) line indexes of its code in `lines` (same layout as self.out)."""
        out, cur = {}, None
        for i, ln in enumerate(lines):
            if ln.startswith("__LABEL_"):
                cur = int(ln[8:16], 16)
                out[cur] = [i + 1, i + 1]
            elif cur is not None:
                out[cur][1] = i + 1
        return out

    def assemble(self):
        body = cache_registers(self.out) if self.tr.regcache and not self.uses_eh else None
        cached = body is not None
        if not cached:
            body = self.out
            self.tr.uncached += 1
        fpu_decl = None
        if cached and self.tr.x87_locals:
            res = x87_locals(self, body)
            if res is not None:
                body, depth = res
                self.tr.x87_local += 1
                fpu_decl = ("    fpu_t " + ", ".join(f"x{i}" for i in range(depth)) + "; " if depth else "    ") + \
                    "uint32_t fsw = F.sw; (void)fsw;"
        native = NATIVE_FUNCS.get(self.entry)
        name = fn_name(self.entry) + ("_t" if native else "")
        lines = [f"void {name}(void) {{",
                 "    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0, af = 0;",
                 "    (void)cf; (void)zf; (void)sf; (void)of; (void)pf; (void)af;"]
        lines.append("    MEM_LOCAL();")
        if cached:
            lines.append("    REGS_LOCAL();")
        if fpu_decl:
            lines.append(fpu_decl)
        if self.uses_eh:
            lines.append("    jmp_buf eh_jb;")
        for ln in body:
            if ln.startswith("__LABEL_"):
                va = int(ln[8:16], 16)
                if va in self.labels:
                    lines.append(f"L_{va:08x}:;")
                continue
            lines.append(ln)
        lines.append("}")
        if native:
            lines.append(f"void {fn_name(self.entry)}(void) {{ if (!{native}()) {name}(); }}")
        return "\n".join(lines)

    # Returns True if control may fall through to insn.end.
    def insn(self, insn):
        m = insn.mnem
        e = self.emit
        if m.startswith("f") and m != "fs":
            return self.x87(insn)
        if m in ("movq", "movd", "punpcklwd", "emms"):
            return self.mmx(insn)
        if m.startswith("rep") or m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
                                         "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd",
                                         "lodsb", "lodsw", "lodsd"):
            return self.string(insn)
        h = getattr(self, "i_" + m, None)
        if h is not None:
            return h(insn)
        if m.startswith("j"):
            return self.jcc(insn)
        if m.startswith("set"):
            e(self.wr(insn, 0, COND[m[3:]] if m[3:] in COND else self.cond_alias(m[3:]), 1))
            return True
        raise Unsupported("no handler")

    def cond_alias(self, c):
        alias = {"z": "e", "nz": "ne", "c": "b", "nc": "ae", "nae": "b", "nb": "ae",
                 "na": "be", "nbe": "a", "nge": "l", "nl": "ge", "ng": "le", "nle": "g",
                 "pe": "p", "po": "np"}
        if c in alias:
            return COND[alias[c]]
        raise Unsupported(f"condition {c}")

    # ---------- data movement ----------
    def i_mov(self, insn):
        size = self.op_size(insn, 0)
        self.emit(self.wr(insn, 0, self.rd(insn, 1, size), size))
        if is_fs0(insn.ops[0]):
            if insn.ops[1] == ("reg", "esp", 4):
                self.uses_eh = True
                self.emit("eh_register(R.esp, &eh_jb);")
                self.emit("if (RECOMP_SETJMP(eh_jb)) { call_indirect(eh_resume_va); return; }")
            else:
                self.emit(f"eh_unlink({self.rd(insn, 1, 4)});")
        return True

    def i_movzx(self, insn):
        self.emit(self.wr(insn, 0, f"(uint32_t){self.rd(insn, 1)}"))
        return True

    def i_movsx(self, insn):
        dsz, ssz = self.op_size(insn, 0), self.op_size(insn, 1)
        self.emit(self.wr(insn, 0, f"({CTYPE[dsz]})({STYPE[dsz]})({STYPE[ssz]}){self.rd(insn, 1)}"))
        return True

    def i_lea(self, insn):
        self.emit(self.wr(insn, 0, self.addr(insn, insn.ops[1][1][:4] + (None,))))
        return True

    def i_xchg(self, insn):
        size = self.op_size(insn, 0)
        self.emit(f"{{ {CTYPE[size]} t0 = {self.rd(insn, 0)}, t1 = {self.rd(insn, 1)}; "
                  f"{self.wr(insn, 0, 't1')} {self.wr(insn, 1, 't0')} }}")
        return True

    def i_push(self, insn):
        kind, val, size = insn.ops[0]
        if kind == "imm":
            self.emit(f"PUSH32({self.imm(insn, val, 4)});")
        elif size == 2:
            self.emit(f"PUSH16({self.rd(insn, 0)});")
        else:
            self.emit(f"PUSH32({self.rd(insn, 0)});")
        return True

    def i_pop(self, insn):
        size = self.op_size(insn, 0)
        if size == 2:
            self.emit(f"{{ uint16_t v = POP16(); {self.wr(insn, 0, 'v')} }}")
        else:
            # pop [esp+x] computes its address after the increment: POP32 runs first.
            self.emit(f"{{ uint32_t v = POP32(); {self.wr(insn, 0, 'v')} }}")
            if is_fs0(insn.ops[0]):
                self.emit("eh_unlink(RD32(R.fs_base));")
        return True

    def i_pushal(self, insn):
        self.emit("{ uint32_t s = R.esp; PUSH32(R.eax); PUSH32(R.ecx); PUSH32(R.edx); PUSH32(R.ebx);"
                  " PUSH32(s); PUSH32(R.ebp); PUSH32(R.esi); PUSH32(R.edi); }")
        return True

    def i_popal(self, insn):
        self.emit("{ R.edi = POP32(); R.esi = POP32(); R.ebp = POP32(); R.esp += 4;"
                  " R.ebx = POP32(); R.edx = POP32(); R.ecx = POP32(); R.eax = POP32(); }")
        return True

    def i_pushfd(self, insn):
        self.emit("PUSH32(EFLAGS_PACK(cf, pf, af, zf, sf, of));")
        return True

    def i_pushf(self, insn):
        self.emit("PUSH16((uint16_t)EFLAGS_PACK(cf, pf, af, zf, sf, of));")
        return True

    def i_popfd(self, insn):
        self.emit("EFLAGS_UNPACK(POP32());")
        return True

    def i_popf(self, insn):
        self.emit("EFLAGS_UNPACK(POP16());")
        return True

    def i_lahf(self, insn):
        self.emit(self.reg_write("ah", "(sf << 7) | (zf << 6) | (af << 4) | (pf << 2) | 2u | cf"))
        return True

    def i_sahf(self, insn):
        self.emit("{ uint32_t h = (R.eax >> 8) & 0xff; cf = h & 1; pf = (h >> 2) & 1; af = (h >> 4) & 1;"
                  " zf = (h >> 6) & 1; sf = (h >> 7) & 1; }")
        return True

    def i_cdq(self, insn):
        self.emit("R.edx = (uint32_t)((int32_t)R.eax >> 31);")
        return True

    def i_cwde(self, insn):
        self.emit("R.eax = (uint32_t)(int32_t)(int16_t)R.eax;")
        return True

    def i_cbw(self, insn):
        self.emit(self.reg_write("ax", "(uint16_t)(int16_t)(int8_t)R.eax"))
        return True

    def i_cwd(self, insn):
        self.emit(self.reg_write("dx", "((int16_t)R.eax < 0) ? 0xffffu : 0u"))
        return True

    def i_xlatb(self, insn):
        self.emit(self.reg_write("al", "RD8(R.ebx + (uint8_t)R.eax)"))
        return True

    def i_leave(self, insn):
        self.emit("R.esp = R.ebp; R.ebp = POP32();")
        return True

    # ---------- arithmetic ----------
    def binop(self, insn, kind):
        size = self.op_size(insn, 0)
        T, sb = CTYPE[size], SIGN[size]
        a = self.rd(insn, 0, size)
        b = self.rd(insn, 1, size)
        s = [f"{{ {T} a = {a}, b = {b}, r;"]
        if kind == "add":
            s.append(f"r = ({T})(a + b); cf = r < a; of = ((a ^ r) & (b ^ r) & {sb}) != 0;"
                     " af = ((a ^ b ^ r) >> 4) & 1;")
        elif kind == "adc":
            s.append(f"uint64_t t = (uint64_t)a + b + cf; r = ({T})t; cf = (t >> {BITS[size]}) & 1;"
                     f" of = ((a ^ r) & (b ^ r) & {sb}) != 0; af = ((a ^ b ^ r) >> 4) & 1;")
        elif kind in ("sub", "cmp"):
            s.append(f"r = ({T})(a - b); cf = a < b; of = ((a ^ b) & (a ^ r) & {sb}) != 0;"
                     " af = ((a ^ b ^ r) >> 4) & 1;")
        elif kind == "sbb":
            s.append(f"uint32_t c = cf; r = ({T})(a - b - c); cf = (uint64_t)a < (uint64_t)b + c;"
                     f" of = ((a ^ b) & (a ^ r) & {sb}) != 0; af = ((a ^ b ^ r) >> 4) & 1;")
        elif kind in ("and", "test"):
            s.append(f"r = a & b; cf = 0; of = 0; af = 0;")
        elif kind == "or":
            s.append(f"r = a | b; cf = 0; of = 0; af = 0;")
        elif kind == "xor":
            s.append(f"r = a ^ b; cf = 0; of = 0; af = 0;")
        s.append(self.flags_szp("r", size))
        if kind not in ("cmp", "test"):
            s.append(self.wr(insn, 0, "r", size))
        s.append("}")
        self.emit(" ".join(s))
        return True

    def i_add(self, insn): return self.binop(insn, "add")
    def i_adc(self, insn): return self.binop(insn, "adc")
    def i_sub(self, insn): return self.binop(insn, "sub")
    def i_sbb(self, insn): return self.binop(insn, "sbb")
    def i_cmp(self, insn): return self.binop(insn, "cmp")
    def i_and(self, insn): return self.binop(insn, "and")
    def i_or(self, insn): return self.binop(insn, "or")
    def i_xor(self, insn):
        k0, v0, _ = insn.ops[0]
        if insn.ops[0] == insn.ops[1] and k0 == "reg":
            self.emit(f"{self.reg_write(v0, '0')} cf = 0; of = 0; af = 0; zf = 1; sf = 0; pf = 1;")
            return True
        return self.binop(insn, "xor")
    def i_test(self, insn): return self.binop(insn, "test")

    def i_inc(self, insn):
        size = self.op_size(insn)
        T = CTYPE[size]
        self.emit(f"{{ {T} a = {self.rd(insn, 0)}, r = ({T})(a + 1); of = r == {SIGN[size]};"
                  f" af = (r & 0xf) == 0; {self.flags_szp('r', size)} {self.wr(insn, 0, 'r')} }}")
        return True

    def i_dec(self, insn):
        size = self.op_size(insn)
        T = CTYPE[size]
        self.emit(f"{{ {T} a = {self.rd(insn, 0)}, r = ({T})(a - 1); of = a == {SIGN[size]};"
                  f" af = (a & 0xf) == 0; {self.flags_szp('r', size)} {self.wr(insn, 0, 'r')} }}")
        return True

    def i_neg(self, insn):
        size = self.op_size(insn)
        T = CTYPE[size]
        self.emit(f"{{ {T} a = {self.rd(insn, 0)}, r = ({T})(0 - a); cf = a != 0; of = a == {SIGN[size]};"
                  f" af = (a & 0xf) != 0; {self.flags_szp('r', size)} {self.wr(insn, 0, 'r')} }}")
        return True

    def i_not(self, insn):
        size = self.op_size(insn)
        self.emit(self.wr(insn, 0, f"~{self.rd(insn, 0)}"))
        return True

    def shift(self, insn, kind):
        size = self.op_size(insn, 0)
        T, n, sb = CTYPE[size], BITS[size], SIGN[size]
        a = self.rd(insn, 0)
        cnt = "1u" if len(insn.ops) == 1 else self.rd(insn, 1, 1)
        s = [f"{{ {T} a = {a}; uint32_t c = ({cnt}) & 31u; if (c) {{ {T} r;"]
        if kind == "shl":
            s.append(f"r = ({T})((uint32_t)a << c); cf = c <= {n} ? (((uint32_t)a >> ({n} - c)) & 1) : 0;"
                     f" of = ((r & {sb}) != 0) ^ cf;")
        elif kind == "shr":
            s.append(f"r = ({T})((uint32_t)a >> c); cf = ((uint32_t)a >> (c - 1)) & 1; of = (a & {sb}) != 0;")
        elif kind == "sar":
            s.append(f"r = ({T})((int32_t)({STYPE[size]})a >> (c > {n - 1} ? {n - 1} : c));"
                     f" cf = ((int32_t)({STYPE[size]})a >> (c - 1 > {n - 1} ? {n - 1} : c - 1)) & 1; of = 0;")
        elif kind == "rol":
            s.append(f"uint32_t k = c % {n}; r = k ? ({T})((a << k) | (a >> ({n} - k))) : a; cf = r & 1;"
                     f" of = ((r & {sb}) != 0) ^ cf;")
            s.append(f"{self.wr(insn, 0, 'r')} }} }}")
            self.emit(" ".join(s))
            return True
        elif kind == "ror":
            s.append(f"uint32_t k = c % {n}; r = k ? ({T})((a >> k) | (a << ({n} - k))) : a;"
                     f" cf = (r & {sb}) != 0; of = ((r >> {n - 1}) ^ (r >> {n - 2})) & 1;")
            s.append(f"{self.wr(insn, 0, 'r')} }} }}")
            self.emit(" ".join(s))
            return True
        elif kind == "rcr":
            if cnt != "1u" and not (insn.ops[1][0] == "imm" and insn.ops[1][1] == 1):
                raise Unsupported("rcr count != 1")
            s.append(f"of = ((a & {sb}) != 0) ^ cf; r = ({T})((a >> 1) | ((uint32_t)cf << {n - 1})); cf = a & 1;")
            s.append(f"{self.wr(insn, 0, 'r')} }} }}")
            self.emit(" ".join(s))
            return True
        s.append(f"{self.flags_szp('r', size)} {self.wr(insn, 0, 'r')} }} }}")
        self.emit(" ".join(s))
        return True

    def i_shl(self, insn): return self.shift(insn, "shl")
    def i_sal(self, insn): return self.shift(insn, "shl")
    def i_shr(self, insn): return self.shift(insn, "shr")
    def i_sar(self, insn): return self.shift(insn, "sar")
    def i_rol(self, insn): return self.shift(insn, "rol")
    def i_ror(self, insn): return self.shift(insn, "ror")
    def i_rcr(self, insn): return self.shift(insn, "rcr")

    def i_shld(self, insn):
        if self.op_size(insn) != 4:
            raise Unsupported("16-bit shld")
        self.emit(f"{{ uint32_t a = {self.rd(insn, 0)}, b = {self.rd(insn, 1)}, c = ({self.rd(insn, 2, 1)}) & 31u;"
                  f" if (c) {{ uint32_t r = (a << c) | (b >> (32 - c)); cf = (a >> (32 - c)) & 1;"
                  f" of = ((r ^ a) >> 31) & 1; {self.flags_szp('r', 4)} {self.wr(insn, 0, 'r')} }} }}")
        return True

    def i_shrd(self, insn):
        if self.op_size(insn) != 4:
            raise Unsupported("16-bit shrd")
        self.emit(f"{{ uint32_t a = {self.rd(insn, 0)}, b = {self.rd(insn, 1)}, c = ({self.rd(insn, 2, 1)}) & 31u;"
                  f" if (c) {{ uint32_t r = (a >> c) | (b << (32 - c)); cf = (a >> (c - 1)) & 1;"
                  f" of = ((r ^ a) >> 31) & 1; {self.flags_szp('r', 4)} {self.wr(insn, 0, 'r')} }} }}")
        return True

    def i_mul(self, insn):
        size = self.op_size(insn)
        v = self.rd(insn, 0)
        if size == 4:
            self.emit(f"{{ uint64_t t = (uint64_t)R.eax * {v}; R.eax = (uint32_t)t; R.edx = (uint32_t)(t >> 32);"
                      " cf = of = R.edx != 0; }")
        elif size == 2:
            self.emit(f"{{ uint32_t t = (uint32_t)(uint16_t)R.eax * {v}; {self.reg_write('ax', 't')}"
                      f" {self.reg_write('dx', 't >> 16')} cf = of = (t >> 16) != 0; }}")
        else:
            self.emit(f"{{ uint32_t t = (uint32_t)(uint8_t)R.eax * {v}; {self.reg_write('ax', 't')}"
                      " cf = of = (t >> 8) != 0; }")
        return True

    def i_imul(self, insn):
        size = self.op_size(insn)
        S = STYPE[size]
        if len(insn.ops) == 1:
            v = self.rd(insn, 0)
            if size == 4:
                self.emit(f"{{ int64_t t = (int64_t)(int32_t)R.eax * (int32_t){v}; R.eax = (uint32_t)t;"
                          " R.edx = (uint32_t)((uint64_t)t >> 32); cf = of = t != (int32_t)t; }")
            elif size == 2:
                self.emit(f"{{ int32_t t = (int32_t)(int16_t)R.eax * (int16_t){v}; {self.reg_write('ax', 't')}"
                          f" {self.reg_write('dx', '(uint32_t)t >> 16')} cf = of = t != (int16_t)t; }}")
            else:
                self.emit(f"{{ int32_t t = (int32_t)(int8_t)R.eax * (int8_t){v}; {self.reg_write('ax', 't')}"
                          " cf = of = t != (int8_t)t; }")
            return True
        if len(insn.ops) == 2:
            a, b = self.rd(insn, 0), self.rd(insn, 1, size)
        else:
            a, b = self.rd(insn, 1), self.rd(insn, 2, size)
        self.emit(f"{{ int64_t t = (int64_t)({S}){a} * ({S}){b}; cf = of = t != ({S})t;"
                  f" {self.wr(insn, 0, f'({CTYPE[size]})t')} }}")
        return True

    def i_div(self, insn):
        size = self.op_size(insn)
        v = self.rd(insn, 0)
        va = insn.va
        if size == 4:
            self.emit(f"{{ uint32_t d = {v}; uint64_t n = ((uint64_t)R.edx << 32) | R.eax;"
                      f" if (!d || n / d > 0xffffffffu) {{ x86_div_fault(0x{va:x}u); return; }}"
                      " R.eax = (uint32_t)(n / d); R.edx = (uint32_t)(n % d); }")
        elif size == 2:
            self.emit(f"{{ uint32_t d = {v}; uint32_t n = ((R.edx & 0xffff) << 16) | (R.eax & 0xffff);"
                      f" if (!d || n / d > 0xffffu) {{ x86_div_fault(0x{va:x}u); return; }}"
                      f" {self.reg_write('ax', 'n / d')} {self.reg_write('dx', 'n % d')} }}")
        else:
            self.emit(f"{{ uint32_t d = {v}; uint32_t n = R.eax & 0xffff;"
                      f" if (!d || n / d > 0xffu) {{ x86_div_fault(0x{va:x}u); return; }}"
                      f" {self.reg_write('al', 'n / d')} {self.reg_write('ah', 'n % d')} }}")
        return True

    def i_idiv(self, insn):
        size = self.op_size(insn)
        v = self.rd(insn, 0)
        va = insn.va
        if size == 4:
            self.emit(f"{{ int64_t d = (int32_t){v}; int64_t n = (int64_t)(((uint64_t)R.edx << 32) | R.eax);"
                      f" if (!d || (n == INT64_MIN && d == -1)) {{ x86_div_fault(0x{va:x}u); return; }}"
                      f" int64_t q = n / d; if (q != (int32_t)q) {{ x86_div_fault(0x{va:x}u); return; }}"
                      " R.eax = (uint32_t)q; R.edx = (uint32_t)(n % d); }")
        elif size == 2:
            self.emit(f"{{ int32_t d = (int16_t){v}; int32_t n = (int32_t)(((R.edx & 0xffff) << 16) | (R.eax & 0xffff));"
                      f" if (!d) {{ x86_div_fault(0x{va:x}u); return; }} int32_t q = n / d;"
                      f" if (q != (int16_t)q) {{ x86_div_fault(0x{va:x}u); return; }}"
                      f" {self.reg_write('ax', 'q')} {self.reg_write('dx', 'n % d')} }}")
        else:
            self.emit(f"{{ int32_t d = (int8_t){v}; int32_t n = (int16_t)R.eax;"
                      f" if (!d) {{ x86_div_fault(0x{va:x}u); return; }} int32_t q = n / d;"
                      f" if (q != (int8_t)q) {{ x86_div_fault(0x{va:x}u); return; }}"
                      f" {self.reg_write('al', 'q')} {self.reg_write('ah', 'n % d')} }}")
        return True

    # ---------- misc ----------
    def i_nop(self, insn): return True
    def i_wait(self, insn): return True
    def i_cld(self, insn): self.emit("R.df = 0;"); return True
    def i_std(self, insn): self.emit("R.df = 1;"); return True
    def i_stc(self, insn): self.emit("cf = 1;"); return True
    def i_clc(self, insn): self.emit("cf = 0;"); return True
    def i_cmc(self, insn): self.emit("cf ^= 1;"); return True
    def i_cli(self, insn): return True
    def i_sti(self, insn): return True
    def i_cpuid(self, insn): self.emit("x86_cpuid();"); return True
    def i_rdtsc(self, insn): self.emit("x86_rdtsc();"); return True

    def i_in(self, insn):
        size = self.op_size(insn, 0)
        port = self.rd(insn, 1, 2) if insn.ops[1][0] == "reg" else f"0x{insn.ops[1][1]:x}u"
        self.emit(f"{{ uint32_t v; v = x86_in({port}, {size}); {self.wr(insn, 0, 'v')} }}")
        return True

    def i_out(self, insn):
        size = self.op_size(insn, 1)
        port = self.rd(insn, 0, 2) if insn.ops[0][0] == "reg" else f"0x{insn.ops[0][1]:x}u"
        self.emit(f"x86_out({port}, {self.rd(insn, 1)}, {size});")
        return True

    def i_int(self, insn):
        self.emit(f"x86_int(0x{insn.va:x}u, {insn.ops[0][1]}); return;")
        return False

    def i_int3(self, insn):
        self.emit(f"x86_int(0x{insn.va:x}u, 3); return;")
        return False

    def i_hlt(self, insn):
        self.emit(f"x86_unimpl(0x{insn.va:x}u, \"hlt\"); return;")
        return False

    # ---------- control flow ----------
    def jcc(self, insn):
        m = insn.mnem
        target = insn.ops[0][1]
        if m == "jecxz":
            cond = "R.ecx == 0"
        elif m == "jcxz":
            cond = "(R.ecx & 0xffff) == 0"
        elif m == "loop":
            self.emit(f"if (--R.ecx != 0) {self.goto(target)}")
            return True
        else:
            c = m[1:]
            cond = COND[c] if c in COND else self.cond_alias(c)
        self.emit(f"if ({cond}) {self.goto(target)}")
        return True

    def i_jmp(self, insn):
        kind, val, _ = insn.ops[0]
        if kind == "imm":
            self.emit(self.goto(val))
            return False
        if kind == "mem" and val[0] is None and val[1] is None and val[3] in self.img.imports:
            dll, name = self.img.imports[val[3]]
            self.emit(f"{self.tr.use_import(dll, name)}(); return;")
            return False
        jt = self.d.tables.get(insn.va)
        if jt is not None:
            cases = " ".join(f"case 0x{t:x}u: {self.goto(t)}" for t in dict.fromkeys(jt.targets))
            self.emit(f"switch ({self.rd(insn, 0, 4)}) {{ {cases} default: "
                      f"x86_unimpl(0x{insn.va:x}u, \"jump table miss\"); return; }}")
            return False
        if insn.va in NONLOCAL_JUMPS:
            self.emit(f"nonlocal_jump({self.rd(insn, 0, 4)}); return;")
            return False
        self.emit(f"call_indirect({self.rd(insn, 0, 4)}); return;")
        return False

    def i_call(self, insn):
        kind, val, _ = insn.ops[0]
        ret = f"PUSH32(0x{insn.end:x}u);"
        if kind == "imm":
            if val in self.d.entries:
                self.emit(f"{ret} {fn_name(val)}();")
            else:
                self.emit(f"{ret} x86_unimpl(0x{insn.va:x}u, \"call to undecoded 0x{val:x}\"); return;")
                return False
        elif kind == "mem" and val[0] is None and val[1] is None and val[3] in self.img.imports:
            dll, name = self.img.imports[val[3]]
            self.emit(f"{ret} {self.tr.use_import(dll, name)}();")
        else:
            # Evaluate the target before pushing: `call [esp+8]` must see the old esp.
            self.emit(f"{{ uint32_t t = {self.rd(insn, 0, 4)}; {ret} call_indirect(t); }}")
        if insn.va in self.d.noreturn_calls:
            self.emit("return; /* noreturn */")
            return False
        return True

    def i_ret(self, insn):
        n = insn.ops[0][1] if insn.ops else 0
        self.emit(f"R.esp += {4 + n}u; return;")
        return False

    # ---------- string ops ----------
    def string(self, insn):
        m = insn.mnem
        rep = 0
        if m.startswith("repne "):
            rep, m = 2, m[6:]
        elif m.startswith("repe ") or m.startswith("rep "):
            rep, m = 1, m.split(" ", 1)[1]
        size = {"b": 1, "w": 2, "d": 4}[m[-1]]
        op = m[:-1]
        flags = "&cf, &zf, &sf, &of, &pf, &af"
        if op == "movs":
            self.emit(f"REP_MOVS({size}, {rep});")
        elif op == "stos":
            self.emit(f"REP_STOS({size}, {rep});")
        elif op == "scas":
            self.emit(f"REP_SCAS({size}, {rep}, {flags});")
        elif op == "cmps":
            self.emit(f"REP_CMPS({size}, {rep}, {flags});")
        elif op == "lods" and rep == 0:
            reg = {1: "al", 2: "ax", 4: "eax"}[size]
            step = f"(R.df ? -{size} : {size})"
            self.emit(f"{self.reg_write(reg, f'RD{BITS[size]}(R.esi)')} R.esi += {step};")
        else:
            raise Unsupported("string op")
        return True

    # ---------- MMX ----------
    def mmx(self, insn):
        m = insn.mnem
        if m == "emms":
            return True

        def mm(k):
            kind, val, _ = insn.ops[k]
            return int(val[2:]) if kind == "reg" and val.startswith("mm") else None
        if m == "movq":
            d, s = mm(0), mm(1)
            if d is not None and s is not None:
                self.emit(f"F.mm[{d}] = F.mm[{s}];")
            elif d is not None:
                self.emit(f"F.mm[{d}] = RD64({self.addr(insn, insn.ops[1][1])});")
            else:
                self.emit(f"WR64({self.addr(insn, insn.ops[0][1])}, F.mm[{s}]);")
        elif m == "movd":
            d, s = mm(0), mm(1)
            if d is not None:
                self.emit(f"F.mm[{d}] = (uint64_t){self.rd(insn, 1, 4)};")
            else:
                self.emit(self.wr(insn, 0, f"(uint32_t)F.mm[{s}]", 4))
        elif m == "punpcklwd":
            d, s = mm(0), mm(1)
            src = f"F.mm[{s}]" if s is not None else f"RD64({self.addr(insn, insn.ops[1][1])})"
            self.emit(f"{{ uint64_t a = F.mm[{d}], b = {src};"
                      " F.mm[%d] = (a & 0xffffu) | ((b & 0xffffu) << 16) | ((a & 0xffff0000u) << 16)"
                      " | ((b & 0xffff0000u) << 32); }" % d)
        else:
            raise Unsupported("mmx")
        return True

    # ---------- x87 (decoded from opcode bytes) ----------
    def x87(self, insn):
        raw = insn.raw
        p = 0
        while raw[p] in PREFIXES:
            p += 1
        op, modrm = raw[p], raw[p + 1] if p + 1 < len(raw) else None
        if op == 0x9B:
            return True
        if not 0xD8 <= op <= 0xDF:
            raise Unsupported("not x87")
        reg = (modrm >> 3) & 7
        e = self.emit
        if modrm < 0xC0:
            mem = next(v for k, v, _ in insn.ops if k == "mem")
            a = self.addr(insn, mem)
            return self.x87_mem(op, reg, a, insn)
        i = modrm & 7
        arith = ["+", "*", None, None, "-", "-r", "/", "/r"]
        if op == 0xD8:  # st0 = st0 op sti
            if reg in (2, 3):
                e(f"FCOM(ST(0), ST({i}));" + (" FPOP();" if reg == 3 else ""))
            else:
                e(self.farith("ST(0)", "ST(0)", f"ST({i})", arith[reg]))
            return True
        if op in (0xDC, 0xDE):  # sti = sti op st0 (sub/div reversed per Intel encoding)
            if op == 0xDE and modrm == 0xD9:
                e("FCOM(ST(0), ST(1)); FPOP(); FPOP();")
                return True
            if reg in (2, 3):
                e(f"FCOM(ST(0), ST({i}));" + (" FPOP();" if reg == 3 or op == 0xDE else ""))
                return True
            dc = {0: "+", 1: "*", 4: "rsub", 5: "-", 6: "rdiv", 7: "/"}[reg]
            if dc == "rsub":
                e(f"ST({i}) = ST(0) - ST({i});")
            elif dc == "rdiv":
                e(f"ST({i}) = ST(0) / ST({i});")
            else:
                e(f"ST({i}) = ST({i}) {dc} ST(0);")
            if op == 0xDE:
                e("FPOP();")
            return True
        if op == 0xD9:
            if reg == 0:
                e(f"{{ fpu_t v = ST({i}); FPUSH(v); }}")
            elif reg == 1:
                e(f"{{ fpu_t t = ST(0); ST(0) = ST({i}); ST({i}) = t; }}")
            elif modrm == 0xD0:
                pass
            else:
                simple = {
                    0xE0: "ST(0) = -ST(0);", 0xE1: "ST(0) = FM(fabs)(ST(0));",
                    0xE4: "FCOM(ST(0), 0.0);", 0xE5: "FXAM();",
                    0xE8: "FPUSH(1.0);", 0xE9: "FPUSH(3.321928094887362347870319429489390175864831393);",
                    0xEA: "FPUSH(1.442695040888963407359924681001892137426645954);",
                    0xEB: "FPUSH(3.141592653589793238462643383279502884197169399);",
                    0xEC: "FPUSH(0.301029995663981195213738894724493026768189881);",
                    0xED: "FPUSH(0.693147180559945309417232121458176568075500134);",
                    0xEE: "FPUSH(0.0);",
                    0xF0: "ST(0) = FM(exp2)(ST(0)) - (fpu_t)1.0;",
                    0xF1: "ST(1) = ST(1) * FM(log2)(ST(0)); FPOP();",
                    0xF2: "ST(0) = FM(tan)(ST(0)); FPUSH(1.0); F.sw &= ~0x400;",
                    0xF3: "ST(1) = FM(atan2)(ST(1), ST(0)); FPOP();",
                    0xF5: "ST(0) = FM(remainder)(ST(0), ST(1)); F.sw &= ~0x400;",
                    0xF6: "F.top = (F.top - 1) & 7;", 0xF7: "F.top = (F.top + 1) & 7;",
                    0xF8: "FPREM();",
                    0xF9: "ST(1) = ST(1) * FM(log2)(ST(0) + (fpu_t)1.0); FPOP();",
                    0xFA: "ST(0) = FM(sqrt)(ST(0));",
                    0xFB: "{ fpu_t v = ST(0); ST(0) = FM(sin)(v); FPUSH(FM(cos)(v)); F.sw &= ~0x400; }",
                    0xFC: "ST(0) = F_ROUND(ST(0));",
                    0xFD: "ST(0) = FM(ldexp)(ST(0), (int)trunc(ST(1)));",
                    0xFE: "ST(0) = FM(sin)(ST(0)); F.sw &= ~0x400;",
                    0xFF: "ST(0) = FM(cos)(ST(0)); F.sw &= ~0x400;",
                }
                if modrm not in simple:
                    raise Unsupported(f"D9 {modrm:02x}")
                e(simple[modrm])
            return True
        if op == 0xDB:
            if modrm == 0xE2:
                e("F.sw &= 0x4700;")  # fnclex: clear exception flags, keep condition codes
                return True
            if modrm == 0xE3:
                e("F.cw = 0x37f; F.sw = 0; F.top = 0; F.tag = 0xffff;")
                return True
            raise Unsupported(f"DB {modrm:02x}")
        if op == 0xDD:
            if reg == 0:
                e("/* ffree */")
            elif reg in (2, 3):
                e(f"ST({i}) = ST(0);" + (" FPOP();" if reg == 3 else ""))
            elif reg in (4, 5):
                e(f"FCOM(ST(0), ST({i}));" + (" FPOP();" if reg == 5 else ""))
            else:
                raise Unsupported(f"DD {modrm:02x}")
            return True
        if op == 0xDA and modrm == 0xE9:
            e("FCOM(ST(0), ST(1)); FPOP(); FPOP();")
            return True
        if op == 0xDF and modrm == 0xE0:
            e(self.reg_write("ax", "FSTSW()"))
            return True
        raise Unsupported(f"x87 {op:02x} {modrm:02x}")

    @staticmethod
    def farith(dst, a, b, o):
        if o == "-r":
            return f"{dst} = {b} - {a};"
        if o == "/r":
            return f"{dst} = {b} / {a};"
        return f"{dst} = {a} {o} {b};"

    def x87_mem(self, op, reg, a, insn):
        e = self.emit
        loads = {0xD8: f"(fpu_t)RDF32({a})", 0xDC: f"(fpu_t)RDF64({a})",
                 0xDA: f"(fpu_t)(int32_t)RD32({a})", 0xDE: f"(fpu_t)(int16_t)RD16({a})"}
        if op in loads:
            v = loads[op]
            if reg in (2, 3):
                e(f"FCOM(ST(0), {v});" + (" FPOP();" if reg == 3 else ""))
            else:
                e(self.farith("ST(0)", "ST(0)", v, ["+", "*", None, None, "-", "-r", "/", "/r"][reg]))
            return True
        if op == 0xD9:
            table = {0: f"FPUSH((fpu_t)RDF32({a}));", 2: f"WRF32({a}, (float)ST(0));",
                     3: f"WRF32({a}, (float)ST(0)); FPOP();", 4: f"FLDENV({a});",
                     5: f"F.cw = RD16({a});", 6: f"FNSTENV({a});", 7: f"WR16({a}, F.cw);"}
        elif op == 0xDB:
            table = {0: f"FPUSH((fpu_t)(int32_t)RD32({a}));",
                     2: f"WR32({a}, (uint32_t)F_TO_I32(ST(0)));",
                     3: f"WR32({a}, (uint32_t)F_TO_I32(ST(0))); FPOP();",
                     5: f"FPUSH(RDF80({a}));", 7: f"WRF80({a}, ST(0)); FPOP();"}
        elif op == 0xDD:
            table = {0: f"FPUSH((fpu_t)RDF64({a}));", 2: f"WRF64({a}, ST(0));",
                     3: f"WRF64({a}, ST(0)); FPOP();", 7: f"WR16({a}, FSTSW());"}
        elif op == 0xDF:
            table = {0: f"FPUSH((fpu_t)(int16_t)RD16({a}));",
                     2: f"WR16({a}, (uint16_t)F_TO_I16(ST(0)));",
                     3: f"WR16({a}, (uint16_t)F_TO_I16(ST(0))); FPOP();",
                     5: f"FPUSH((fpu_t)(int64_t)RD64({a}));",
                     7: f"WR64({a}, (uint64_t)F_TO_I64(ST(0))); FPOP();"}
        else:
            table = {}
        if reg not in table:
            raise Unsupported(f"x87 mem {op:02x}/{reg}")
        e(table[reg])
        return True


class Translator:
    def __init__(self, img, discovery, regcache=True, x87_locals=True):
        self.img = img
        self.regcache = regcache
        self.x87_locals = x87_locals
        self.d = discovery
        self.imports_used = {}
        self.unsupported = {}
        self.smc_uses = 0
        self.uncached = 0   # functions translated without register caching
        self.x87_local = 0  # functions with the x87 stack in locals
        self.x87_fail = {}  # reasons functions with x87 code kept the global stack
        self.x87_deltas = {}
        self.emitted = {}
        self.smc_bytes = self.find_smc_bytes()

    def find_smc_bytes(self):
        """Bytes of .text written by `mov [abs], reg` somewhere in live code."""
        out = set()
        for insn in self.d.insns.values():
            if insn.mnem != "mov" or insn.ops[0][0] != "mem":
                continue
            base, index, _, disp, _ = insn.ops[0][1]
            if base is None and index is None and self.img.in_text(disp):
                out.update(range(disp, disp + insn.ops[0][2]))
        return out

    def use_import(self, dll, name):
        n = imp_name(dll, name)
        self.imports_used[n] = (dll, name)
        return n

    def analyze(self, entries):
        """Emit every function once and compute each one's net x87 stack effect (fixpoint over
        the call graph), for x87_locals."""
        for e in entries:
            ft = FunctionTranslator(self, e, self.d.functions[e]["insns"])
            ft.emit_body()
            self.emitted[e] = ft
        deltas = {}
        for _ in range(20):
            new = {}
            for e, ft in self.emitted.items():
                new[e] = x87_delta(ft, deltas)
            if new == deltas:
                break
            deltas = new
        self.x87_deltas = deltas

    def translate_function(self, entry):
        ft = self.emitted.get(entry)
        if ft is None:
            ft = FunctionTranslator(self, entry, self.d.functions[entry]["insns"])
        return ft.translate()

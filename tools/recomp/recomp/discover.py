"""Recursive-descent code discovery and function partitioning for an MSVC-built PE32.

Seeds: entry point, call targets, and relocation slots that point into .text
(vtables, callbacks, EH tables). Jump tables are recognised from
`jmp dword ptr [reg*4 + table]` and bounded by the preceding `cmp reg, N; ja`.
"""
from collections import deque

import capstone
from capstone import x86

NORETURN_IMPORTS = {"ExitProcess", "ExitThread", "FatalExit", "FatalAppExitA",
                    "RaiseException", "_CxxThrowException"}


_SUBREG = {"al": "eax", "ah": "eax", "ax": "eax", "bl": "ebx", "bh": "ebx", "bx": "ebx",
           "cl": "ecx", "ch": "ecx", "cx": "ecx", "dl": "edx", "dh": "edx", "dx": "edx",
           "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp"}


def full_reg(name):
    return _SUBREG.get(name, name)


class Insn:
    __slots__ = ("va", "size", "mnem", "op_str", "groups", "ops", "raw")

    def __init__(self, ci):
        self.va = ci.address
        self.size = ci.size
        self.mnem = ci.mnemonic
        self.op_str = ci.op_str
        self.groups = set(ci.groups)
        self.raw = bytes(ci.bytes)
        self.ops = []
        for op in ci.operands:
            if op.type == x86.X86_OP_IMM:
                self.ops.append(("imm", op.imm & 0xFFFFFFFF, op.size))
            elif op.type == x86.X86_OP_REG:
                self.ops.append(("reg", ci.reg_name(op.reg), op.size))
            elif op.type == x86.X86_OP_MEM:
                m = op.mem
                self.ops.append(("mem", (
                    ci.reg_name(m.base) if m.base else None,
                    ci.reg_name(m.index) if m.index else None,
                    m.scale, m.disp & 0xFFFFFFFF,
                    ci.reg_name(m.segment) if m.segment else None), op.size))

    @property
    def end(self):
        return self.va + self.size

    def __repr__(self):
        return f"{self.va:08x}: {self.mnem} {self.op_str}"


class JumpTable:
    def __init__(self, insn_va, table_va, targets, index_table=None):
        self.insn_va = insn_va
        self.table_va = table_va
        self.targets = targets
        self.index_table = index_table  # (va, length) for MSVC two-level byte index tables


class Discovery:
    def __init__(self, img):
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.insns = {}            # va -> Insn
        self.owner = {}            # byte va -> insn va (to detect overlapping decodes)
        self.entries = set()       # function entry VAs
        self.entry_reasons = {}    # va -> first reason seen
        self.labels = set()        # branch targets
        self.tables = {}           # jmp insn va -> JumpTable
        self.data_ranges = []      # (va, size, kind) inside .text that are data
        self.indirect_jumps = []   # jmp reg / jmp [mem] we could not resolve
        self.indirect_calls = 0
        self.bad = []              # (va, reason)
        self.overlaps = []         # (va, existing_insn_va)
        self.noreturn_calls = set()

    # ---------- decoding ----------
    def decode(self, va):
        if va in self.insns:
            return self.insns[va]
        if not self.img.in_text(va):
            return None
        ci = next(self.md.disasm(self.img.bytes(va, 16), va, count=1), None)
        if ci is None:
            return None
        insn = Insn(ci)
        for b in range(va, insn.end):
            prev = self.owner.get(b)
            if prev is not None and prev != va:
                self.overlaps.append((va, prev))
                break
        for b in range(va, insn.end):
            self.owner.setdefault(b, va)
        self.insns[va] = insn
        return insn

    def add_entry(self, va, reason):
        if va not in self.entries:
            self.entries.add(va)
            self.entry_reasons[va] = reason
            return True
        return False

    # ---------- main walk ----------
    def walk(self, start, queue):
        """Linear walk from start until an unconditional control transfer."""
        va = start
        while True:
            if va in self.insns and va != start:
                return  # already decoded from here on
            insn = self.decode(va)
            if insn is None:
                self.bad.append((va, "undecodable or outside .text"))
                return
            m = insn.mnem
            if m == "int3" or m == "hlt":
                if va == start:
                    self.bad.append((va, f"control reached {m}"))
                return
            if m == "ret" or m == "retf" or m == "iretd":
                return
            if m == "call":
                kind, val, _ = insn.ops[0]
                if kind == "imm":
                    if self.add_entry(val, f"call from {va:#x}"):
                        queue.append(val)
                elif kind == "mem" and val[0] is None and val[1] is None and val[3] in self.img.imports:
                    if self.img.imports[val[3]][1] in NORETURN_IMPORTS:
                        self.noreturn_calls.add(va)
                        return
                else:
                    self.indirect_calls += 1
            elif m == "jmp":
                kind, val, _ = insn.ops[0]
                if kind == "imm":
                    self.labels.add(val)
                    queue.append(val)
                elif kind == "mem":
                    self.handle_indirect_jmp(insn, queue)
                else:
                    self.indirect_jumps.append(insn.va)
                return
            elif x86.X86_GRP_JUMP in insn.groups:  # jcc / loop / jecxz
                kind, val, _ = insn.ops[0]
                self.labels.add(val)
                queue.append(val)
            va = insn.end

    def handle_indirect_jmp(self, insn, queue):
        base, index, scale, disp, seg = insn.ops[0][1]
        if base is None and index is None:
            if disp in self.img.imports:
                return  # import thunk: tail call into the IAT
            self.indirect_jumps.append(insn.va)
            return
        if index is None or scale != 4 or base is not None or disp not in self.img.relocs and not self.img.in_text(disp):
            self.indirect_jumps.append(insn.va)
            return
        count, index_table = self.table_bound(insn, index)
        targets = []
        va = disp
        while True:
            if count is not None and len(targets) >= count:
                break
            if va not in self.img.relocs:
                break
            t = self.img.u32(va)
            if not self.img.in_text(t):
                break
            targets.append(t)
            va += 4
            if count is None and (va in self.insns or va in self.entries):
                break
        if not targets:
            self.indirect_jumps.append(insn.va)
            return
        self.tables[insn.va] = JumpTable(insn.va, disp, targets, index_table)
        self.data_ranges.append((disp, 4 * len(targets), "jumptable"))
        if index_table:
            self.data_ranges.append((index_table[0], index_table[1], "jumptable-index"))
        for t in targets:
            self.labels.add(t)
            queue.append(t)

    def table_bound(self, jmp, index_reg):
        """Look back a few instructions for `cmp reg, N` + `ja` and an MSVC byte index table."""
        prevs = []
        va = jmp.va
        # Walk backwards over already-decoded linear predecessors.
        for _ in range(8):
            p = next((self.insns[v] for v in range(va - 1, va - 16, -1)
                      if v in self.insns and self.insns[v].end == va), None)
            if p is None:
                break
            prevs.append(p)
            va = p.va
        index_table = None
        reg = index_reg
        for p in prevs:
            if p.mnem in ("ret", "call", "jmp") or (p.mnem.startswith("j") and p.mnem not in ("ja", "jae")):
                break
            dst = p.ops[0] if p.ops else None
            writes_reg = dst is not None and dst[0] == "reg" and full_reg(dst[1]) == reg
            if (writes_reg and index_table is None and p.mnem in ("movzx", "mov")
                    and len(p.ops) == 2 and p.ops[1][0] == "mem" and p.ops[1][2] == 1):
                # MSVC two-level switch: index = byte [idx + table] (movzx, or xor r,r; mov r8,...)
                b, i, s, d, _ = p.ops[1][1]
                if b is None and i is not None and s == 1:
                    index_table, reg = d, full_reg(i)
                    continue
                if b is not None and i is None:
                    index_table, reg = d, full_reg(b)
                    continue
                break
            if p.mnem == "cmp" and dst == ("reg", reg, 4) and p.ops[1][0] == "imm":
                n = p.ops[1][1] + 1
                if index_table is not None:
                    entries = max(self.img.bytes(index_table, n)) + 1
                    return entries, (index_table, n)
                return n, None
            if writes_reg and not (p.mnem == "xor" and p.ops[1] == dst):
                break
        return None, None

    # ---------- driver ----------
    def run(self):
        img = self.img
        queue = deque()
        self.add_entry(img.entry, "PE entry point")
        queue.append(img.entry)

        # Pointers into .text stored in data sections: vtables, callback tables, EH tables.
        for slot in sorted(img.relocs):
            if img.in_text(slot):
                continue
            t = img.u32(slot)
            if img.in_text(t):
                if self.add_entry(t, f"data pointer at {slot:#x}"):
                    queue.append(t)

        while True:
            while queue:
                self.walk(queue.popleft(), queue)
            # Pointers embedded in decoded instructions (push offset callback, mov reg, offset fn).
            table_slots = set()
            for jt in self.tables.values():
                table_slots.update(range(jt.table_va, jt.table_va + 4 * len(jt.targets)))
            new = False
            for slot in sorted(img.relocs):
                if not img.in_text(slot) or slot in table_slots:
                    continue
                ins_va = self.owner.get(slot)
                if ins_va is None:
                    continue
                insn = self.insns[ins_va]
                t = img.u32(slot)
                if not img.in_text(t):
                    continue
                if insn.mnem in ("push", "mov") and any(k == "imm" for k, _, _ in insn.ops):
                    if self.add_entry(t, f"code pointer operand at {ins_va:#x}"):
                        queue.append(t)
                        new = True
            if not new:
                break
        return self

    # ---------- partitioning ----------
    def successors(self, insn):
        m = insn.mnem
        if m in ("ret", "retf", "iretd", "int3", "hlt"):
            return []
        if insn.va in self.noreturn_calls:
            return []
        if m == "jmp":
            kind, val, _ = insn.ops[0]
            if kind == "imm":
                return [val]
            if insn.va in self.tables:
                return list(dict.fromkeys(self.tables[insn.va].targets))
            return []
        if x86.X86_GRP_JUMP in insn.groups:
            return [insn.end, insn.ops[0][1]]
        return [insn.end]

    def partition(self):
        """Assign instructions to functions. Edges into another entry are tail calls.

        Code reachable from several entries without passing through an entry (shared tails
        such as __CxxFrameHandler reached from per-function EH stubs) gets its branch-target
        heads promoted to entries, then partitioning repeats until nothing is shared.
        """
        while True:
            self._partition_once()
            if not self.shared:
                return self
            heads = {va for va in self.shared
                     if va in self.labels and va not in self.entries}
            if not heads:
                return self
            for va in heads:
                self.add_entry(va, "shared jump target")

    def _partition_once(self):
        self.functions = {}
        claimed = {}
        shared = set()
        for e in sorted(self.entries):
            body = []
            seen = {e}
            work = [e]
            tail = set()
            while work:
                va = work.pop()
                insn = self.insns.get(va)
                if insn is None:
                    continue
                body.append(va)
                if va in claimed and claimed[va] != e:
                    shared.add(va)
                claimed.setdefault(va, e)
                for s in self.successors(insn):
                    if s in self.entries and s != e:
                        tail.add(s)
                        continue
                    if s not in seen:
                        seen.add(s)
                        work.append(s)
            self.functions[e] = {"insns": sorted(body), "tailcalls": sorted(tail)}
        self.shared = shared

    # ---------- data embedded in .text ----------
    def mark_text_data(self):
        """Memory operands with an absolute disp inside .text (byte lookup tables, CRT state
        machines) are data; extend each up to the next decoded instruction."""
        code_starts = sorted(self.insns)
        import bisect
        known = {va for va, _, _ in self.data_ranges}
        for insn in self.insns.values():
            for kind, val, _ in insn.ops:
                if kind != "mem" or insn.mnem in ("jmp", "call", "lea"):
                    continue
                disp = val[3]
                if not self.img.in_text(disp) or disp in known or disp in self.insns:
                    continue
                if self.owner.get(disp) is not None:
                    continue  # SMC operand slot inside a real instruction
                k = bisect.bisect_right(code_starts, disp)
                end = code_starts[k] if k < len(code_starts) else self.img.text.end
                self.data_ranges.append((disp, end - disp, "text-data"))
                known.add(disp)
        return self

    # ---------- coverage ----------
    def coverage(self):
        img = self.img
        covered = bytearray(img.text.vsize)
        lo = img.text.va
        for insn in self.insns.values():
            covered[insn.va - lo:insn.end - lo] = b"\x01" * insn.size
        for va, size, _ in self.data_ranges:
            covered[va - lo:va - lo + size] = b"\x02" * size
        gaps = []
        i = 0
        n = len(covered)
        while i < n:
            if covered[i]:
                i += 1
                continue
            j = i
            while j < n and not covered[j]:
                j += 1
            chunk = img.bytes(lo + i, j - i)
            if not all(b in (0xCC, 0x90) for b in chunk) and not all(b == 0 for b in chunk):
                gaps.append((lo + i, j - i))
            i = j
        code = sum(1 for c in covered if c == 1)
        data = sum(1 for c in covered if c == 2)
        return {"text_bytes": n, "code_bytes": code, "table_bytes": data,
                "unexplained_gaps": gaps}

#!/usr/bin/env python3
"""Find instructions writing to absolute addresses inside a code range (self-modifying code).
usage: smc_scan.py <exe> <lo_va> <hi_va>"""
import sys, pefile, capstone
from capstone import x86
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
ts = base + text.VirtualAddress
code = img[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
off = 0
hits = []
while off < len(code):
    insns = list(md.disasm(code[off:off+16], ts + off, count=1))
    if not insns: off += 1; continue
    i = insns[0]
    for k, op in enumerate(i.operands):
        if op.type == x86.X86_OP_MEM and op.mem.base == 0 and lo <= (op.mem.disp & 0xffffffff) < hi:
            w = (op.access & capstone.CS_AC_WRITE) != 0
            hits.append((i.address, w, f"{i.mnemonic} {i.op_str}"))
    off += i.size
writes = [h for h in hits if h[1]]
print(f"refs={len(hits)} writes={len(writes)}")
for a, w, t in writes: print(f"{a:08x} W {t}")

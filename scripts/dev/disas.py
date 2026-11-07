#!/usr/bin/env python3
"""Disassemble a VA range of game.EXE: disas.py <exe> <start_va> <end_va>"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
data = pe.get_memory_mapped_image()[start - base:end - base]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
imports = {imp.address: f"{e.dll.decode()}!{(imp.name or b'?').decode()}"
           for e in (pe.parse_data_directories() or pe.DIRECTORY_ENTRY_IMPORT) for imp in e.imports}
for i in md.disasm(data, start):
    note = ""
    for a, n in imports.items():
        if f"0x{a:x}" in i.op_str: note = "  ; " + n
    print(f"{i.address:08x}: {i.bytes.hex():<20} {i.mnemonic} {i.op_str}{note}")

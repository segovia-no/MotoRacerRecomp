#!/usr/bin/env python3
"""Generate C sources from the GOG game.EXE.

usage: recomp_main.py <game.EXE> <out_dir> [--per-file N] [--only VA,VA,...]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from recomp.discover import Discovery  # noqa: E402
from recomp.pe_image import Image  # noqa: E402
from recomp.translate import Translator, fn_name  # noqa: E402

HEADER = "/* Generated from game.EXE by tools/recomp. Do not edit, do not commit. */\n"


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    with open(path, "w") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--per-file", type=int, default=64)
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    t0 = time.time()
    img = Image(args.exe)
    d = Discovery(img).run().partition().mark_text_data()
    tr = Translator(img, d)
    entries = sorted(d.entries)
    if args.only:
        entries = [int(x, 16) for x in args.only.split(",")]
    os.makedirs(args.out, exist_ok=True)

    chunks = [entries[i:i + args.per_file] for i in range(0, len(entries), args.per_file)]
    for n, chunk in enumerate(chunks):
        body = [HEADER, '#include "recomp.h"', '#include "recomp_funcs.h"', '#include "imports.h"', ""]
        for e in chunk:
            body.append(tr.translate_function(e))
            body.append("")
        write_if_changed(os.path.join(args.out, f"code_{n:03d}.c"), "\n".join(body))

    protos = [HEADER, "#pragma once", ""] + [f"void {fn_name(e)}(void);" for e in sorted(d.entries)]
    write_if_changed(os.path.join(args.out, "recomp_funcs.h"), "\n".join(protos) + "\n")

    imps = [HEADER, "#pragma once", "/* Import shims: stdcall/cdecl args are on the guest stack;"
            " each shim pops its own return address (and args if stdcall). */", ""]
    imps += [f"void {n}(void); /* {dll}!{name} */" for n, (dll, name) in sorted(tr.imports_used.items())]
    write_if_changed(os.path.join(args.out, "imports.h"), "\n".join(imps) + "\n")

    table = [HEADER, '#include "recomp.h"', '#include "recomp_funcs.h"', "",
             "typedef struct { uint32_t va; RecompFn fn; } RecompEntry;",
             "const RecompEntry recomp_table[] = {"]
    table += [f"    {{0x{e:08x}u, {fn_name(e)}}}," for e in sorted(d.entries)]
    table += ["};", f"const uint32_t recomp_table_len = {len(d.entries)};", ""]
    write_if_changed(os.path.join(args.out, "dispatch_table.c"), "\n".join(table))

    print(f"functions {len(entries)} in {len(chunks)} files, imports used {len(tr.imports_used)}, "
          f"smc operand reads {tr.smc_uses}, time {time.time() - t0:.1f}s")
    if tr.unsupported:
        print("UNSUPPORTED:", dict(sorted(tr.unsupported.items(), key=lambda x: -x[1])))


if __name__ == "__main__":
    main()

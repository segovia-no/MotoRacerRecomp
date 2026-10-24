#!/usr/bin/env python3
"""Generate C sources from the GOG game.EXE.

usage: recomp_main.py <game.EXE> <out_dir> [--files N] [--only VA,VA,...] [--module]

Always writes exactly N code files (code_000.c ..) so build systems know the outputs.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from recomp.discover import Discovery  # noqa: E402
from recomp.pe_image import Image  # noqa: E402
from recomp.translate import NATIVE_FUNCS, Translator, fn_name, imp_name as imp  # noqa: E402

HEADER = "/* Generated from game.EXE by tools/recomp. Do not edit, do not commit. */\n"


def write_if_changed(path, text):
    # "\n" on every host: the same bytes on Windows as elsewhere
    if os.path.exists(path) and open(path, encoding="utf-8", newline="").read() == text:
        return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--files", type=int, default=64)
    ap.add_argument("--only", default=None)
    ap.add_argument("--no-regcache", action="store_true",
                    help="keep guest registers in R everywhere (baseline for benchmarks)")
    ap.add_argument("--no-x87-locals", action="store_true",
                    help="keep the x87 stack in F everywhere (for benchmarks)")
    ap.add_argument("--no-spill-opt", action="store_true",
                    help="write back / reload all registers at every call (for benchmarks)")
    ap.add_argument("--module", action="store_true",
                    help="code for a game-code module (docs/28-easy-install.md): import shims are "
                         "slots the host fills, instead of weak fallbacks overridden at link time")
    args = ap.parse_args()

    t0 = time.time()
    img = Image(args.exe)
    d = Discovery(img).run().partition().mark_text_data()
    tr = Translator(img, d, regcache=not args.no_regcache, x87_locals=not args.no_x87_locals,
                    spill_opt=not args.no_spill_opt)
    entries = sorted(d.entries)
    if args.only:
        entries = [int(x, 16) for x in args.only.split(",")]
    os.makedirs(args.out, exist_ok=True)
    tr.analyze(entries)

    per = -(-len(entries) // args.files)
    chunks = [entries[i * per:(i + 1) * per] for i in range(args.files)]
    for n, chunk in enumerate(chunks):
        body = [HEADER, '#include "recomp.h"', '#include "recomp_funcs.h"', '#include "imports.h"', ""]
        for e in chunk:
            body.append(tr.translate_function(e))
            body.append("")
        write_if_changed(os.path.join(args.out, f"code_{n:03d}.c"), "\n".join(body))

    protos = [HEADER, "#pragma once", ""] + [f"void {fn_name(e)}(void);" for e in sorted(d.entries)]
    write_if_changed(os.path.join(args.out, "recomp_funcs.h"), "\n".join(protos) + "\n")

    # Every IAT slot, not just directly-called ones: MSVC often caches import pointers in
    # registers (mov esi, [IAT]; call esi), which reach the shim through call_indirect.
    all_imports = sorted(img.imports.items())
    for _, (dll, name) in all_imports:
        tr.use_import(dll, name)
    imps = [HEADER, "#pragma once", "/* Import shims: args are on the guest stack above the return"
            " address; each shim pops its own return address (and args if stdcall). */", ""]
    imps += [f"void {n}(void); /* {dll}!{name} */" for n, (dll, name) in sorted(tr.imports_used.items())]
    write_if_changed(os.path.join(args.out, "imports.h"), "\n".join(imps) + "\n")

    used = sorted(tr.imports_used.items())
    stubs = [HEADER, '#include "recomp.h"', '#include "imports.h"', "",
             "void import_unimplemented(const char *dll, const char *name);", "",
             "/* the host's shim per name (a module's host fills these when it loads it) */",
             "const char *const recomp_slot_names[] = {"]
    stubs += [f'    "{n}",' for n, _ in used]
    stubs += ["};", f"RecompFn recomp_slots[{len(used)}];", f"const uint32_t recomp_slot_count = {len(used)};", ""]
    for i, (n, (dll, name)) in enumerate(used):
        if args.module:   # the shims live in the host program
            stubs.append(f'void {n}(void) {{ if (recomp_slots[{i}]) recomp_slots[{i}](); '
                         f'else import_unimplemented("{dll}", "{name}"); }}')
        else:             # the runtime's strong definitions replace these at link time
            stubs.append(f'__attribute__((weak)) void {n}(void) {{ import_unimplemented("{dll}", "{name}"); }}')
    stubs += ["", "const ImportEntry import_table[] = {"]
    stubs += [f'    {{0x{va:08x}u, "{dll}", "{name}", {imp(dll, name)}}},' for va, (dll, name) in all_imports]
    stubs += ["};", f"const uint32_t import_table_len = {len(all_imports)};", ""]
    write_if_changed(os.path.join(args.out, "imports.c"), "\n".join(stubs))

    table = [HEADER, '#include "recomp.h"', '#include "recomp_funcs.h"', "",
             "const RecompEntry recomp_table[] = {"]
    table += [f"    {{0x{e:08x}u, {fn_name(e)}}}," for e in sorted(d.entries)]
    table += ["};", f"const uint32_t recomp_table_len = {len(d.entries)};", ""]
    # the translated bodies of functions with a native version, which the runtime's check mode and
    # fallbacks call by name
    natives = [e for e in sorted(d.entries) if e in NATIVE_FUNCS]
    table += [f"void {fn_name(e)}_t(void);" for e in natives]
    table += ["const RecompEntry recomp_fallbacks[] = {"]
    table += [f"    {{0x{e:08x}u, {fn_name(e)}_t}}," for e in natives]
    table += ["};", "",
              "extern const ImportEntry import_table[];",
              "extern const char *const recomp_slot_names[];",
              "extern RecompFn recomp_slots[];",
              "/* what a host program loading this code as a module looks up (docs/28-easy-install.md) */",
              "RECOMP_EXPORT const RecompModule recomp_module = {",
              f"    RECOMP_MODULE_VERSION, recomp_table, {len(d.entries)}, recomp_fallbacks, {len(natives)},",
              f"    import_table, {len(all_imports)}, recomp_slot_names, recomp_slots, {len(used)},",
              "};", ""]
    write_if_changed(os.path.join(args.out, "dispatch_table.c"), "\n".join(table))

    print(f"functions {len(entries)} in {len(chunks)} files, imports {len(tr.imports_used)}, "
          f"smc operand reads {tr.smc_uses}, without register caching {tr.uncached}, "
          f"x87 stack in locals {tr.x87_local} (kept global: {tr.x87_fail}), "
          f"time {time.time() - t0:.1f}s")
    if tr.unsupported:
        print("UNSUPPORTED:", dict(sorted(tr.unsupported.items(), key=lambda x: -x[1])))


if __name__ == "__main__":
    main()

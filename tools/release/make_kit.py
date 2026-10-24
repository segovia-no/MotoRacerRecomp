#!/usr/bin/env python3
"""Assemble the kit a release program uses on its first start (docs/28-easy-install.md, E4).

    make_kit.py --tools DIR --out KIT [--host-lib LIB] [--build-id ID] [--link]

KIT gets the platform tools from DIR (tools/release/fetch_tools.py: python/, zig/, innoextract,
licenses/), the translator (tools/build_gamecode.py, tools/recomp/), the headers the generated C
includes (runtime/include/), the default port settings, the Windows program's import library
(--host-lib) and BUILD_ID. A program whose module was built from another BUILD_ID rebuilds it.
--link symlinks the tools instead of copying them (local tests).
"""
import argparse
import hashlib
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def sources():
    """The files that decide the generated code: translator and the headers it is compiled with."""
    files = [os.path.join("tools", "build_gamecode.py"), os.path.join("tools", "default_settings.ini")]
    for base in (os.path.join("tools", "recomp"), os.path.join("runtime", "include")):
        for d, dirs, names in os.walk(os.path.join(ROOT, base)):
            dirs[:] = sorted(x for x in dirs if x not in ("__pycache__", "tests"))
            for n in sorted(names):
                if n.endswith((".py", ".h")):
                    files.append(os.path.relpath(os.path.join(d, n), ROOT))
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tools", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--host-lib")
    ap.add_argument("--build-id")
    ap.add_argument("--link", action="store_true")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    if os.path.lexists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    h = hashlib.sha256()
    for rel in sources():
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, rel), dst)
        h.update(rel.replace(os.sep, "/").encode() + b"\0" + open(os.path.join(ROOT, rel), "rb").read())
    shutil.move(os.path.join(out, "tools", "default_settings.ini"), os.path.join(out, "default_settings.ini"))
    tools = os.path.abspath(args.tools)
    for name in sorted(os.listdir(tools)):
        src, dst = os.path.join(tools, name), os.path.join(out, name)
        if args.link:
            os.symlink(src, dst)
        elif os.path.isdir(src):
            shutil.copytree(src, dst, symlinks=True)
        else:
            shutil.copy2(src, dst)
    if args.host_lib:
        shutil.copy2(args.host_lib, os.path.join(out, os.path.basename(args.host_lib)))
        h.update(open(args.host_lib, "rb").read())
    build_id = args.build_id
    if not build_id:   # local kits: the commit + a hash of what decides the module
        try:
            rev = subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty"], capture_output=True,
                                 text=True).stdout.strip()
        except OSError:
            rev = ""
        build_id = f"{rev or 'local'}-{h.hexdigest()[:12]}"
    with open(os.path.join(out, "BUILD_ID"), "w", newline="\n") as f:
        f.write(build_id + "\n")
    print(f"kit {build_id} in {out}")


if __name__ == "__main__":
    main()

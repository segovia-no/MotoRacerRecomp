#!/usr/bin/env python3
"""Build the game-code module from game.EXE with a bundled `zig cc` (docs/28-easy-install.md, E2).

    build_gamecode.py --exe game.EXE --out DIR --zig ZIG [--target macos|linux|windows]
                      [--host-lib LIB] [--jobs N]

Translates game.EXE to C (tools/recomp, --module), compiles the files in parallel and links
DIR/gamecode.so (macOS, Linux) or DIR/gamecode.dll (Windows), which the program built with
MOTORACER_GAME_MODULE=ON loads. Only a C compiler is needed: the module's undefined symbols are
the program's (R, M, the import shims, ...), resolved when the program loads it. Windows links
against the program's import library (--host-lib, libmotoracer.dll.a from the host build).
"""
import argparse
import concurrent.futures
import os
import platform
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FILES = 64

# zig targets: the glibc floor of the Steam Deck build (docs/15-ports-plan.md); the macOS minimum
# of the program
TARGETS = {
    "macos": {"arm64": "aarch64-macos.11.0", "x86_64": "x86_64-macos.11.0"},
    "linux": {"x86_64": "x86_64-linux-gnu.2.29", "arm64": "aarch64-linux-gnu.2.29"},
    "windows": {"x86_64": "x86_64-windows-gnu"},
}
# same as the gamecode target in CMakeLists.txt (+ module build: PIC, hidden visibility)
CFLAGS = ["-O2", "-g0", "-w", "-fno-strict-aliasing", "-fPIC", "-fvisibility=hidden", "-fno-sanitize=all",
          "-DRECOMP_GENERATED", "-DRECOMP_MODULE"]


def host_os():
    return {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}[platform.system()]


def host_arch():
    m = platform.machine().lower()
    return "arm64" if m in ("arm64", "aarch64") else "x86_64"


def run(cmd, env=None):
    p = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + p.stdout + p.stderr)
        raise SystemExit(f"failed: {os.path.basename(cmd[0])} (exit {p.returncode})")
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--zig", required=True)
    ap.add_argument("--target", choices=sorted(TARGETS), default=host_os())
    ap.add_argument("--arch", choices=["arm64", "x86_64"], default=host_arch())
    ap.add_argument("--host-lib", help="Windows: the program's import library")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--progress", action="store_true",
                    help="machine-readable progress lines (@step NAME, @compiled DONE TOTAL) for the "
                         "program's first-start screen")
    args = ap.parse_args()
    progress = (lambda *a: print("@" + " ".join(str(x) for x in a), flush=True)) if args.progress else (lambda *a: None)

    triple = TARGETS[args.target].get(args.arch)
    if not triple:
        raise SystemExit(f"no {args.arch} build for {args.target}")
    out = os.path.abspath(args.out)
    gen, obj = os.path.join(out, "gen"), os.path.join(out, "obj")
    os.makedirs(obj, exist_ok=True)
    env = dict(os.environ)
    # zig's caches (compiled libc / compiler-rt pieces) under the output, not in the user's home
    env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(out, "zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = os.path.join(out, "zig-cache")

    t0 = time.time()
    progress("step", "translate")
    run([sys.executable, "-I", os.path.join(HERE, "recomp", "recomp_main.py"), args.exe, gen,
         "--files", str(FILES), "--module"])
    t1 = time.time()
    print(f"translated in {t1 - t0:.1f} s", flush=True)

    sources = sorted(f for f in os.listdir(gen) if f.endswith(".c"))
    inc = ["-I", os.path.join(ROOT, "runtime", "include"), "-I", gen]

    def compile_one(name):
        o = os.path.join(obj, name[:-2] + ".o")
        run([args.zig, "cc", "-target", triple, "-c", *CFLAGS, *inc, "-o", o, os.path.join(gen, name)], env)
        return o

    progress("step", "compile")
    objects = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for o in pool.map(compile_one, sources):
            objects.append(o)
            progress("compiled", len(objects), len(sources))
    t2 = time.time()
    print(f"compiled {len(objects)} files in {t2 - t1:.1f} s ({args.jobs} jobs)", flush=True)

    if args.target == "windows":
        if not args.host_lib:
            raise SystemExit("--host-lib (the program's import library) is needed for Windows")
        module = os.path.join(out, "gamecode.dll")
        link = ["-shared", "-o", module, *objects, args.host_lib]
    elif args.target == "macos":
        module = os.path.join(out, "gamecode.so")
        link = ["-shared", "-o", module, *objects, "-Wl,-undefined,dynamic_lookup"]
    else:
        module = os.path.join(out, "gamecode.so")
        link = ["-shared", "-o", module, *objects]
    progress("step", "link")
    run([args.zig, "cc", "-target", triple, "-s", *link], env)   # -s: no symbol / debug tables
    print(f"linked {module} in {time.time() - t2:.1f} s (total {time.time() - t0:.1f} s)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Rank guest functions by executed x86 instructions between two per-function count dumps.

usage: icprof.py <ic_A.txt> <ic_B.txt> [--gen build/pspprof/gen] [--top 40]

The dumps come from a counting build (CMake MOTORACER_COUNT_INSNS=ON) run with
MOTORACER_ICOUNT_DUMP=<dir>; A and B bracket the window to profile (e.g. the race). Counts are
"self" counts: instructions of the function itself, not of its callees. With --gen, the static
size of each function (translated x86 instructions) is read from the generated C.
"""
import argparse
import collections
import glob
import re


def load(path):
    frames, counts = 0, {}
    with open(path) as f:
        for line in f:
            a, b = line.split()
            if a == "frames":
                frames = int(b)
            else:
                counts[int(a, 16)] = int(b)
    return frames, counts


def static_sizes(gen_dir):
    sizes = collections.Counter()
    for path in glob.glob(f"{gen_dir}/*.c"):
        with open(path) as f:
            for m in re.finditer(r"IC\(0x([0-9a-f]+)\)", f.read()):
                sizes[int(m.group(1), 16)] += 1
    return sizes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--gen")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()
    fa, ca = load(args.a)
    fb, cb = load(args.b)
    frames = fb - fa
    diff = {va: n - ca.get(va, 0) for va, n in cb.items() if n - ca.get(va, 0) > 0}
    total = sum(diff.values())
    sizes = static_sizes(args.gen) if args.gen else {}
    print(f"{frames} frames, {total / frames / 1e6:.3f} M x86 instructions/frame, "
          f"{len(diff)} functions executed")
    print(f"{'#':>3} {'function':>9} {'K/frame':>9} {'%':>6} {'cum %':>6} {'size':>6}")
    cum = 0
    for i, (va, n) in enumerate(sorted(diff.items(), key=lambda x: -x[1])[:args.top]):
        cum += n
        size = sizes.get(va, "")
        print(f"{i + 1:>3} {va:#09x} {n / frames / 1e3:>9.1f} {100 * n / total:>6.2f} "
              f"{100 * cum / total:>6.1f} {size:>6}")
    # how concentrated is it: functions needed for 50 / 75 / 90 % of the instructions
    ranked = sorted(diff.values(), reverse=True)
    for goal in (50, 75, 90):
        acc = 0
        for k, n in enumerate(ranked):
            acc += n
            if acc >= total * goal / 100:
                print(f"{goal}% of the instructions: top {k + 1} functions")
                break


if __name__ == "__main__":
    main()

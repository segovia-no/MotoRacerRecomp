#!/usr/bin/env python3
"""Phase 1: discover functions in game.EXE and report coverage.

usage: discover_main.py <game.EXE> <out.json>
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from recomp.discover import Discovery  # noqa: E402
from recomp.pe_image import Image  # noqa: E402


def main():
    exe, out = sys.argv[1], sys.argv[2]
    t0 = time.time()
    img = Image(exe)
    d = Discovery(img).run().partition().mark_text_data()
    cov = d.coverage()

    reasons = {}
    for r in d.entry_reasons.values():
        k = r.split(" ")[0] + " " + r.split(" ")[1]
        reasons[k] = reasons.get(k, 0) + 1

    print(f"decoded insns       {len(d.insns)}")
    print(f"functions           {len(d.entries)}  {reasons}")
    print(f"jump tables         {len(d.tables)}")
    print(f"indirect jmps       {len(d.indirect_jumps)} (unresolved)")
    print(f"indirect calls      {d.indirect_calls}")
    print(f"noreturn call sites {len(d.noreturn_calls)}")
    print(f"overlapping decodes {len(d.overlaps)}")
    print(f"bad paths           {len(d.bad)}")
    print(f"shared insns        {len(d.shared)}")
    print(f".text {cov['text_bytes']} B: code {cov['code_bytes']} "
          f"({100*cov['code_bytes']/cov['text_bytes']:.1f}%), tables {cov['table_bytes']}, "
          f"unexplained gaps {len(cov['unexplained_gaps'])} totalling "
          f"{sum(s for _, s in cov['unexplained_gaps'])} B")
    print(f"time {time.time()-t0:.1f}s")

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    json.dump({
        "sha256": img.sha256,
        "functions": {f"{e:#x}": {"reason": d.entry_reasons[e],
                                  "ninsns": len(f["insns"]),
                                  "tailcalls": [f"{t:#x}" for t in f["tailcalls"]]}
                      for e, f in sorted(d.functions.items())},
        "jump_tables": {f"{k:#x}": {"table": f"{t.table_va:#x}",
                                    "targets": [f"{x:#x}" for x in t.targets],
                                    "index_table": [f"{t.index_table[0]:#x}", t.index_table[1]] if t.index_table else None}
                        for k, t in sorted(d.tables.items())},
        "indirect_jumps": [f"{v:#x}" for v in sorted(d.indirect_jumps)],
        "bad": [[f"{v:#x}", r] for v, r in d.bad],
        "overlaps": [[f"{a:#x}", f"{b:#x}"] for a, b in d.overlaps],
        "gaps": [[f"{v:#x}", s] for v, s in cov["unexplained_gaps"]],
    }, open(out, "w"), indent=1)


if __name__ == "__main__":
    main()

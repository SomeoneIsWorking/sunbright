#!/usr/bin/env python3
"""tools/oracle/parse_native_draw_dump.py — turn aurora's SB_DRAW_DUMP stderr lines
into the same-shape per-draw TSV as tools/oracle/parse_fifo_dff.py --matrix-tsv, for
side-by-side comparison against the retail .dff oracle.

Input: a captured stderr log from a run with SB_DRAW_DUMP=<n> SB_DRAW_DUMP_AFTER=<retrace>
(extern/aurora/lib/gx/command_processor.cpp push_gx_draw), containing lines like:

  [draw-dump] #189462 prim=152 verts=5 tex0=58x68 zcmp=1 zupd=0 trans=(...) proj=O blend=1
  vp=(...) sc=(...) tev=2 ch0[...] a0[...] prj=[0.0045 -0.0031 -0.5000 -0.5000] cU=1 aU=1
  bm=1 bf=4/1 pos[desc=3 cnt=1 type=4 frac=0] clr0=3 clr1=0 mtxIdx=0
  posmtx=[10.56 0.00 0.00 79560.29 | 0.00 10.56 0.00 81048.28 | 0.00 0.00 7.32 -222751.72]
  mark='DrawBuf Sky Xlu'

Only `prj=[...]` (4 values: proj matrix elements [0],[5],[10],[11] — NOT the raw 6 GX
projection params retail's parser dumps, aurora reconstructs a 4x4 and never keeps the
original 6-param form after XF load) and `posmtx=[...]` (12 values, full 3x4) are decoded.
"""
import argparse
import re
import sys

LINE_RE = re.compile(
    r"^\[draw-dump\] #(?P<idx>\d+) prim=(?P<prim>\d+) verts=(?P<verts>\d+) .*?"
    r"proj=(?P<proj>[OP]) .*?"
    r"prj=\[(?P<prj>[^\]]+)\].*?"
    r"mtxIdx=(?P<mtxidx>\d+) "
    r"posmtx=\[(?P<posmtx>[^\]]+)\] mark='(?P<mark>[^']*)'"
)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="stderr log containing [draw-dump] lines")
    ap.add_argument("--out", required=True, help="output TSV path")
    args = ap.parse_args()

    with open(args.log, "rb") as f:
        raw = f.read()
    text = raw.decode("ascii", errors="replace")

    rows = []
    unmatched = 0
    for line in text.splitlines():
        if "[draw-dump]" not in line:
            continue
        m = LINE_RE.match(line)
        if not m:
            unmatched += 1
            continue
        prj = [float(x) for x in m.group("prj").split()]
        pm = [float(x) for x in m.group("posmtx").replace("|", " ").split()]
        if len(prj) != 4 or len(pm) != 12:
            unmatched += 1
            continue
        rows.append((int(m.group("idx")), m.group("mark"), int(m.group("verts")),
                      int(m.group("prim")), m.group("proj"), prj, int(m.group("mtxidx")), pm))

    if not rows:
        raise SystemExit(f"REFUSING: 0 [draw-dump] lines parsed from {args.log} "
                          f"({unmatched} lines matched the marker but not the full regex)")

    cols = ["draw_idx", "mark", "nverts", "prim", "proj_type"] + [f"projdiag{k}" for k in range(4)] + \
           ["mtxIdx"] + [f"pm{k}" for k in range(12)]
    with open(args.out, "w") as out:
        out.write("\t".join(cols) + "\n")
        for idx, mark, verts, prim, proj, prj, mtxidx, pm in rows:
            out.write("\t".join([str(idx), mark, str(verts), str(prim), proj] +
                                 [f"{v:.6g}" for v in prj] + [str(mtxidx)] +
                                 [f"{v:.6g}" for v in pm]) + "\n")
    print(f"wrote {len(rows)} draws to {args.out} ({unmatched} unparseable lines skipped)",
          file=sys.stderr)


if __name__ == "__main__":
    main()

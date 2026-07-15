#!/usr/bin/env python3
"""tools/oracle/draw_diff.py — native-vs-oracle per-draw RENDER-STATE diff.

The recurring blocker this removes: hand-grepping two SB_DRAW_DUMP logs to find where
native's per-draw GX state (lighting / material / tex / blend) diverges from the oracle's.
This turns that into one reusable, committed tool that emits a ranked divergence report.

Both inputs are stderr logs captured with SB_DRAW_DUMP=1 (aurora
command_processor.cpp push_gx_draw prints one `[draw-dump] ... mark='...'` line per draw):
  - NATIVE:  SB_STAGE=15 SB_DRAW_DUMP=1 SB_DRAW_DUMP_AFTER=<n> ./run.sh   (live game render)
  - ORACLE:  SB_FIFO_REPLAY=<dff> SB_DRAW_DUMP=1 ./run.sh                 (aurora replays retail draws)

Draws have no stable cross-run IDs (different counts/order), so we key by the draw-buffer
`mark` and, within each mark, compare the SET of distinct render-state signatures each side
emits. A signature that appears under a mark in one log but not the other — or a mark whose
lighting/material/blend signatures differ between the two — is a divergence to RE.

Usage:
  draw_diff.py --native draw_native.log --oracle draw_oracle.log [--field ch0|all] [--mark SUBSTR]
Exit 0 always; the report is the product. Add --json for a machine-readable dump.
"""
import argparse
import json
import re
import sys
from collections import defaultdict

# Pull the interesting, render-affecting fields out of a [draw-dump] line. Everything is
# optional-tolerant so a format tweak degrades gracefully instead of crashing.
FIELD_RES = {
    "prim": re.compile(r"prim=(\d+)"),
    "verts": re.compile(r"verts=(\d+)"),
    "tex0": re.compile(r"tex0=(\d+x\d+)"),
    "proj": re.compile(r"proj=([OP])"),
    "blend": re.compile(r"blend=(\d+)"),
    "tev": re.compile(r"tev=(\d+)"),
    "ch0": re.compile(r"ch0\[([^\]]*)\]"),
    "a0": re.compile(r"a0\[([^\]]*)\]"),
    "bm": re.compile(r"\bbm=(\d+)\b"),
    "bf": re.compile(r"\bbf=(\d+/\d+)"),
    "cull": re.compile(r"\bcull=(\d+)"),
    "zfunc": re.compile(r"\bzfunc=(\d+)"),
}
MARK_RE = re.compile(r"mark='([^']*)'")
DUMP_RE = re.compile(r"\[draw-dump\]")


def parse_log(path, field, group):
    """Return {group_key: {signature: count}}. `signature` is a tuple of the selected fields.
    group='mark' keys by draw-buffer mark; group='none' puts everything under one bucket
    (needed because the oracle .dff replay has no game-side markers — all mark='')."""
    marks = defaultdict(lambda: defaultdict(int))
    n = 0
    with open(path, "rb") as f:
        raw = f.read()
    for line in raw.split(b"\n"):
        if b"[draw-dump]" not in line:
            continue
        s = line.decode("utf-8", "replace")
        if group == "none":
            mark = "<all draws>"
        else:
            m = MARK_RE.search(s)
            mark = m.group(1) if m else "<none>"
        vals = {}
        for k, rx in FIELD_RES.items():
            mm = rx.search(s)
            vals[k] = mm.group(1) if mm else "?"
        if field == "ch0":
            # lighting-only signature: the ch0 block + tex + prim (what drives lit appearance)
            sig = ("ch0=" + vals["ch0"], "a0=" + vals["a0"], "tex0=" + vals["tex0"],
                   "bm=" + vals["bm"], "bf=" + vals["bf"])
        else:
            sig = tuple(f"{k}={vals[k]}" for k in
                        ("prim", "verts", "tex0", "proj", "blend", "tev", "ch0", "a0", "bm", "bf", "cull", "zfunc"))
        marks[mark][sig] += 1
        n += 1
    return marks, n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--native", required=True, help="SB_DRAW_DUMP log from the live native run")
    ap.add_argument("--oracle", required=True, help="SB_DRAW_DUMP log from the oracle .dff replay")
    ap.add_argument("--field", choices=["ch0", "all"], default="ch0",
                    help="ch0 = lighting/material/tex/blend signature (default); all = full state")
    ap.add_argument("--group", choices=["mark", "none"], default="none",
                    help="none (default) compares the GLOBAL signature set — use this vs a .dff "
                         "replay (no game markers); mark groups by draw-buffer mark (both logs native)")
    ap.add_argument("--mark", default=None, help="only report marks containing this substring")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    nat, nnat = parse_log(args.native, args.field, args.group)
    ora, nora = parse_log(args.oracle, args.field, args.group)
    all_marks = sorted(set(nat) | set(ora))
    if args.mark:
        all_marks = [m for m in all_marks if args.mark in m]

    report = []
    for mark in all_marks:
        nsigs = nat.get(mark, {})
        osigs = ora.get(mark, {})
        only_native = set(nsigs) - set(osigs)
        only_oracle = set(osigs) - set(nsigs)
        if not only_native and not only_oracle:
            continue  # this mark's render-state signatures match exactly
        report.append({
            "mark": mark,
            "native_total": sum(nsigs.values()),
            "oracle_total": sum(osigs.values()),
            "only_native": sorted([" ".join(s) for s in only_native]),
            "only_oracle": sorted([" ".join(s) for s in only_oracle]),
        })

    if args.json:
        print(json.dumps({"native_draws": nnat, "oracle_draws": nora, "divergent_marks": report}, indent=2))
        return 0

    print(f"native draws: {nnat}   oracle draws: {nora}   ({args.field} signature)")
    print(f"marks in native: {len(nat)}   in oracle: {len(ora)}   divergent: {len(report)}\n")
    if not report:
        print("NO render-state divergences under any shared mark — native matches the oracle for this field.")
        return 0
    for d in sorted(report, key=lambda r: -(len(r["only_native"]) + len(r["only_oracle"]))):
        print(f"### mark='{d['mark']}'  (native {d['native_total']} draws, oracle {d['oracle_total']} draws)")
        for s in d["only_native"]:
            print(f"   NATIVE-only: {s}")
        for s in d["only_oracle"]:
            print(f"   ORACLE-only: {s}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

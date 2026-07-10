#!/usr/bin/env python3
"""tools/oracle/parse_native_tev_dump.py — turn a captured SB_DRAW_DUMP + SB_TEV_DUMP window
into the same per-draw-per-stage TEV/material-state TSV shape as tools/oracle/parse_fifo_dff.py
--tev-tsv, so the two can be diffed field-by-field.

Input: stderr lines matching `[draw-dump] ...` (one per draw, carries verts/mark) immediately
followed by zero or more `  [tev] #N stage=S ...` lines (one per active TEV stage, extern/aurora
lib/gx/command_processor.cpp's SB_TEV_DUMP printf) — captured together by running with
SB_DRAW_DUMP=1 SB_DRAW_DUMP_AFTER=<retrace> SB_DRAW_DUMP_FRAME=<retrace> SB_TEV_DUMP=1 (all
four; SB_TEV_DUMP alone does nothing — it's gated inside the SB_DRAW_DUMP window).

Field-encoding note (load-bearing): the native printf already emits DECODED GX SDK enum values
(GXTevColorArg/GXTevAlphaArg/GXTevOp/GXTevBias/GXTevScale/GXTevKColorSel/GXTevKAlphaSel), and
those enums share the SAME raw bit encoding as the GC HW BP registers (GX SDK is a thin wrapper
over the register bitfields) — so these ints are directly comparable to parse_fifo_dff.py
--tev-tsv's raw-bit-decoded columns (ca/cb/cc/cd, c_op/c_bias/c_scale, kc_sel/ka_sel, etc.)
without any further translation table, unlike cull-mode (which DOES need translation, see
parse_native_raster_dump.py's note).
"""
import argparse
import re
import sys

DRAW_RE = re.compile(
    r"^\[draw-dump\] #(?P<idx>\d+) prim=(?P<prim>\d+) verts=(?P<verts>\d+) "
    r".*?mark='(?P<mark>[^']*)'"
)
TEV_RE = re.compile(
    r"^\s*\[tev\] #(?P<idx>\d+) stage=(?P<stage>\d+) texMap=(?P<texmap>-?\d+) "
    r"texCoord=(?P<texcoord>-?\d+) chan=(?P<chan>-?\d+) "
    r"colorPass\(a=(?P<ca>\d+) b=(?P<cb>\d+) c=(?P<cc>\d+) d=(?P<cd>\d+) op=(?P<cop>\d+) "
    r"bias=(?P<cbias>\d+) scale=(?P<cscale>\d+) clamp_outReg=(?P<coutreg>\d+)\) "
    r"alphaPass\(a=(?P<aa>\d+) b=(?P<ab>\d+) c=(?P<ac>\d+) d=(?P<ad>\d+) op=(?P<aop>\d+) "
    r"bias=(?P<abias>\d+) scale=(?P<ascale>\d+) outReg=(?P<aoutreg>\d+)\) "
    r"kcSel=(?P<kcsel>\d+) kaSel=(?P<kasel>\d+) swapRas=(?P<swapras>\d+) swapTex=(?P<swaptex>\d+)"
)


def parse_lines(lines):
    """Returns a list of draw dicts, each with a 'stages' list of per-stage dicts, in the
    original stream order (a [tev] line belongs to whichever [draw-dump] line most recently
    preceded it -- the printf call site emits them back-to-back for the same draw)."""
    draws = []
    current = None
    for line in lines:
        dm = DRAW_RE.search(line)
        if dm:
            g = dm.groupdict()
            current = {"idx": int(g["idx"]), "nverts": int(g["verts"]), "mark": g["mark"], "stages": []}
            draws.append(current)
            continue
        tm = TEV_RE.search(line)
        if tm and current is not None:
            g = tm.groupdict()
            texmap = int(g["texmap"])
            current["stages"].append({
                "texmap": texmap if texmap >= 0 else 255,  # GX_TEXMAP_NULL sentinel, match oracle side
                "texcoord": int(g["texcoord"]),
                "chan": int(g["chan"]),
                "ca": int(g["ca"]), "cb": int(g["cb"]), "cc": int(g["cc"]), "cd": int(g["cd"]),
                "c_op": int(g["cop"]), "c_bias": int(g["cbias"]), "c_scale": int(g["cscale"]),
                "c_outreg": int(g["coutreg"]),
                "aa": int(g["aa"]), "ab": int(g["ab"]), "ac": int(g["ac"]), "ad": int(g["ad"]),
                "a_op": int(g["aop"]), "a_bias": int(g["abias"]), "a_scale": int(g["ascale"]),
                "a_outreg": int(g["aoutreg"]),
                "kc_sel": int(g["kcsel"]), "ka_sel": int(g["kasel"]),
                "swap_ras": int(g["swapras"]), "swap_tex": int(g["swaptex"]),
            })
    return draws


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="captured stderr window containing [draw-dump]/[tev] lines "
                                 "(decode with errors='replace' -- embedded Shift-JIS elsewhere "
                                 "in a raw run log breaks plain grep, per prior sessions' notes)")
    ap.add_argument("--out", required=True, help="output TSV path")
    args = ap.parse_args()

    with open(args.log, "rb") as f:
        raw_lines = f.read().decode("utf-8", errors="replace").splitlines()
    draws = parse_lines(raw_lines)
    if not draws:
        raise SystemExit(f"REFUSING: 0 parseable [draw-dump] lines in {args.log} -- "
                          f"regex out of sync with the printf format, or wrong log file")

    dome = [d for d in draws if d["nverts"] == 202]
    cloud = [d for d in draws if d["nverts"] == 40]
    selected = [("DOME", d) for d in dome[:2]] + [("CLOUD", d) for d in cloud[:3]]
    if not selected:
        raise SystemExit(
            f"REFUSING: found 0 matching dome (202v) or cloud-strip (40v) draws among "
            f"{len(draws)} parsed [draw-dump] entries in {args.log}")

    cols = ["idx", "role", "nverts", "mark", "stage", "num_stages",
            "texmap", "texcoord", "chan_hw",
            "ca", "cb", "cc", "cd", "c_op", "c_bias", "c_scale", "c_outreg",
            "aa", "ab", "ac", "ad", "a_op", "a_bias", "a_scale", "a_outreg",
            "swap_ras", "swap_tex", "kc_sel", "ka_sel"]
    with open(args.out, "w") as out:
        out.write("\t".join(cols) + "\n")
        for role, d in selected:
            stages = d["stages"]
            for st_idx, st in enumerate(stages):
                row = [d["idx"], role, d["nverts"], d["mark"], st_idx, len(stages),
                       st["texmap"], st["texcoord"], st["chan"],
                       st["ca"], st["cb"], st["cc"], st["cd"],
                       st["c_op"], st["c_bias"], st["c_scale"], st["c_outreg"],
                       st["aa"], st["ab"], st["ac"], st["ad"],
                       st["a_op"], st["a_bias"], st["a_scale"], st["a_outreg"],
                       st["swap_ras"], st["swap_tex"], st["kc_sel"], st["ka_sel"]]
                out.write("\t".join(str(x) for x in row) + "\n")
    n_stage_rows = sum(len(d["stages"]) for _, d in selected)
    print(f"# MANIFEST wrote {n_stage_rows} stage-rows from {len(selected)} draws "
          f"({len(dome[:2])} DOME + {len(cloud[:3])} CLOUD) out of {len(draws)} parsed "
          f"[draw-dump] entries in {args.log} to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()

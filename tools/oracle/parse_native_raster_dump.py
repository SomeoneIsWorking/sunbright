#!/usr/bin/env python3
"""tools/oracle/parse_native_raster_dump.py — turn a captured SB_DRAW_DUMP window into the
same per-draw RASTER-STATE TSV shape as tools/oracle/parse_fifo_dff.py --raster-tsv, so the
two can be diffed field-by-field.

Input: stderr lines matching `[draw-dump] ...` (extern/aurora/lib/gx/command_processor.cpp,
push_gx_draw's SB_DRAW_DUMP printf, extended 2026-07-10 with cull/zfunc/alpha-compare).

Field-encoding notes (load-bearing -- do not "fix" these to look prettier without re-checking
the enums):
  - `cull=` is GX SDK's GXCullMode (GX_CULL_NONE=0, GX_CULL_FRONT=1, GX_CULL_BACK=2,
    GX_CULL_ALL=3) -- NOT Dolphin's BPMemory CullMode enum (NONE=0, BACK=1, FRONT=2, ALL=3)
    used by parse_fifo_dff.py's `dolphin_cullmode` column. This script decodes to a
    common english label (NONE/FRONT/BACK/ALL) so the two sides are directly comparable
    without re-deriving the swap by hand.
  - `zfunc=`/`acmp=`'s comp0/comp1 are GX SDK's GXCompare, which (unlike cull mode) uses the
    SAME numeric ordering as Dolphin's CompareMode (both mirror the GC HW register bits
    directly) -- decoded to the same GX_COMPARE_NAMES table for symmetry, not because the
    raw ints would have disagreed.
  - `bm=` is GX SDK's GXBlendMode (GX_BM_NONE=0, GX_BM_BLEND=1, GX_BM_LOGIC=2,
    GX_BM_SUBTRACT=3) -- semantically distinct from Dolphin's separate blend_enable/
    logic_op_enable/subtract bits (BPMemory BlendMode). Decoded to blend_enable/
    logic_op_enable/subtract booleans matching the retail TSV's column meanings:
    NONE -> (0,0,0), BLEND -> (1,0,0), LOGIC -> (0,1,0), SUBTRACT -> (1,0,1).
"""
import argparse
import re
import sys

LINE_RE = re.compile(
    r"\[draw-dump\] #(?P<idx>\d+) prim=(?P<prim>\d+) verts=(?P<verts>\d+) "
    r"tex0=(?P<texw>\d+)x(?P<texh>\d+) zcmp=(?P<zcmp>\d+) zupd=(?P<zupd>\d+) "
    r"trans=\((?P<tx>[-\d.]+),(?P<ty>[-\d.]+),(?P<tz>[-\d.]+)\) proj=(?P<proj>[OP]) "
    r"blend=(?P<blend>\d+) vp=\((?P<vpx>[-\d.]+),(?P<vpy>[-\d.]+) (?P<vpw>[-\d.]+)x(?P<vph>[-\d.]+)\) "
    r"sc=\((?P<scx>-?\d+),(?P<scy>-?\d+) (?P<scw>\d+)x(?P<sch>\d+)\) "
    r".*?"
    r"prj=\[(?P<p0>[-\d.]+) (?P<p1>[-\d.]+) (?P<p2>[-\d.]+) (?P<p3>[-\d.]+)\] "
    r"cU=(?P<cu>\d+) aU=(?P<au>\d+) bm=(?P<bm>\d+) bf=(?P<src>\d+)/(?P<dst>\d+) "
    r".*?"
    r"cull=(?P<cull>\d+) zfunc=(?P<zfunc>\d+) "
    r"acmp=\[c0=(?P<ac0>\d+) r0=(?P<ar0>\d+) op=(?P<aop>\d+) c1=(?P<ac1>\d+) r1=(?P<ar1>\d+)\] "
    r"posmtx=\[(?P<pm00>[-\d.]+) (?P<pm01>[-\d.]+) (?P<pm02>[-\d.]+) (?P<pm03>[-\d.]+) \| "
    r"(?P<pm10>[-\d.]+) (?P<pm11>[-\d.]+) (?P<pm12>[-\d.]+) (?P<pm13>[-\d.]+) \| "
    r"(?P<pm20>[-\d.]+) (?P<pm21>[-\d.]+) (?P<pm22>[-\d.]+) (?P<pm23>[-\d.]+)\] "
    r"mark='(?P<mark>[^']*)'"
)

GX_COMPARE_NAMES = ["NEVER", "LESS", "EQUAL", "LEQUAL", "GREATER", "NEQUAL", "GEQUAL", "ALWAYS"]
GX_CULL_NAMES = ["NONE", "FRONT", "BACK", "ALL"]  # GX SDK GXCullMode ordering
GX_BLEND_MODE = ["NONE", "BLEND", "LOGIC", "SUBTRACT"]  # GXBlendMode ordering
GX_ALPHAOP_NAMES = ["AND", "OR", "XOR", "XNOR"]


def parse_lines(lines):
    rows = []
    for line in lines:
        m = LINE_RE.search(line)
        if not m:
            continue
        g = m.groupdict()
        bm = int(g["bm"])
        blend_enable = 1 if bm in (1, 3) else 0
        logic_op_enable = 1 if bm == 2 else 0
        subtract = 1 if bm == 3 else 0
        rows.append({
            "idx": int(g["idx"]), "mark": g["mark"], "nverts": int(g["verts"]),
            "vp_wd": float(g["vpw"]), "vp_ht": float(g["vph"]),
            "vp_x0": float(g["vpx"]), "vp_y0": float(g["vpy"]),
            "scissor_x0": int(g["scx"]), "scissor_y0": int(g["scy"]),
            "scissor_x1": int(g["scx"]) + int(g["scw"]) - 1,
            "scissor_y1": int(g["scy"]) + int(g["sch"]) - 1,
            "cull": GX_CULL_NAMES[int(g["cull"])],
            "z_test_enable": int(g["zcmp"]), "z_func": GX_COMPARE_NAMES[int(g["zfunc"])],
            "z_update_enable": int(g["zupd"]),
            "blend_enable": blend_enable, "logic_op_enable": logic_op_enable,
            "color_update": int(g["cu"]), "alpha_update": int(g["au"]),
            "dst_factor": int(g["dst"]), "src_factor": int(g["src"]), "subtract": subtract,
            "acmp_ref0": int(g["ar0"]), "acmp_ref1": int(g["ar1"]),
            "acmp_comp0": GX_COMPARE_NAMES[int(g["ac0"])], "acmp_comp1": GX_COMPARE_NAMES[int(g["ac1"])],
            "acmp_logic": GX_ALPHAOP_NAMES[int(g["aop"])],
            "posmtx_tx": float(g["pm03"]), "posmtx_ty": float(g["pm13"]), "posmtx_tz": float(g["pm23"]),
            "proj_type": "PERSPECTIVE" if g["proj"] == "P" else "ORTHOGRAPHIC",
        })
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="captured stderr log containing [draw-dump] lines "
                                 "(grep -a 'draw-dump' <run.log> first if the raw log has "
                                 "embedded Shift-JIS bytes that break plain grep)")
    ap.add_argument("--out", required=True, help="output TSV path")
    args = ap.parse_args()

    with open(args.log, "rb") as f:
        raw_lines = f.read().decode("utf-8", errors="replace").splitlines()
    rows = parse_lines(raw_lines)
    if not rows:
        raise SystemExit(f"REFUSING: 0 parseable [draw-dump] lines in {args.log} -- "
                          f"regex out of sync with the printf format, or wrong log file")

    dome = [r for r in rows if r["nverts"] == 202]
    mapopa = [r for r in rows if abs(r["posmtx_tx"] - 305.42) < 0.5
              and abs(r["posmtx_ty"] - (-1043.36)) < 0.5 and abs(r["posmtx_tz"] - (-353.41)) < 0.5]
    # Pick 3 MapOpa rows with distinct raster signatures (not 3 copies of the same state) --
    # matches the retail side's approach of sampling real signature variety, not padding.
    seen_sig = set()
    mapopa_sample = []
    for r in mapopa:
        sig = (r["cull"], r["z_func"], r["blend_enable"], r["acmp_comp0"])
        if sig not in seen_sig:
            seen_sig.add(sig)
            mapopa_sample.append(r)
        if len(mapopa_sample) == 3:
            break
    if len(mapopa_sample) < 3:
        mapopa_sample = mapopa[:3]

    selected = [("DOME", r) for r in dome[:1]] + [("MAPOPA_ANCHOR", r) for r in mapopa_sample]
    if not selected:
        raise SystemExit("REFUSING: found 0 matching dome/MapOpa-anchor draws in this window")

    cols = ["idx", "role", "nverts", "vp_wd", "vp_ht", "vp_x0", "vp_y0",
            "scissor_x0", "scissor_y0", "scissor_x1", "scissor_y1", "cull",
            "z_test_enable", "z_func", "z_update_enable",
            "blend_enable", "logic_op_enable", "color_update", "alpha_update",
            "dst_factor", "src_factor", "subtract",
            "acmp_ref0", "acmp_ref1", "acmp_comp0", "acmp_comp1", "acmp_logic",
            "posmtx_tx", "posmtx_ty", "posmtx_tz", "proj_type"]
    with open(args.out, "w") as out:
        out.write("\t".join(cols) + "\n")
        for role, r in selected:
            row = [r["idx"], role, r["nverts"], r["vp_wd"], r["vp_ht"], r["vp_x0"], r["vp_y0"],
                   r["scissor_x0"], r["scissor_y0"], r["scissor_x1"], r["scissor_y1"], r["cull"],
                   r["z_test_enable"], r["z_func"], r["z_update_enable"],
                   r["blend_enable"], r["logic_op_enable"], r["color_update"], r["alpha_update"],
                   r["dst_factor"], r["src_factor"], r["subtract"],
                   r["acmp_ref0"], r["acmp_ref1"], r["acmp_comp0"], r["acmp_comp1"], r["acmp_logic"],
                   r["posmtx_tx"], r["posmtx_ty"], r["posmtx_tz"], r["proj_type"]]
            out.write("\t".join(str(x) for x in row) + "\n")
    print(f"# MANIFEST wrote {len(selected)} rows (1 DOME + {len(mapopa_sample)} MAPOPA_ANCHOR) "
          f"from {len(rows)} parsed draw-dump lines in {args.log} to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()

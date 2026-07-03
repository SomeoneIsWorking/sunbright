#!/usr/bin/env python3
"""dome_projection_diff.py — matrix-level projection diff for the sky.bmd dome pass.

Sky #16 dome projection audit (2026-07-04). The pixel-level defect on the title
screen is that native's dome renders brighter blue than oracle at matching pixels
(mean|Δ|=56.46 → ~44 attributable to dome). Native's TEV/material/CLR0 decode has
been proven bit-exact; the working hypothesis is that native's camera/view/matrix
chain projects the 752 dome verts to slightly different NDC than oracle does, so
different verts land under the same pixel.

This tool consumes two matrix snapshots taken at the first-primitive-of-scene-pass:

  * Native — SB_DOME_XF=1 log lines from scratch/passes/quick.log. Parses the LATCH
    proj/vp + posMtx that native's j3dSys hands to the sky.bmd dome draw.

  * Oracle — the per-frame JSON GxFrameInfo dump (fi_pass["scene"] entry) written by
    gx_capture.cpp for the Dolphin-GX build. Contains "proj", "vp", "posMtx" now that
    gx_parse.h latches PNMTX0 at the scene pass's first primitive (see the 2026-07-04
    posmtx_pass extension).

Usage:
    tools/render/dome_projection_diff.py                        \\
        --native scratch/passes/quick.log                        \\
        --oracle scratch/oracle/plaza_frames/frame_00500.json   \\

Reports per-element |delta| for the 6-float projection, 6-float viewport, and the
12-float (3x4 row-major) PNMTX0, plus a one-line summary. A material delta on any
of these three matrices names the divergence.
"""

from __future__ import annotations
import argparse
import json
import re
import sys
from pathlib import Path


_LATCH_PROJ_RE = re.compile(
    r"LATCH\s+type=(\d+)\s+proj\[([^\]]+)\]\s+vp\[([^\]]+)\]"
)
_POSMTX_R_RE = re.compile(r"posMtx\s+r0\[([^\]]+)\]")
_POSMTX_R1_RE = re.compile(r"\s+r1\[([^\]]+)\]")
_POSMTX_R2_RE = re.compile(r"\s+r2\[([^\]]+)\]")


def _floats(s: str) -> list[float]:
    return [float(x) for x in s.replace(",", " ").split()]


def parse_native_log(path: Path) -> dict:
    """Extract the FIRST SB_DOME_XF hit from a sms-boot stderr log.

    Returns a dict with keys `proj_type`, `proj` (6 floats), `vp` (6 floats),
    `posMtx` (12 floats, row-major 3x4). Only the first hit is used — that is the
    ph1 (or ph4) scene-pass dome draw, which is what oracle's scene-pass fingerprint
    latches too.
    """
    text = path.read_text(errors="replace")
    if "[dome-xf] hit#1" not in text:
        sys.exit(f"error: no '[dome-xf]' lines in {path}. did you run with SB_DOME_XF=1?")
    block = text.split("[dome-xf] hit#1", 1)[1].split("[dome-xf] hit#2", 1)[0]
    m = _LATCH_PROJ_RE.search(block)
    if not m:
        sys.exit("error: LATCH line missing in native dome-xf block")
    r0 = _POSMTX_R_RE.search(block)
    r1 = _POSMTX_R1_RE.search(block)
    r2 = _POSMTX_R2_RE.search(block)
    if not (r0 and r1 and r2):
        sys.exit("error: posMtx rows missing in native dome-xf block")
    return {
        "proj_type": int(m.group(1)),
        "proj": _floats(m.group(2)),
        "vp": _floats(m.group(3)),
        "posMtx": _floats(r0.group(1)) + _floats(r1.group(1)) + _floats(r2.group(1)),
    }


def parse_oracle_json(path: Path) -> dict:
    """Return the SCENE-pass entry from an oracle GxFrameInfo JSON dump.

    Expects the layout gx_capture writes: a top-level object with a "passes" array,
    each entry keyed by "pass" ∈ {"scene", "hud"} (or the older {0, 1} form).
    """
    data = json.loads(path.read_text())
    passes = data.get("passes") or data.get("pass") or []
    scene = None
    for p in passes:
        if p.get("pass") in ("scene", 0, "0"):
            scene = p
            break
    if scene is None:
        # legacy schema: top-level object IS the scene pass.
        scene = data
    if "proj" not in scene:
        sys.exit(f"error: no 'proj' in oracle JSON scene pass ({path}). "
                 f"regenerate oracle capture from a build that latches proj_pass.")
    return {
        "proj_type": scene.get("projType", 0),
        "proj": list(scene["proj"]),
        "vp": list(scene.get("vp", [0.0]*6)),
        "posMtx": list(scene.get("posMtx", [0.0]*12)),
    }


def _diff_row(name: str, a: list[float], b: list[float], fmt: str = "%.5f") -> float:
    n = min(len(a), len(b))
    delta = [a[i] - b[i] for i in range(n)]
    absd = [abs(d) for d in delta]
    print(f"{name:>8}  {'native':>13}  {'oracle':>13}  {'delta':>13}")
    for i in range(n):
        marker = " !!" if absd[i] > 1e-3 else ""
        print(f"  [{i:2d}]   {a[i]:13.5f}  {b[i]:13.5f}  {delta[i]:+13.5f}{marker}")
    return max(absd) if absd else 0.0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--native", type=Path,
                    default=Path("scratch/passes/quick.log"),
                    help="path to sms-boot stderr with SB_DOME_XF=1 output")
    ap.add_argument("--oracle", type=Path, required=True,
                    help="per-frame oracle GxFrameInfo JSON with the scene-pass entry")
    a = ap.parse_args()

    nv = parse_native_log(a.native)
    orc = parse_oracle_json(a.oracle)

    print(f"native  proj_type={nv['proj_type']}")
    print(f"oracle  proj_type={orc['proj_type']}")
    if nv["proj_type"] != orc["proj_type"]:
        print("*** proj_type MISMATCH — verify the scene pass, not HUD/ortho ***")

    print("\n--- projection[6] (perspective: m00 m11 m22 m23 zscale zoffset) ---")
    dp = _diff_row("proj", nv["proj"], orc["proj"])
    print("\n--- viewport[6] (wd, ht, nearz, x, y, farz) ---")
    dv = _diff_row("vp", nv["vp"], orc["vp"])
    print("\n--- posMtx[12] (row-major 3x4 — PNMTX0 at scene-pass first primitive) ---")
    dm = _diff_row("posMtx", nv["posMtx"], orc["posMtx"])

    print("\n--- SUMMARY ---")
    print(f"  max|Δproj|   = {dp:.6f}")
    print(f"  max|Δvp|     = {dv:.6f}")
    print(f"  max|ΔposMtx| = {dm:.6f}")
    biggest = max(dp, dv, dm)
    if dp == biggest and dp > 1e-3:
        print("  → projection matrix diverges. Suspect C_MTXPerspective / SETPROJECTION call site.")
    elif dv == biggest and dv > 1e-3:
        print("  → viewport diverges. Suspect GXSetViewport call site.")
    elif dm == biggest and dm > 1e-3:
        print("  → PNMTX0 (view matrix) diverges. Suspect view chain: TCamera → J3DModel::viewCalc.")
    else:
        print("  → all matrices match. Divergence is downstream (vertex decode or per-vertex skin).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

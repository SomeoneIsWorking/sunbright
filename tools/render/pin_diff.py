#!/usr/bin/env python3
"""pin_diff.py — cross-engine scene-sync assertion at a pinned tick.

Loads scratch/frames/pin_<N>.json (native) and scratch/frames/pin_<N>_oracle.json
(oracle) and compares the state each engine rendered at the pinned tick.

The contract, per plan nifty-chasing-bachman.md:
  - view mtx per-element |Δ| ≤ VIEW_TOL   (or fail)
  - proj matches (native's 4x4 vs oracle's 6-float SETPROJECTION — both maps
    to the same GC perspective params; we cross-check the resulting FOVy/aspect)
  - viewport equal
  - light count equal, per-light (pos, col) match within LIGHT_TOL
  - Mario fields exact (native-only fields; oracle side doesn't scrape guest RAM
    for this MVP — Mario asserted native-side only)

Exit:
  0   — state parity green
  2   — state parity RED (mismatch beyond tolerance) — DO NOT run pixel diff
  1   — missing/malformed inputs

Usage: pin_diff.py <tick>  or  pin_diff.py <native.json> <oracle.json>
"""
from __future__ import annotations

import json
import math
import os
import sys
from dataclasses import dataclass

VIEW_TOL = 1e-3        # posMtx elements are ~unit-length rotations + translation floats,
                       # both engines run the same guest code → 1e-3 is generous slack for
                       # inline-gather ordering, still tight enough to catch a real drift
LIGHT_TOL = 1e-2       # light pos/col floats are stored as GC f32 in guest RAM
PROJ_TOL = 1e-3        # GC proj[6] values (near/far/A/B/C/D) are shared game state


@dataclass
class Verdict:
    label: str
    ok: bool
    detail: str = ""

    def __str__(self) -> str:
        tag = "OK " if self.ok else "FAIL"
        return f"  [{tag}] {self.label}: {self.detail}"


def load(path: str) -> dict:
    if not os.path.exists(path):
        print(f"pin_diff: MISSING {path}", file=sys.stderr)
        sys.exit(1)
    try:
        with open(path) as f:
            return json.load(f)
    except json.JSONDecodeError as e:
        print(f"pin_diff: MALFORMED {path}: {e}", file=sys.stderr)
        sys.exit(1)


def diff_arr(a, b, tol) -> tuple[bool, float, int]:
    """Return (ok, max_abs_delta, worst_idx)."""
    if len(a) != len(b):
        return False, math.inf, -1
    worst = 0.0
    idx = -1
    for i, (x, y) in enumerate(zip(a, b)):
        d = abs(float(x) - float(y))
        if d > worst:
            worst = d
            idx = i
    return worst <= tol, worst, idx


def fovy_from_proj6(proj6, proj_type) -> float | None:
    """GC SETPROJECTION perspective packet → FOVy degrees.
    proj[0] = 2 * near / width  (== m00 of the 4x4)
    proj[2] = 2 * near / height (== m11 of the 4x4)
    FOVy = 2*atan(1/m11) — proj[2] / 2 = near / height/2 → tan(fov/2) = height/(2 near) → 1/m11.
    """
    if proj_type != 0 or len(proj6) < 3:
        return None
    m11 = proj6[2]
    if m11 <= 0.0:
        return None
    return math.degrees(2.0 * math.atan(1.0 / m11))


def fovy_from_proj44(proj44) -> float | None:
    if len(proj44) < 16:
        return None
    m11 = proj44[5]  # row-major 4x4, m[1][1] at index 5
    if m11 <= 0.0:
        return None
    return math.degrees(2.0 * math.atan(1.0 / m11))


def cmp_views(n: dict, o: dict, verdicts: list[Verdict]) -> None:
    nv = n.get("view", [])
    ov = o.get("view", [])
    ok, d, i = diff_arr(nv, ov, VIEW_TOL)
    verdicts.append(Verdict(
        "view mtx (3x4)",
        ok,
        f"max|Δ|={d:.6g} at idx {i} (tol {VIEW_TOL})"))


def cmp_proj(n: dict, o: dict, verdicts: list[Verdict]) -> None:
    # Native emits full 4x4; oracle emits GC SETPROJECTION 6-float packet.
    # Cross-check via FOVy — the load-bearing scalar for camera parity.
    n_fov = fovy_from_proj44(n.get("proj", []))
    o_fov = fovy_from_proj6(o.get("proj6", []), o.get("proj_type", 0))
    if n_fov is None or o_fov is None:
        verdicts.append(Verdict("proj FOVy", False,
                                f"could not derive (native={n_fov}, oracle={o_fov})"))
        return
    d = abs(n_fov - o_fov)
    ok = d < 0.1  # 0.1° — extremely generous; real mismatch is degrees (52° vs 40°)
    verdicts.append(Verdict(
        "proj FOVy (degrees)",
        ok,
        f"native={n_fov:.4f}° oracle={o_fov:.4f}° |Δ|={d:.4f} (tol 0.1)"))
    # Also compare the shared proj[0]/proj[2] scalars (m00, m11 == GC packet slot 0, 2).
    n_p = n.get("proj", [])
    o_p = o.get("proj6", [])
    if len(n_p) >= 6 and len(o_p) >= 3:
        d00 = abs(n_p[0] - o_p[0])
        d11 = abs(n_p[5] - o_p[2])
        okp = d00 <= PROJ_TOL and d11 <= PROJ_TOL
        verdicts.append(Verdict(
            "proj m00/m11",
            okp,
            f"m00 |Δ|={d00:.4g} m11 |Δ|={d11:.4g} (tol {PROJ_TOL})"))


def cmp_lights(n: dict, o: dict, verdicts: list[Verdict]) -> None:
    nl = n.get("lights", [])
    ol = o.get("lights", [])
    if len(nl) != len(ol):
        verdicts.append(Verdict("light count", False,
                                f"native={len(nl)} oracle={len(ol)}"))
        return
    verdicts.append(Verdict("light count", True, f"n={len(nl)}"))
    # Match by index (both sides emit only valid lights, in stream order).
    all_ok = True
    worst = 0.0
    detail = []
    for i in range(len(nl)):
        okp, dp, _ = diff_arr(nl[i]["pos"], ol[i]["pos"], LIGHT_TOL)
        okc, dc, _ = diff_arr(nl[i]["col"], ol[i]["col"], LIGHT_TOL)
        worst = max(worst, dp, dc)
        if not (okp and okc):
            all_ok = False
            detail.append(f"L{i} posΔ={dp:.4g} colΔ={dc:.4g}")
    verdicts.append(Verdict(
        "per-light (pos, col)",
        all_ok,
        f"max|Δ|={worst:.4g} (tol {LIGHT_TOL})" + (" — " + "; ".join(detail) if detail else "")))


def cmp_int(name: str, nv, ov, verdicts: list[Verdict], hex_fmt: bool = False) -> None:
    ok = (nv == ov)
    fmt = (lambda x: f"0x{x:x}") if hex_fmt else (lambda x: f"{x}")
    verdicts.append(Verdict(name, ok, f"native={fmt(nv)} oracle={fmt(ov)}"))


def cmp_vec3(name: str, nv, ov, verdicts: list[Verdict], tol: float = 1e-3) -> None:
    if len(nv) != 3 or len(ov) != 3:
        verdicts.append(Verdict(name, False, f"malformed native={nv} oracle={ov}"))
        return
    d = max(abs(float(nv[i]) - float(ov[i])) for i in range(3))
    verdicts.append(Verdict(
        name,
        d <= tol,
        f"native=({nv[0]:.3f},{nv[1]:.3f},{nv[2]:.3f}) oracle=({ov[0]:.3f},{ov[1]:.3f},{ov[2]:.3f}) max|Δ|={d:.4g}"))


def cmp_game_state(n: dict, o: dict, verdicts: list[Verdict]) -> None:
    """Byte-exact compare of every game-state field between native and oracle.
    Any mismatch is a real RE anchor: something in game LOGIC diverged."""
    napp = n.get("app", {})
    oapp = o.get("app", {})
    if napp.get("have") and oapp.get("have"):
        cmp_int("app.mAppState",   napp.get("appState", -1), oapp.get("appState", -1), verdicts, hex_fmt=True)
        cmp_int("app.mNextArea",   napp.get("nextArea", -1), oapp.get("nextArea", -1), verdicts, hex_fmt=True)
        cmp_int("app.mMovie",      napp.get("movie", -1),    oapp.get("movie", -1),    verdicts, hex_fmt=True)
    else:
        verdicts.append(Verdict("app.*", False,
                                f"one side missing (native.have={napp.get('have')} oracle.have={oapp.get('have')})"))

    ncam = n.get("camera", {})
    ocam = o.get("camera", {})
    if ncam.get("have") and ocam.get("have"):
        cmp_vec3("camera.mPosition",     ncam.get("pos",    [0]*3), ocam.get("pos",    [0]*3), verdicts, tol=1e-2)
        cmp_vec3("camera.mTarget",       ncam.get("target", [0]*3), ocam.get("target", [0]*3), verdicts, tol=1e-2)
        cmp_vec3("camera.up",            ncam.get("up",     [0]*3), ocam.get("up",     [0]*3), verdicts, tol=1e-3)
        cmp_int ("camera.mMode",         ncam.get("mode", -1), ocam.get("mode", -1), verdicts)
        # FOVy is float; already asserted in cmp_proj but repeat here at game-state level.
        nfov = ncam.get("fovy", 0.0); ofov = ocam.get("fovy", 0.0)
        verdicts.append(Verdict("camera.mFovy",
                                abs(nfov - ofov) < 1e-3,
                                f"native={nfov:.4f} oracle={ofov:.4f} |Δ|={abs(nfov-ofov):.4g}"))
        # The intro countdowns — THIS is the game-tick anchor. If these differ, native and oracle
        # are at different intro-camera phases, i.e. NOT at the same game state.
        cmp_int("camera.mIntroChaseTimer", ncam.get("introTimer", -1), ocam.get("introTimer", -1), verdicts)
        cmp_int("camera.mLoadPanTimer",    ncam.get("loadPanTimer", -1), ocam.get("loadPanTimer", -1), verdicts)
        cmp_int("camera.mLoadPanFrames",   ncam.get("loadPanFrames", -1), ocam.get("loadPanFrames", -1), verdicts)
    else:
        verdicts.append(Verdict("camera.*", False,
                                f"one side missing (native.have={ncam.get('have')} oracle.have={ocam.get('have')})"))

    nmp = n.get("marioPos", {})
    omp = o.get("marioPos", {})
    if nmp.get("have") and omp.get("have"):
        cmp_vec3("gpMarioPos", nmp.get("pos", [0]*3), omp.get("pos", [0]*3), verdicts, tol=1e-2)

    nlm = n.get("lmgr", {})
    olm = o.get("lmgr", {})
    if nlm.get("have") and olm.get("have"):
        cmp_vec3("lmgr.mEffectPos",     nlm.get("effectPos", [0]*3), olm.get("effectPos", [0]*3), verdicts, tol=1e-2)
        cmp_int ("lmgr.mEffectColor",   nlm.get("effectColor", -1),  olm.get("effectColor", -1),  verdicts, hex_fmt=True)
        cmp_int ("lmgr.mEffectEnabled", nlm.get("effectEnabled",-1), olm.get("effectEnabled",-1), verdicts)
        cmp_int ("lmgr.mEffectValid",   nlm.get("effectValid",-1),   olm.get("effectValid",-1),   verdicts)
    else:
        verdicts.append(Verdict("lmgr.*", False,
                                f"one side missing (native.have={nlm.get('have')} oracle.have={olm.get('have')})"))

    nm = n.get("mario", {})
    om = o.get("mario", {})
    if nm.get("have") and om.get("have"):
        cmp_int("mario.mStatus",      nm.get("status", -1),      om.get("status", -1),      verdicts, hex_fmt=True)
        cmp_int("mario.mAnimationId", nm.get("animId", -1),      om.get("animId", -1),      verdicts, hex_fmt=True)
        cmp_int("mario.mStatusState", nm.get("statusState", -1), om.get("statusState", -1), verdicts)
        cmp_int("mario.mStatusTimer", nm.get("statusTimer", -1), om.get("statusTimer", -1), verdicts)
    else:
        verdicts.append(Verdict("mario.*", False,
                                f"one side missing (native.have={nm.get('have')} oracle.have={om.get('have')})"))


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1].isdigit():
        tick = int(sys.argv[1])
        n_path = f"scratch/frames/pin_{tick:04d}.json"
        o_path = f"scratch/frames/pin_{tick:04d}_oracle.json"
    elif len(sys.argv) == 3:
        n_path, o_path = sys.argv[1], sys.argv[2]
    else:
        print(__doc__.strip(), file=sys.stderr)
        return 1

    print(f"pin_diff: native={n_path}  oracle={o_path}")
    n = load(n_path)
    o = load(o_path)
    print(f"  native tick={n.get('tick')}  oracle tick={o.get('tick')}")
    if n.get("tick") != o.get("tick"):
        print(f"  FAIL: tick mismatch native={n.get('tick')} oracle={o.get('tick')}",
              file=sys.stderr)
        return 2

    verdicts: list[Verdict] = []
    # Game-state parity FIRST — a game-state divergence means the two engines are at
    # different states, so any renderer diff is confounded. Only if game state matches
    # does a view/proj/lights diff carry meaning.
    cmp_game_state(n, o, verdicts)
    cmp_views(n, o, verdicts)
    cmp_proj(n, o, verdicts)
    cmp_lights(n, o, verdicts)

    for v in verdicts:
        print(v)

    failed = [v for v in verdicts if not v.ok]
    if failed:
        print(f"\nSTATE PARITY RED — {len(failed)} check(s) failed. "
              f"Do NOT run pixel diff on this pin.", file=sys.stderr)
        return 2
    print("\nSTATE PARITY GREEN — pin is real. Pixel diff on the PNG bundle is meaningful.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Unit tests for parity_sweep.py's CROSS-ENGINE comparison logic — specifically the
LIGHTING + projType window-summary comparison added for the title/file-select per-pass
fidelity work (compare geometry AND lighting, not pixels).

The cross-engine path (oracle dump = geometry+lighting but NO `proj` field, native dump =
full) compares WINDOW SUMMARIES (medians over the settled window) because the two engines
number frames independently. These tests assert that a divergence in light count, ambient
colour, material colour, or projection TYPE is detected, and that matching lighting passes.

Run: python3 tools/render/parity_sweep_test.py   (exit 0 = all pass)
Also wired as ctest `parity_sweep_test`.
"""
import io, json, os, sys, tempfile, contextlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import parity_sweep as ps


def _write(frames):
    f = tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False)
    for fr in frames:
        f.write(json.dumps(fr) + "\n")
    f.close()
    return f.name


# A settled on-screen frame. `engine="oracle"` omits the `proj`/`vp`/`xfb` fields (matches the
# ngx oracle dump) so parity_sweep stays in cross-engine window-summary mode; `engine="native"`
# carries them. Both carry projType + lights + amb + matc (the cross-engine lighting signal).
def frame(engine, n_lights=1, amb=(0.20, 0.20, 0.22), matc=(1.0, 1.0, 1.0, 1.0),
          projType=0, onscr=1000, nbatch=40):
    g = {"onscr": onscr, "nan": 0, "ndc": [-0.8, 0.8, -0.7, 0.7, 0.0, 1.0],
         "cks": 12.3, "colcks": 45.6}
    lights = {"n": n_lights, "l": [{"p": [10.0, 20.0, 30.0], "c": [1.0, 0.95, 0.9]}
                                   for _ in range(n_lights)]}
    fr = {"frame": 0, "nverts": onscr + 200, "nbatch": nbatch, "geom": g,
          "projType": projType, "lights": lights, "amb": list(amb), "matc": list(matc)}
    if engine == "native":
        fr["proj"] = [1.2, 0.0, 1.5, 0.0, 1.0, 2.0]
        fr["vp"] = [0.0, 0.0, 640.0, 480.0, 0.0, 1.0]
        fr["xfb"] = {"bright": 100.0}
    return fr


def run_diff(A, B):
    """Run ps.diff capturing stdout; returns (rc, text)."""
    a, b = _write(A), _write(B)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ps.diff(a, b)
    os.unlink(a); os.unlink(b)
    return rc, buf.getvalue()


FAILS = []
def check(cond, msg):
    if cond:
        print(f"  ok  : {msg}")
    else:
        print(f"  FAIL: {msg}")
        FAILS.append(msg)


def main():
    N = 8  # several settled frames each side so the median is stable
    oracle = [frame("oracle") for _ in range(N)]
    native = [frame("native") for _ in range(N)]

    print("[1] matching lighting → no lighting divergence")
    rc, txt = run_diff(oracle, native)
    check("cross-engine summary" in txt, "cross-engine window-summary path taken (oracle lacks proj)")
    check("light count" not in txt.lower() or "diverge" not in txt.lower().split("light count")[0][-40:],
          "no spurious flag on matching lights")
    # The summary must actually REPORT lighting metrics now (regression guard for the feature).
    check("ambient" in txt.lower() or "amb" in txt.lower(), "summary reports an ambient metric")
    check("light" in txt.lower(), "summary reports a light metric")

    print("[2] light COUNT diverges (oracle 1 → native 4) → flagged")
    rc, txt = run_diff([frame("oracle", n_lights=1) for _ in range(N)],
                       [frame("native", n_lights=4) for _ in range(N)])
    check(rc == 1, "diff returns nonzero (divergence)")
    check("light" in txt.lower() and "diverge" in txt.lower(), "light-count divergence reported")

    print("[3] AMBIENT diverges (0.20 → 0.50) → flagged")
    rc, txt = run_diff([frame("oracle", amb=(0.20, 0.20, 0.22)) for _ in range(N)],
                       [frame("native", amb=(0.50, 0.50, 0.55)) for _ in range(N)])
    check(rc == 1, "diff returns nonzero (divergence)")
    check("amb" in txt.lower() and "diverge" in txt.lower(), "ambient divergence reported")

    print("[4] MATERIAL colour diverges (1.0 → 0.4) → flagged")
    rc, txt = run_diff([frame("oracle", matc=(1.0, 1.0, 1.0, 1.0)) for _ in range(N)],
                       [frame("native", matc=(0.4, 0.4, 0.4, 1.0)) for _ in range(N)])
    check(rc == 1, "diff returns nonzero (divergence)")
    check("mat" in txt.lower() and "diverge" in txt.lower(), "material-colour divergence reported")

    print("[5] projTYPE diverges (0 persp → 1 ortho) → flagged")
    rc, txt = run_diff([frame("oracle", projType=0) for _ in range(N)],
                       [frame("native", projType=1) for _ in range(N)])
    check(rc == 1, "diff returns nonzero (divergence)")
    check("projtype" in txt.lower() and "diverge" in txt.lower(), "projType divergence reported")

    print("[6] matching ambient with tiny noise (≤0.02) → NOT flagged")
    rc, txt = run_diff([frame("oracle", amb=(0.20, 0.20, 0.22)) for _ in range(N)],
                       [frame("native", amb=(0.21, 0.20, 0.23)) for _ in range(N)])
    # ambient within tolerance must not be flagged as a divergence (find the ambient row, check no DIVERGE)
    amb_lines = [ln for ln in txt.splitlines() if "amb" in ln.lower()]
    check(all("DIVERGE" not in ln for ln in amb_lines), "ambient within tolerance not flagged")

    if FAILS:
        print(f"\nFAILED {len(FAILS)} check(s)")
        return 1
    print("\nALL parity_sweep lighting tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

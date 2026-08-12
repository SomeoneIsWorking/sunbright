#!/usr/bin/env python3
"""afc_table_check.py — the two runtimes' AFC coefficient tables must be byte-identical.

WHY THIS EXISTS. The AFC predictor coefficients are duplicated: the decomp runtime's voice renderer
(`sms-boot/runtime/jas_kernel_native.cpp`) and the recomp's (`sms-recomp/runtime/devices/
dsp_mixer.cpp`) each carry a copy. The recomp's was transcribed BY HAND and entries 8-15 came out
wrong, while the comment beside it asserted the two were byte-identical — a claim nobody had
checked.

What that produced is the reason this is a committed check rather than a note. A wrong predictor
coefficient does not crash, does not silence anything, and does not shift pitch or tempo. The
decoded waveform stays continuous, in tune and in time; it is simply the wrong waveform. It passed
every numeric gate the mixer reports — non-silent seconds, peak, rms, clipping, adjacent-sample
delta, spectral peak-to-median, beat autocorrelation — because those all measure "is this a
plausible musical signal", and it was one. Only an ear caught it, and only diffing against the
proven copy located it.

THE NEGATIVE. Failing to FIND either table is an error, not a pass. A rename, a move, or a
refactor that inlines the table would otherwise turn this check into a permanent silent success —
which is exactly the failure mode it exists to prevent, one level up.

Usage:
    tools/audio/afc_table_check.py             # compare; exit non-zero on any difference
    tools/audio/afc_table_check.py --selftest  # prove the comparison can FAIL
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

SOURCES = [
    ("decomp", "sms-boot/runtime/jas_kernel_native.cpp"),
    ("recomp", "sms-recomp/runtime/devices/dsp_mixer.cpp"),
]
SYMBOL = "kAfcCoef"
EXPECTED_ENTRIES = 16


def extract(path):
    """Return the 32 integers of the 16x2 table, or raise with a reason."""
    if not os.path.exists(path):
        raise SystemExit(f"afc_table_check: {path} does not exist — the table moved or was "
                         f"renamed, so this check is no longer looking at anything. Fix the path "
                         f"rather than deleting the check.")
    src = open(path, encoding="utf-8", errors="replace").read()
    i = src.find(SYMBOL)
    if i < 0:
        raise SystemExit(f"afc_table_check: no `{SYMBOL}` in {path}. If the table was inlined or "
                         f"renamed this check silently stops comparing, which is the bug it "
                         f"exists to catch.")
    open_brace = src.index("{", i)
    end = src.index("};", open_brace)
    nums = [int(x) for x in re.findall(r"-?\d+", src[open_brace:end])]
    if len(nums) != EXPECTED_ENTRIES * 2:
        raise SystemExit(f"afc_table_check: {path} has {len(nums)} numbers in {SYMBOL}, expected "
                         f"{EXPECTED_ENTRIES * 2}. A partially-parsed table would compare equal on "
                         f"the part it did read.")
    return nums


def compare(tables):
    (na, a), (nb, b) = tables
    diffs = [(k, a[k], b[k]) for k in range(len(a)) if a[k] != b[k]]
    if not diffs:
        print(f"AFC coefficient tables agree: {EXPECTED_ENTRIES} entries, "
              f"{len(a)} values, {na} == {nb}")
        return 0
    print(f"*** AFC coefficient tables DIFFER: {len(diffs)} of {len(a)} values ***")
    for k, x, y in diffs:
        print(f"    entry {k // 2:2d} component {k % 2}:  {na} {x:>6}   {nb} {y:>6}")
    print("\nThese must be byte-identical. A wrong coefficient does not crash and does not measure\n"
          "wrong — it produces a plausible but incorrect waveform that only an ear detects.")
    return 1


def selftest():
    """Prove the comparison reports a difference when there is one, and none when there is not."""
    real = [(n, extract(os.path.join(REPO, p))) for n, p in SOURCES]
    ok = compare(real) == 0
    if not ok:
        print("selftest: the REAL tables differ, so the check is working and the tree is broken.")
        return 1
    mutated = [real[0], (real[1][0], list(real[1][1]))]
    mutated[1][1][17] += 1          # perturb one component of one entry
    print("\n-- selftest: the same comparison against a deliberately perturbed copy --")
    if compare(mutated) == 0:
        print("SELFTEST FAILED: a perturbed table compared EQUAL, so this check cannot fail and "
              "its passes mean nothing.")
        return 1
    print("\nselftest OK: agrees on the real tables, and detects a single perturbed value.")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(compare([(n, extract(os.path.join(REPO, p))) for n, p in SOURCES]))

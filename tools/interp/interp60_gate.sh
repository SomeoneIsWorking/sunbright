#!/usr/bin/env bash
# interp60_gate.sh — the acceptance gate for game-native 60fps interpolation (recomp path).
#
# WHAT IT ASKS. Three questions that must be answered TOGETHER, because each one alone has a
# failure mode that looks like success:
#
#   identity  alpha=1.0 must be PIXEL-IDENTICAL to a run with the substitution off.
#             A non-zero identity means the write path corrupts the frame it should reproduce.
#   control   alpha=0.0 must DIFFER. A zero control means the write reaches nothing rendered,
#             and an identity of 0 would then be measuring a no-op — the two must be read as a pair.
#   liveness  the game must still be MOVING (interp60's own `moved=` share). A frozen game
#             produces a beautifully stable, entirely wrong picture: an earlier placement scored
#             a confident 98.6% pixel difference purely because every actor had stopped.
#
# reach (--kick N) is the fourth, optional question: with a large constant displacement applied
# inside the same bracket, how much of the frame moves? The sign of `control` proves the write
# reaches SOMETHING; only reach says whether it reaches the SCENE. A tiny control with a tiny
# reach means the bracket is in the wrong place, however cleanly the pair passes.
#
# DETERMINISM IS A PRECONDITION, not a detail. Frame comparison is meaningless if two identical
# runs differ, and they did until tb_get() stopped reading the host clock (SBR_DETERMINISTIC=1
# substitutes a monotonic virtual timebase). The gate re-establishes this every time by running
# the baseline TWICE and refusing to score anything if the two disagree.
#
# Usage:
#   tools/interp/interp60_gate.sh                 # identity + control + liveness
#   tools/interp/interp60_gate.sh --kick 3000     # ...and the reach probe
#   tools/interp/interp60_gate.sh --alpha 0.5     # score an extra alpha against the baseline
#   SBR_INTERP60_BRACKET=0x200 tools/interp/interp60_gate.sh   # move the bracket, re-read the pair
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$HERE/scratch/render"
LOGS="$HERE/scratch/logs"
mkdir -p "$OUT" "$LOGS"

KICK=""
EXTRA_ALPHA=""
AFTER="${GATE_AFTER:-1500}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --kick)  KICK="$2"; shift 2 ;;
        --alpha) EXTRA_ALPHA="$2"; shift 2 ;;
        --after) AFTER="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

BIN="$HERE/build-sms-recomp/sms-recomp"
[[ -x "$BIN" ]] || { echo "GATE REFUSES: $BIN is not built. Nothing was measured." >&2; exit 1; }

# A pad script that actually MOVES the player. A gate run on a still scene cannot distinguish a
# working interpolation from a dead one — (prev) and (cur) are equal for everything on screen and
# every alpha renders the same picture. Keyed on PAD reads, so it is stable across run speeds.
PAD="${GATE_PAD:-400:STICK=0/100,1400:STICK=90/0,2200:STICK=0/100}"

run() {  # run <tag> [env assignments...]
    local tag="$1"; shift
    local dump="$OUT/gate_$tag.rgba"
    rm -f "$dump"
    ( set +e
      timeout -s KILL 180 env \
        SB_HEADLESS=1 SB_TURBO=1 \
        SBR_DETERMINISTIC=1 SBR_FASTBOOT=1 \
        SBR_PAD_SCRIPT="$PAD" \
        SB_DUMP_FRAME="$dump" SB_DUMP_FRAME_AFTER="$AFTER" \
        SBR_LUCENT_DEBUG=interp60 \
        "$@" "$HERE/run-recomp.sh" >"$LOGS/gate_$tag.log" 2>&1
      echo "$?" >"$LOGS/gate_$tag.rc" )
    local rc; rc="$(cat "$LOGS/gate_$tag.rc")"
    if [[ ! -s "$dump" ]]; then
        echo "GATE REFUSES: run '$tag' produced no frame (exit $rc). See $LOGS/gate_$tag.log" >&2
        echo "  Nothing is scored from a missing frame -- a gate that skips a failed run and" >&2
        echo "  reports on the rest is the failure mode this whole file exists to avoid." >&2
        exit 1
    fi
}

# `moved=` from interp60's own report: the liveness number. Absent means interp60 never reported,
# which is itself a finding and must not read as 0.
liveness() {
    local tag="$1"
    local line; line="$(grep -o 'compared=[0-9]* moved=[0-9]* ([0-9.]*%)' "$LOGS/gate_$tag.log" | tail -1 || true)"
    if [[ -z "$line" ]]; then echo "NO REPORT (interp60 never printed -- not the same as 0%)"
    else echo "$line"; fi
}

diff_px() {  # diff_px <a.rgba> <b.rgba> -> "<n> (<pct>%)"
    python3 - "$1" "$2" <<'PY'
import sys
a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
if len(a) != len(b) or not a:
    print(f"INCOMPARABLE (sizes {len(a)} vs {len(b)})"); raise SystemExit(0)
n = len(a) // 4
d = sum(1 for i in range(n) if a[4*i:4*i+3] != b[4*i:4*i+3])
print(f"{d:>8} of {n} ({100.0*d/n:.4f}%)")
PY
}

echo "=== interp60 gate =================================================="
echo "pad script : $PAD"
echo "dump after : $AFTER presents"
echo "bracket    : ${SBR_INTERP60_BRACKET:-0x8 (default: draw block)}"
echo

# --- determinism precondition -------------------------------------------------
run base1 SBR_INTERP60=1
run base2 SBR_INTERP60=1
DET="$(diff_px "$OUT/gate_base1.rgba" "$OUT/gate_base2.rgba")"
echo "determinism  base vs base : $DET"
if [[ "$DET" != *"       0 of"* ]]; then
    echo
    echo "GATE REFUSES: two identical runs produced different frames." >&2
    echo "  Every number below would be noise plus signal with no way to separate them." >&2
    exit 1
fi

# --- the pair -----------------------------------------------------------------
run a1 SBR_INTERP60=1 SBR_INTERP60_ALPHA=1.0
run a0 SBR_INTERP60=1 SBR_INTERP60_ALPHA=0.0
ID="$(diff_px "$OUT/gate_base1.rgba" "$OUT/gate_a1.rgba")"
CT="$(diff_px "$OUT/gate_base1.rgba" "$OUT/gate_a0.rgba")"
echo "identity     alpha=1.0     : $ID   MUST be 0"
echo "control      alpha=0.0     : $CT   MUST be > 0"
echo "liveness     alpha=1.0     : $(liveness a1)"
echo "liveness     alpha=0.0     : $(liveness a0)"

if [[ -n "$EXTRA_ALPHA" ]]; then
    run ax SBR_INTERP60=1 SBR_INTERP60_ALPHA="$EXTRA_ALPHA"
    echo "extra        alpha=$EXTRA_ALPHA   : $(diff_px "$OUT/gate_base1.rgba" "$OUT/gate_ax.rgba")"
    echo "liveness     alpha=$EXTRA_ALPHA   : $(liveness ax)"
fi

if [[ -n "$KICK" ]]; then
    run kick SBR_INTERP60=1 SBR_INTERP60_ALPHA=1.0 SBR_INTERP60_KICK="$KICK"
    echo "reach        kick=$KICK     : $(diff_px "$OUT/gate_base1.rgba" "$OUT/gate_kick.rgba")"
    echo "             (how much of the frame the bracket can move at all; a large displacement"
    echo "              that moves almost nothing means the bracket is not where the scene is built)"
fi

echo
ID_N="${ID%% of*}"; CT_N="${CT%% of*}"
if [[ "${ID_N// /}" == "0" && "${CT_N// /}" != "0" ]]; then
    echo "VERDICT: PAIR PASSES -- identity exact AND control fires."
    echo "  This says the write path is correct where it is placed. It does NOT say the bracket"
    echo "  covers the scene: read the reach probe (--kick) for that."
else
    echo "VERDICT: FAILED -- identity=$ID_N control=$CT_N"
fi

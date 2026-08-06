#!/usr/bin/env bash
# interp60_run.sh — produce a consecutive-present series from the 60fps sub-frame path, with the
# WHOLE set of switches it needs set together.
#
# WHY THIS EXISTS. The sub-frame is only VISIBLE when four things hold at once:
#
#   SBR_INTERP60=1              the sub-frame runs at all
#   SBR_INTERP60_ALPHA=<a>      a pose is substituted (unset = snapshot only, writes nothing)
#   SBR_INTERP60_COPY=1         the sub-frame copies its own EFB out to the XFB
#   SBR_PRESENT_AFTER_COPY=1    ...and the present happens after that copy, not before it
#
# Omit either of the last two and the run still succeeds, still renders, still dumps a labelled
# series -- and every "sub" dump is BIT-IDENTICAL to the main frame before it, because the display
# is showing the previously copied XFB. That reads as "the sub-frame duplicates its predecessor",
# which is a real failure mode of the interpolation and is indistinguishable from this one in the
# pixels. This project has an identical hazard documented for the native renderer ("six env vars
# set together and omitting any one fails silently and plausibly"); this is the interp60 copy of
# it, and the fix is the same: one runner that carries the set.
#
# subframe_position.py names the signature explicitly when it sees it, so a series produced by
# hand without these still gets diagnosed rather than scored.
#
# Usage:
#   tools/interp/interp60_run.sh <tag> <alpha> [extra env assignments...]
#     tools/interp/interp60_run.sh a05 0.5
#     tools/interp/interp60_run.sh a05_vc 0.5 SBR_INTERP60_PREENTRY_VC=1
#     tools/interp/interp60_run.sh base -                 # alpha "-" = substitution off (baseline)
#
# Environment knobs: DUMP_AFTER (default 2400 presents), DUMP_COUNT (7), PAD, RUN_TIMEOUT (300s).
#
# Before it scores anything the runner prints the MOTION CENSUS at the dumped moment — the per-tick
# displacement of the drawn matrices. That is the gate. Read it first; a score taken where the
# census says STATIC is a statement about the scene and not about the interpolation.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$HERE/scratch/render"
LOGS="$HERE/scratch/logs"
mkdir -p "$OUT" "$LOGS"

if [[ $# -lt 2 ]]; then
    sed -n '2,30p' "${BASH_SOURCE[0]}" >&2
    exit 2
fi
TAG="$1"; ALPHA="$2"; shift 2

BIN="$HERE/build-sms-recomp/sms-recomp"
[[ -x "$BIN" ]] || { echo "REFUSES: $BIN is not built. Nothing was run." >&2; exit 1; }

AFTER="${DUMP_AFTER:-2400}"
COUNT="${DUMP_COUNT:-7}"
# A pad script that MOVES THE CAMERA, and holds it moving for the whole run.
#
# The previous default walked the player and scored nothing. Measured with the motion census
# (per-tick displacement of the drawn matrices, the buckets printed below): walking Mario forward
# puts NOTHING in the >=100-unit bucket at any moment past the intro — the frame's per-tick change
# is his own animation plus the 2D news ticker, and neither is geometry this path covers. Holding
# the C-stick puts ~130k elements per window there, for as long as it is held:
#
#   400:STICK=0/100              (walk only)   <1e4 bucket: 0, 0, 0, ... for the whole run
#   400:CSTICK=110/0             (camera only) <1e4 bucket: ~130k every window
#   400:STICK=0/100+CSTICK=110/0 (both)        <1e4 bucket: ~125k every window   <-- this default
#
# A two-step script that releases the walk and then engages the camera (the previous session's
# `2000:STICK=0/0+CSTICK=110/0`) dies out after ~300 ticks: the camera stops responding once Mario
# is parked wherever the walk left him. Hold both from one step and it does not.
PAD="${PAD:-400:STICK=0/100+CSTICK=110/0}"

ALPHA_ENV=()
if [[ "$ALPHA" != "-" ]]; then ALPHA_ENV=("SBR_INTERP60_ALPHA=$ALPHA"); fi

PREFIX="$OUT/i60_$TAG.rgba"
rm -f "$PREFIX".* 2>/dev/null || true

echo "=== interp60 series '$TAG' (alpha=$ALPHA) ==="
echo "  dump   : $PREFIX.<n>.<main|sub>, every present, $COUNT of them, from present $AFTER"
echo "  pad    : $PAD"
echo "  extra  : ${*:-<none>}"

set +e
timeout -s KILL "${RUN_TIMEOUT:-300}" env \
    SB_HEADLESS=1 SB_TURBO=1 \
    SBR_DETERMINISTIC=1 SBR_FASTBOOT=1 \
    SBR_PAD_SCRIPT="$PAD" \
    SBR_INTERP60=1 \
    SBR_INTERP60_CENSUS=1 \
    SBR_INTERP60_COPY=1 \
    SBR_PRESENT_AFTER_COPY=1 \
    "${ALPHA_ENV[@]}" \
    SB_DUMP_FRAME="$PREFIX" SB_DUMP_FRAME_AFTER="$AFTER" \
    SB_DUMP_FRAME_EVERY=1 SB_DUMP_FRAME_COUNT="$COUNT" \
    SBR_INTERP60_CAMTRACE=1 SBR_INTERP60_CAMFAST=0 SBR_INTERP60_VIEWSEQ_AT="$AFTER" \
    SBR_LUCENT_DEBUG=interp60 \
    "$@" "$HERE/run-recomp.sh" >"$LOGS/i60_$TAG.log" 2>&1
RC=$?
set -e

N=$(ls "$PREFIX".* 2>/dev/null | wc -l)
if [[ "$N" -lt 3 ]]; then
    echo "REFUSES: run '$TAG' produced $N dump(s) (exit $RC); a triple needs 3." >&2
    echo "  Nothing is scored from a short series. See $LOGS/i60_$TAG.log" >&2
    exit 1
fi
echo "  produced $N dumps (exit $RC)"

# LIVENESS AT THE DUMPED MOMENT — the precondition, not a footnote. If the drawn geometry is not
# moving there, alpha provably cannot change a pixel and a score of "the sub-frame duplicates its
# follower" is a fact about the scene rather than about the interpolation. This project has paid for
# that mistake more than once: a fast test moment silently moved a measurement into a pre-gameplay
# window and four `eye moved 0.000` samples became a wrong root cause; and three runs at three
# alphas once produced BYTE-IDENTICAL scores because the scene was static and nothing said so.
#
# THE PRIMARY GATE IS THE MOTION CENSUS, NOT THE CAMERA PROBE.
#
# Every camera probe in this arc has been shown to be BLIND (commit 1c59a30): CAMTRACE follows
# g_camObj 0x81588cd0, whose eye moves only during the first ~90 presents and reads 0.000 for the
# whole of gameplay while the viewpoint visibly changes, and MTXTRACE auto-pins a model rendering
# under the MIRROR view. Both print "the camera did not move", and both mean "the object I watch
# did not move" — a negative that reads exactly like a finding.
#
# The census watches no named object. It buckets |cur-prev| over the translation of every draw
# matrix recorded that tick — the matrices the hardware is about to read — so "nothing moved" from
# it is a statement about the drawn geometry and cannot be an artefact of watching the wrong thing.
# The CAMTRACE block below is kept, demoted to a footnote, because it is still the only line that
# separates camera motion from actor motion when it does fire.
# AT THE DUMPED MOMENT, not at the end of the run. The run continues to its timeout long after the
# dump, and this arc has already published a reading taken from a window the dump was nowhere near
# (the FOUR CLOCKS entry, commit 1c59a30). Guest tick = present / 2, so the window that covers the
# dump is the census line whose tick is nearest AFTER/2.
CENSUS="$(grep -a 'MOTION CENSUS' "$LOGS/i60_$TAG.log" |
          awk -v want=$((AFTER / 2)) '
            { if (match($0, /@ tick [0-9]+/)) { t = substr($0, RSTART+7, RLENGTH-7)+0
                d = t > want ? t - want : want - t
                if (best == "" || d < best) { best = d; line = $0 } } }
            END { if (line != "") print line }' || true)"
echo "  motion census (drawn-matrix displacement per tick) AT THE DUMPED MOMENT (guest tick ~$((AFTER / 2))):"
if [[ -z "$CENSUS" ]]; then
    echo "    NO CENSUS LINES. The census did not run, so this run cannot say whether the scene"
    echo "    was moving. That is NOT 'the scene was static'. Nothing below should be read."
else
    echo "$CENSUS" | sed -E 's/^\[i60r\] /    /; s/ THE VERDICT IS .*//'
    if echo "$CENSUS" | grep -q "STATIC: not one drawn matrix"; then
        echo "    ^ STATIC. The drawn geometry does not move at this moment, so no interpolation"
        echo "      of it can change a pixel and the score below describes the SCENE. Change PAD."
    fi
fi
echo

CAM="$(grep -a 'CAMTRACE present' "$LOGS/i60_$TAG.log" | head -4 || true)"
echo "  camera liveness at the dumped moment (|eye cur-prev| per tick):"
if [[ -z "$CAM" ]]; then
    echo "    NO CAMTRACE LINES — the camera probe never fired at present >= $AFTER."
    echo "    That is NOT 'the camera was still': it means this run cannot say either way."
else
    echo "$CAM" | sed 's/^/    /'
    if ! echo "$CAM" | grep -qv '|eye cur-prev|=0\.000'; then
        echo "    ^ this probe reports the camera parked. DO NOT read that as a fact about the"
        echo "      scene: g_camObj is not the active gameplay camera (commit 1c59a30), so this"
        echo "      line means only that the watched object did not move. The census above is the"
        echo "      gate; this line is informative ONLY when it reports motion."
    fi
fi
echo
exec python3 "$HERE/tools/interp/subframe_position.py" "$PREFIX"

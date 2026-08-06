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
# Environment knobs: DUMP_AFTER (default 1500 presents), DUMP_COUNT (7), PAD, RUN_TIMEOUT (300s).
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

AFTER="${DUMP_AFTER:-1500}"
COUNT="${DUMP_COUNT:-7}"
# A pad script that MOVES the player. A series taken on a still scene scores as a perfect midpoint
# no matter what the interpolation does, because prev == next and there is nothing to be between.
PAD="${PAD:-400:STICK=0/100,1400:STICK=90/0,2200:STICK=0/100}"

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

# LIVENESS AT THE DUMPED MOMENT — the precondition, not a footnote.
#
# The only thing this configuration interpolates by default is the CAMERA, so if the camera is
# parked across the dumped presents then alpha provably cannot change a pixel and a score of "the
# sub-frame duplicates its follower" is a fact about the scene, not about the interpolation. The
# scene can be moving hard (Mario walking fills 40% of the frame) while the camera stands still,
# so the frame-level "moment scale" cannot answer this and a separate number is needed.
#
# This project has already paid for the same mistake once: a fast test moment was chosen for speed
# and silently moved the measurement to a pre-gameplay window with a static camera, and four
# `eye moved 0.000` samples became a wrong root cause. SBR_INTERP60_CAMTRACE is armed at the dump
# present for exactly this reason.
CAM="$(grep -a 'CAMTRACE present' "$LOGS/i60_$TAG.log" | head -4 || true)"
echo "  camera liveness at the dumped moment (|eye cur-prev| per tick):"
if [[ -z "$CAM" ]]; then
    echo "    NO CAMTRACE LINES — the camera probe never fired at present >= $AFTER."
    echo "    That is NOT 'the camera was still': it means this run cannot say either way."
else
    echo "$CAM" | sed 's/^/    /'
    if ! echo "$CAM" | grep -qv '|eye cur-prev|=0\.000'; then
        echo "    ^ THE CAMERA IS PARKED HERE. The camera is the only thing this configuration"
        echo "      interpolates, so alpha cannot reach a pixel at this moment and the score below"
        echo "      describes the SCENE, not the interpolation. Move the dump, or add the actor"
        echo "      seams (SBR_INTERP60_ACTORS / _PLAYER / _ANIM), before reading it."
    fi
fi
echo
exec python3 "$HERE/tools/interp/subframe_position.py" "$PREFIX"

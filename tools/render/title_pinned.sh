#!/usr/bin/env bash
# title_pinned.sh — REAL LOCKSTEP scene-sync at the title screen.
#
# Goal (nifty-chasing-bachman.md): before any pixel-delta claim, prove native
# (build-native/sms-boot) and oracle (build/sunbright) rendered the SAME game
# state on the SAME game tick. Every SBS before this script existed diffed two
# independently-settled captures at unrelated states.
#
# Contract:
#   1. Fastboot both sides to stage 15 with EMPTY pad script (title screen,
#      pre-Press-Start — the strictly simplest scene).
#   2. Both sides pin at PIN_TICK (VI/present-frame equality, not a settle
#      detector) — camera intro is asymptotic so PIN_TICK sits well past
#      convergence (default 900 ≈ 15 s at 60 Hz, past all 4 CameraOption phases).
#   3. Each side emits scratch/frames/pin_<N>[_oracle].json + .png + .done.
#   4. tools/render/pin_diff.py asserts view/proj/lights match within tolerance;
#      exits non-zero on RED — DO NOT trust a pixel diff on an unpinned pair.
#   5. On GREEN, produce an SBS PNG. The pixel delta from that PNG is the first
#      trustworthy renderer-only divergence number at title.
#
# See handoff notes in <home>/.claude/plans/nifty-chasing-bachman.md.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"
source .env 2>/dev/null || { echo "no .env (need SUNBRIGHT_ROM)"; exit 1; }

PIN_TICK="${PIN_TICK:-900}"
OUT="scratch/frames"
mkdir -p "$OUT" scratch/passes

# Clean stale pin artifacts so the .done markers can't produce false positives from a prior run.
rm -f "$OUT/pin_$(printf %04d "$PIN_TICK")".{json,done,png,ppm} \
      "$OUT/pin_$(printf %04d "$PIN_TICK")_oracle".{json,done,png}

# Real lockstep is impossible with pkill-based cleanup: leftover processes carry
# stale save state. Use SIGINT + short escalation to SIGKILL per feedback
# `feedback-fix-timeout-with-sigint-dont-sidestep`, and always `wait` explicitly.
kill_stale() {
    local name=$1
    pkill -x "$name" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
        pgrep -x "$name" >/dev/null 2>&1 || return 0
        sleep 0.2
    done
    pkill -9 -x "$name" 2>/dev/null || true
    sleep 0.3
}

echo "== title_pinned: PIN_TICK=$PIN_TICK =="
echo

# ============================================================================
# [1/3] Oracle side — build/sunbright, Dolphin-GX. WIDESCREEN=0 to disable
# runtime/overrides/scene_render.cpp's m[0][0]*=0.75 squeeze so both engines
# produce raw 4:3 proj matrices from identical game code. Empty pad script —
# we want the actual title screen, NOT the file-select.
# ============================================================================
echo "[1/3] ORACLE — build/sunbright  (fastboot stage 15, empty pad, PIN_TICK=$PIN_TICK)"
kill_stale sunbright

env SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_BACKEND=Vulkan \
    SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_SCENARIO=0 \
    SUNBRIGHT_PAD_SCRIPT="" \
    SUNBRIGHT_WIDESCREEN=0 \
    SUNBRIGHT_PROBE=1 \
    SUNBRIGHT_PARITY_DUMP=scratch/passes/pinned_oracle.jsonl \
    SUNBRIGHT_PIN_TICK="$PIN_TICK" \
    ./build/sunbright "$SUNBRIGHT_ROM" > scratch/passes/pinned_oracle.log 2>&1 &
OPID=$!

# Poll for the oracle probe to be listening + the .done marker to appear.
ORACLE_DONE="$OUT/pin_$(printf %04d "$PIN_TICK")_oracle.done"
ORACLE_JSON="$OUT/pin_$(printf %04d "$PIN_TICK")_oracle.json"
for i in $(seq 1 60); do   # up to 60 s
    if [[ -s "$ORACLE_DONE" ]]; then break; fi
    if ! kill -0 "$OPID" 2>/dev/null; then
        echo "  ORACLE died before pin fired — last log lines:" >&2
        tail -30 scratch/passes/pinned_oracle.log >&2
        exit 1
    fi
    sleep 1
done
if [[ ! -s "$ORACLE_DONE" ]]; then
    echo "  ORACLE pin TIMEOUT ($ORACLE_DONE never appeared)" >&2
    tail -30 scratch/passes/pinned_oracle.log >&2
    kill_stale sunbright
    exit 1
fi
# Ask the probe to save a PNG at the pinned state. Oracle is still running at
# turbo; title is quiescent post-settle so the frame captured a few ms after
# the JSON write is the same visible state.
curl -s -m 10 "http://127.0.0.1:17654/pinshot?vi=$PIN_TICK&timeout_s=5" >/dev/null || true
ORACLE_PNG="$OUT/pin_$(printf %04d "$PIN_TICK")_oracle.png"
kill_stale sunbright
wait "$OPID" 2>/dev/null || true
echo "  oracle JSON $([[ -s "$ORACLE_JSON" ]] && echo present || echo MISSING)"
echo "  oracle PNG  $([[ -s "$ORACLE_PNG" ]]  && echo present || echo missing)"
echo

# ============================================================================
# [2/3] Native side — build-native/sms-boot. SB_PIN_TICK=N triggers a one-shot
# dump of scratch/frames/pin_<N>.ppm + pin_<N>.json + pin_<N>.done at present
# frame == N. No settle detector — deterministic tick equality.
# ============================================================================
echo "[2/3] NATIVE — build-native/sms-boot  (fastboot stage 15, empty pad, PIN_TICK=$PIN_TICK)"
kill_stale sms-boot

NATIVE_DONE="$OUT/pin_$(printf %04d "$PIN_TICK").done"
NATIVE_JSON="$OUT/pin_$(printf %04d "$PIN_TICK").json"
NATIVE_PPM="$OUT/boot_$(printf %04d "$PIN_TICK").ppm"

setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
    SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 \
    SB_PAD_SCRIPT="" \
    SB_FRAME_DUMP=1 SB_FRAME_DUMP_MAX=2 \
    SB_PIN_TICK="$PIN_TICK" \
    SB_WATCHDOG_SECS="${SB_WATCHDOG_SECS:-0}" \
    ./build-native/sms-boot > scratch/passes/pinned_native.log 2>&1 &
NPID=$!

for i in $(seq 1 90); do   # up to 90 s
    if [[ -s "$NATIVE_DONE" ]]; then break; fi
    if ! kill -0 "$NPID" 2>/dev/null; then
        echo "  NATIVE died before pin fired — last log lines:" >&2
        tail -30 scratch/passes/pinned_native.log >&2
        exit 1
    fi
    sleep 1
done
if [[ ! -s "$NATIVE_DONE" ]]; then
    echo "  NATIVE pin TIMEOUT ($NATIVE_DONE never appeared)" >&2
    tail -30 scratch/passes/pinned_native.log >&2
    kill_stale sms-boot
    exit 1
fi
kill_stale sms-boot
wait "$NPID" 2>/dev/null || true
echo "  native JSON $([[ -s "$NATIVE_JSON" ]] && echo present || echo MISSING)"
echo "  native PPM  $([[ -s "$NATIVE_PPM" ]]  && echo present || echo missing)"
echo

# ============================================================================
# [3/3] Assert scene sync. If RED, no pixel diff — the number would lie.
# ============================================================================
echo "[3/3] pin_diff — cross-engine state parity assertion"
python3 tools/render/pin_diff.py "$PIN_TICK"
RC=$?
if [[ $RC -ne 0 ]]; then
    echo
    echo "STATE PARITY RED (rc=$RC). Fix scene-sync before comparing pixels."
    exit "$RC"
fi
echo
echo "STATE PARITY GREEN — pin bundle at $OUT/pin_$(printf %04d "$PIN_TICK")*."
echo "Bundle: $NATIVE_JSON  $NATIVE_PPM  $ORACLE_JSON  $ORACLE_PNG"

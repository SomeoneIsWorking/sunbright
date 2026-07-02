#!/usr/bin/env bash
# fileselect_pinned.sh — SAME-STATE PINNED file-select parity capture.
#
# Runs the Dolphin-GX oracle (build/sunbright) and the native renderer
# (build-native/sms-boot) with:
#   • identical SUNBRIGHT_FASTBOOT / SB_FASTBOOT path — both fastboot to stage 15 (file-select)
#   • identical SUNBRIGHT_PAD_SCRIPT / SB_PAD_SCRIPT — same VI-indexed pad input
#   • identical SUNBRIGHT_PARITY_DUMP_FROM / SB_FRAME_DUMP_START — same
#     GXCopyDisp/present-count floor for parity emission
# so the JSONL captures both engines produce cover the SAME game state.
#
# The parity_sweep drawdiff STATE-UNPINNED banner remains — this script pins
# what CAN be pinned deterministically today (input schedule + emission floor);
# proving the pinned game states are actually bit-equivalent will require an
# additional in-game camera-settle detector on both sides (WIP).
#
# Prereqs: .env (SUNBRIGHT_ROM), scratch/disc/sms.iso, scratch/memcard_chan0.raw.
# Cite (per 2026-07-02 workflow directive): tools/render/parity_sweep.py,
# runtime/gx_capture.cpp (SUNBRIGHT_PARITY_DUMP_FROM), runtime/main_sdl.cpp
# (SUNBRIGHT_PAD_SCRIPT), runtime/overrides/fastboot_native.cpp.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"
source .env 2>/dev/null || { echo "no .env (need SUNBRIGHT_ROM)"; exit 1; }
mkdir -p scratch/passes

# Same input on both sides. Empty script (just fastboot lands on file-select).
# Add START pulses if the game needs to click through additional dialogs — both
# engines get the same schedule so their game state stays aligned.
PAD_SCRIPT="${PAD_SCRIPT:-}"
# Same emission floor on both sides. Under TURBO both engines reach ~500 frames
# of settled file-select within seconds; FROM=250 discards the intro/settle pan.
FROM="${FROM:-250}"

ORACLE=scratch/passes/pinned_oracle.jsonl
NATIVE=scratch/passes/pinned_native.jsonl

echo "[1/3] ORACLE — build/sunbright (fastboot stage 15, FROM=$FROM)"
pkill -9 -x sunbright 2>/dev/null; sleep 1; rm -f "$ORACLE"
timeout -s KILL 60 env SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_BACKEND=Vulkan \
  SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_SCENARIO=0 \
  SUNBRIGHT_PAD_SCRIPT="$PAD_SCRIPT" \
  SUNBRIGHT_WIDESCREEN=0 \
  SUNBRIGHT_PARITY_DUMP="$ORACLE" SUNBRIGHT_PARITY_DUMP_FROM="$FROM" SUNBRIGHT_PARITY_DRAWS=1 \
  ./build/sunbright "$SUNBRIGHT_ROM" > scratch/passes/pinned_oracle.log 2>&1
# SUNBRIGHT_WIDESCREEN=0 disables runtime/overrides/scene_render.cpp's ws_squeeze m[0][0]*=0.75.
# The oracle's default 16:9 squeeze widens the 3D projection horizontally — a REAL divergence from
# native (which doesn't apply that squeeze at all in the sms-boot renderer). For state-pin parity
# both engines must run RAW 4:3 so their proj matrices come from identical game code with no
# post-hoc squeeze.
echo "    oracle lines: $(wc -l < "$ORACLE" 2>/dev/null || echo 0)"

echo "[2/3] NATIVE — build-native/sms-boot (fastboot stage 15, START=$FROM)"
pkill -9 -x sms-boot 2>/dev/null; sleep 1; rm -f "$NATIVE"
timeout -s KILL 60 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
  SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 \
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_START="$FROM" \
  SB_WATCHDOG_SECS="${SB_WATCHDOG_SECS:-0}" \
  SB_PAD_SCRIPT="$PAD_SCRIPT" \
  SB_PARITY_DUMP="$NATIVE" \
  ./build-native/sms-boot > scratch/passes/pinned_native.log 2>&1 &
NPID=$!; sleep 55; pkill -9 -x sms-boot 2>/dev/null; wait $NPID 2>/dev/null
echo "    native lines: $(wc -l < "$NATIVE" 2>/dev/null || echo 0)"

echo "[3/3] drawdiff (per-draw ordered NAMED divergence)"
python3 tools/render/parity_sweep.py drawdiff "$ORACLE" "$NATIVE"

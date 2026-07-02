#!/usr/bin/env bash
# title_value_oracle.sh — VALUE-track parity for the stage-15 title screen screen.
#
# Captures the per-frame GX-command-stream parity JSON from BOTH engines at title screen and runs the
# cross-engine diff (tools/render/parity_sweep.py). This is the VALUE track (lights/projType/geometry
# aggregate) — the pixel track is title_oracle.sh.
#
#   ORACLE (ground truth)  : build/sunbright (pure Dolphin-GX) → gx_capture.cpp → SUNBRIGHT_PARITY_DUMP
#   NATIVE (the product)   : build-native/sms-boot → sb_parity_dump.h → SB_PARITY_DUMP
#
# Both fastboot to STAGE 15. The oracle is driven to leave PRESS-START via the probe /pad; sms-boot
# uses a pad script. HEADLESS + TURBO (no window — user directive). Prereqs: .env (ROM),
# scratch/disc/sms.iso, a formatted scratch/memcard_chan0.raw (auto-made on first sms-boot run).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"
source .env 2>/dev/null || { echo "no .env (need SUNBRIGHT_ROM)"; exit 1; }
mkdir -p scratch/passes
ORACLE=scratch/passes/fs_oracle.jsonl
NATIVE=scratch/passes/fs_native.jsonl

echo "[1/3] ORACLE — build/sunbright pure Dolphin-GX → $ORACLE"
pkill -9 -x sunbright 2>/dev/null; sleep 1; rm -f "$ORACLE"
( timeout -s KILL 150 env SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_BACKEND=Vulkan \
    SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_SCENARIO=0 SUNBRIGHT_PROBE=1 \
    SUNBRIGHT_PARITY_DUMP="$ORACLE" ./build/sunbright "$SUNBRIGHT_ROM" \
    > scratch/passes/fs_oracle.log 2>&1 ) &
for i in $(seq 1 30); do sleep 2; curl -s --max-time 2 http://127.0.0.1:17654/metrics >/dev/null 2>&1 && break; done
curl -s --max-time 5 "http://127.0.0.1:17654/pad?do=start&ms=250" >/dev/null; sleep 2
curl -s --max-time 5 "http://127.0.0.1:17654/pad?do=start&ms=250" >/dev/null
sleep 28; pkill -9 -x sunbright 2>/dev/null; sleep 1
echo "    oracle frames: $(wc -l < "$ORACLE" 2>/dev/null || echo 0)"

echo "[2/3] NATIVE — build-native/sms-boot → $NATIVE"
pkill -9 -x sms-boot 2>/dev/null; sleep 1; rm -f "$NATIVE"
S=""; for f in $(seq 200 12 1690); do S="$S ${f}:START $((f+6)):-"; done
# SB_OWN_GXLIST=1: measure the geometry-COMPLETE foundation — the REAL master GX perform-list render
# (TMarDirector::direct). The hand-driven default scene_drive path is structurally incomplete (only
# draws 通常シーン's 3 children: conductor/light-mgr/perf-group), topping out at ~6.5k of the 22.7k
# scene verts; SB_OWN_GXLIST closes the geometry gap to relΔ 0.02 (the map/manager/player groups the
# master perform list draws into the GLOBAL draw buffers). See
# debug_journal/2026-06-30_fileselect_geometry_gap_is_ownlist.md. Override with SB_OWN_GXLIST=0 to A/B
# the hand-driven path. The residual overbright on this foundation is the next divergence to own.
timeout -s KILL 200 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
  SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_OWN_GXLIST="${SB_OWN_GXLIST:-1}" \
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_ON_SCENE=1 SB_PARITY_DUMP="$NATIVE" \
  SB_PAD_SCRIPT="$S" ./build-native/sms-boot > scratch/passes/fs_native.log 2>&1 &
NPID=$!
# sms-boot dumps every frame once on-scene; let it reach + settle title screen, then stop.
sleep 150; pkill -9 -x sms-boot 2>/dev/null; wait $NPID 2>/dev/null
echo "    native frames: $(wc -l < "$NATIVE" 2>/dev/null || echo 0)"

echo "[3/3] DIFF (cross-engine value summary)"
python3 tools/render/parity_sweep.py diff "$ORACLE" "$NATIVE"

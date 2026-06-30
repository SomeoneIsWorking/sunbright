#!/usr/bin/env bash
# gxstate_diff.sh — capture the per-draw GX state from BOTH engines at the settled file-select
# and run the deterministic native-vs-oracle GX-state diff (tools/render/gxstate_diff.py).
#
# This ends the manual pass/phase log-reading oscillation: it groups every draw by GX-state
# SIGNATURE (blend src/dst/subtract + TEV stage count) and reports each engine's colorUpdate
# value-set per signature. A signature whose colorUpdate differs IS a per-draw divergence.
#
#   ORACLE = build/sunbright (Dolphin-GX ground truth) + SUNBRIGHT_DBG_GXDRAW=1, frames 4-7.
#   NATIVE = build-native/sms-boot (nvk) + SB_GXDRAW=1 + SB_OWN_GXLIST=1, a settled frame.
#
# Usage: tools/render/gxstate_diff.sh
# Output: scratch/passes/gxdraw_{oracle,native}_frame.log + the diff table on stdout.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"
source .env 2>/dev/null || true
PASS=scratch/passes; mkdir -p "$PASS"

pkill -9 -x sunbright 2>/dev/null; pkill -9 -x sms-boot 2>/dev/null; sleep 1

echo "[gxstate] capturing ORACLE (Dolphin-GX, headless, frames 4-7)…"
timeout -s KILL 180 setarch -R env \
  SUNBRIGHT_ROM="$SUNBRIGHT_ROM" SUNBRIGHT_HEADLESS=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_SCENARIO=0 \
  SUNBRIGHT_NGX_PRESENT=0 SUNBRIGHT_PARITY_DUMP="$PASS/parity_gxdraw.json" \
  SUNBRIGHT_DBG_GXDRAW=1 SUNBRIGHT_TURBO=1 \
  build/sunbright > "$PASS/oracle_gxdraw.log" 2>&1 &
OPID=$!
# Frames 4-7 arrive early; wait until frame 7 is dumped, then stop.
for i in $(seq 1 60); do sleep 2; grep -q "\[gxdraw\] fr=7 " "$PASS/oracle_gxdraw.log" 2>/dev/null && break; done
pkill -9 -x sunbright 2>/dev/null; wait $OPID 2>/dev/null
grep "\[gxdraw\] fr=6 " "$PASS/oracle_gxdraw.log" > "$PASS/gxdraw_oracle_frame.log"
echo "[gxstate] oracle frame 6: $(wc -l < "$PASS/gxdraw_oracle_frame.log") draws"

echo "[gxstate] capturing NATIVE (nvk, SB_OWN_GXLIST, settled)…"
PAD=""; for f in $(seq 200 12 560); do PAD="$PAD ${f}:START $((f+6)):-"; done
timeout -s KILL 280 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
  SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_OWN_GXLIST=1 SB_GXDRAW=1 \
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_SETTLE=1 SB_FRAME_DUMP_MAX=2 \
  SB_PAD_SCRIPT="$PAD" ./build-native/sms-boot > "$PASS/native_gxdraw.log" 2>&1 &
NPID=$!
# Let it settle past the intro pan; grab a late, complete frame.
sleep 200; pkill -9 -x sms-boot 2>/dev/null; wait $NPID 2>/dev/null
# Pick the 2nd-to-last fully-emitted frame (the very last may be truncated by the kill).
LASTFR=$(grep -o "\[gxdraw\] fr=[0-9]*" "$PASS/native_gxdraw.log" | tail -200 | sort -un -t= -k2 | tail -2 | head -1 | grep -o '[0-9]*')
grep "\[gxdraw\] fr=$LASTFR " "$PASS/native_gxdraw.log" > "$PASS/gxdraw_native_frame.log"
echo "[gxstate] native frame $LASTFR: $(wc -l < "$PASS/gxdraw_native_frame.log") batches"

echo "[gxstate] === DIFF ==="
python3 tools/render/gxstate_diff.py "$PASS/gxdraw_oracle_frame.log" "$PASS/gxdraw_native_frame.log"

#!/usr/bin/env bash
# title_drawdiff.sh — fast per-draw parity diff for the title screen (stage 15).
#
# Uses tools/render/oracle_cache.sh so the oracle JSONL is reused across runs until
# the oracle binary or capture code changes. Native captures fresh every call
# (it's the thing under iteration). Runs parity_sweep.py drawdiff at the end —
# emits the STATE-MATCH banner + per-signature-bucket table.
#
# Usage: tools/render/title_drawdiff.sh [native_settle_secs=40]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"
SETTLE="${1:-40}"

ORACLE="$(bash tools/render/oracle_cache.sh)" || exit 1
NATIVE=scratch/passes/dd_native.jsonl

echo "[title-drawdiff] capturing native (${SETTLE}s) → $NATIVE"
pkill -9 -x sms-boot 2>/dev/null; sleep 1; rm -f "$NATIVE"
[ -x "build-native/sms-boot" ] || { echo "build-native/sms-boot missing"; exit 1; }
timeout -s KILL "$((SETTLE + 5))" setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso \
    SB_THP_FAST=1 SB_TURBO=1 SB_HOST_ALLOC_CAP_MB=3072 \
    SB_STAGE=15 SB_SCENARIO=0 SB_OWN_GXLIST=1 \
    SB_FRAME_DUMP=1 SB_FRAME_DUMP_ON_SCENE=1 SB_WATCHDOG_SECS=0 \
    SB_PARITY_DUMP="$NATIVE" SB_PARITY_DRAWS=1 \
    ./build-native/sms-boot > scratch/passes/dd_native.log 2>&1 &
NPID=$!
sleep "$SETTLE"
pkill -9 -x sms-boot 2>/dev/null
wait $NPID 2>/dev/null
echo "[title-drawdiff] native frames: $(wc -l < "$NATIVE" 2>/dev/null || echo 0)"

echo "----------------------------------------------------------------"
python3 tools/render/parity_sweep.py drawdiff "$ORACLE" "$NATIVE"

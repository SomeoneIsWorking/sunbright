#!/usr/bin/env bash
# parity_run.sh — one-command VALUE-track parity gate for the sms-boot native engine.
# Fastboots to Delfino gameplay, dumps a window of frames + the parity JSONL, then runs
# `parity_sweep.py check`. Optionally `diff` against a saved baseline.
#
#   tools/render/parity_run.sh                       # run + check, dump -> scratch/frames/parity.jsonl
#   tools/render/parity_run.sh baseline.jsonl        # also diff the new dump vs baseline.jsonl
#
# Env overrides: START (first dumped frame, default 255), N (frame count, 6), SECS (timeout, 70),
#   DISC (scratch/disc/sms.iso), OUT (scratch/frames/parity.jsonl).
set -u
cd "$(dirname "$0")/../.." || exit 1   # repo root

START=${START:-255}; N=${N:-6}; SECS=${SECS:-70}
DISC=${DISC:-scratch/disc/sms.iso}
OUT=${OUT:-scratch/frames/parity.jsonl}
BIN=${BIN:-./build-native/sms-boot}
BASELINE=${1:-}

[ -x "$BIN" ] || { echo "no $BIN — build sms-boot first (cmake --build build-native --target sms-boot)"; exit 2; }
[ -f "$DISC" ] || { echo "no disc image at $DISC (set DISC=...)"; exit 2; }

pkill -9 -x sms-boot 2>/dev/null; sleep 1
mkdir -p scratch/frames
echo "[parity_run] fastboot $BIN, dump frames $START..$((START+N-1)) -> $OUT"
timeout -s KILL "$SECS" setarch -R env \
  SUNBRIGHT_DISC="$DISC" SB_THP_FAST=1 SB_TURBO=1 SB_HOST_ALLOC_CAP_MB=3072 \
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_START="$START" SB_FRAME_DUMP_MAX="$N" \
  SB_PARITY_DUMP="$OUT" "$BIN" > scratch/frames/parity_run.log 2>&1
echo "[parity_run] engine exited; $(wc -l < "$OUT" 2>/dev/null || echo 0) frames dumped"

echo; echo "[parity_run] === check ==="
python3 tools/render/parity_sweep.py check "$OUT"; rc=$?

if [ -n "$BASELINE" ] && [ -f "$BASELINE" ]; then
    echo; echo "[parity_run] === diff vs $BASELINE ==="
    python3 tools/render/parity_sweep.py diff "$BASELINE" "$OUT"
fi
exit $rc

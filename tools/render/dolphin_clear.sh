#!/usr/bin/env bash
# dolphin_clear.sh — reset all Dolphin/sunbright oracle state before a fresh capture run.
#
# A stale process squatting the probe port (17654) makes /metrics answer IMMEDIATELY on the
# next launch even though the new process hasn't booted yet — that false-fast "probe up" reading
# silently desyncs pad-press timing from actual game state (this is what caused a mistimed Start
# press to fall through the picker straight into gameplay during the 2026-07-01 tri-count-gap
# session). Leftover frame dumps from a prior run are also easy to misattribute to the current
# run when eyeballing "the latest PNG". Always run this immediately before launching build/sunbright
# for a fresh, deterministic oracle capture.
set -uo pipefail

echo "[dolphin-clear] killing any sunbright process..."
pkill -9 -x sunbright 2>/dev/null
sleep 1

echo "[dolphin-clear] confirming probe port 17654 is free..."
for i in $(seq 1 10); do
  curl -s --max-time 1 http://127.0.0.1:17654/metrics >/dev/null 2>&1 || { echo "[dolphin-clear] port free"; break; }
  echo "[dolphin-clear]   still answering, waiting..."
  pkill -9 -x sunbright 2>/dev/null
  sleep 1
done

DUMP="$HOME/.local/share/dolphin-emu/Dump/Frames"
if [ -d "$DUMP" ]; then
  n=$(ls -1 "$DUMP" 2>/dev/null | wc -l)
  rm -f "$DUMP"/*.png 2>/dev/null
  echo "[dolphin-clear] cleared $n stale frame dump(s) from $DUMP"
fi

echo "[dolphin-clear] done — safe to launch a fresh build/sunbright run."

#!/usr/bin/env bash
# fileselect_oracle.sh — capture the Dolphin-GX ground-truth render of the stage-15
# file-select (option.arc) screen, the oracle for sms-boot's native file-select fidelity.
#
# HOW IT WORKS: the MAIN `sunbright` build runs the real game under Dolphin's JIT. With
# SUNBRIGHT_NGX_PRESENT=0 the native renderer is OFF, so Dolphin's GX path produces the
# frame = ground truth (engine overrides are inert in that path). SUNBRIGHT_STAGE=15 fastboots
# to the option scene; it stops at the "PRESS START" title, so we press Start ONCE via the
# probe /pad endpoint (NOT SUNBRIGHT_AUTOSTART — that keeps pulsing and auto-selects a file,
# blowing past the settled file-select into the opening movie). Then the intro camera settles
# on the 3-block picker and waits there forever (no further input), so we dump a settled frame.
#
# Output: scratch/oracle/fileselect_gx_oracle.png (full-res GX frame).
# Compare against sms-boot: build-native/sms-boot with SB_STAGE=15 (see fileselect_smsboot.png).
#
# Usage: tools/render/fileselect_oracle.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
DUMP="$HOME/.local/share/dolphin-emu/Dump/Frames"
OUT="scratch/oracle/fileselect_gx_oracle.png"
mkdir -p scratch/oracle

pkill -9 -x sunbright 2>/dev/null; sleep 1
rm -f "$DUMP"/*.png 2>/dev/null

echo "[oracle] launching main build → stage 15 (Dolphin-GX, probe, dump)…"
SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_SCENARIO=0 SUNBRIGHT_NGX_PRESENT=0 \
  SUNBRIGHT_PROBE=1 SUNBRIGHT_DUMP=1 SUNBRIGHT_BIN=build/sunbright \
  timeout -s KILL 240 ./run.sh > scratch/frames/fileselect_oracle.log 2>&1 &

# Wait for the probe (boot reaches the title in ~40s wall at ~0.24x emu speed).
for i in $(seq 1 40); do
  sleep 3
  if curl -s --max-time 3 http://127.0.0.1:17654/metrics >/dev/null 2>&1; then
    es=$(curl -s --max-time 3 http://127.0.0.1:17654/metrics | grep -o '"emu_secs":[^,]*')
    echo "[oracle] probe up, $es"
    [ "$(curl -s http://127.0.0.1:17654/metrics | grep -o '"emu_secs": [0-9]*' | grep -o '[0-9]*')" -ge 14 ] 2>/dev/null && break
  fi
done

echo "[oracle] pressing START to leave PRESS-START…"
curl -s --max-time 5 "http://127.0.0.1:17654/pad?do=start&ms=250" >/dev/null; sleep 2
curl -s --max-time 5 "http://127.0.0.1:17654/pad?do=start&ms=250" >/dev/null

echo "[oracle] letting the intro camera settle (~50s)…"
sleep 55

# Grab the newest fully-written frame (skip the last few — may be mid-write).
python3 - "$DUMP" "$OUT" <<'PY'
import sys, glob, os
from PIL import Image
d, out = sys.argv[1], sys.argv[2]
fs = sorted(glob.glob(os.path.join(d,'framedump_*.png')),
            key=lambda f:int(f.split('_')[-1].split('.')[0]))
for f in reversed(fs[:-3]):
    try:
        im = Image.open(f); im.load(); im.convert('RGB').save(out)
        print('[oracle] saved', out, 'from', os.path.basename(f), im.size); break
    except Exception:
        pass
else:
    print('[oracle] FAILED — no complete frame found'); sys.exit(1)
PY

pkill -9 -x sunbright 2>/dev/null
echo "[oracle] done."

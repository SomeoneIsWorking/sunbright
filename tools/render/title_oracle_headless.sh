#!/usr/bin/env bash
# title_oracle_headless.sh — capture the Dolphin-GX ground-truth render of the settled
# stage-15 title screen 3-block picker, HEADLESS (no window, no ./run.sh). Companion to the
# windowed tools/render/title_oracle.sh — use this one when there's no display / the user
# doesn't want a headed run.
#
# Recipe matches tools/render/title_value_oracle.sh's oracle-capture step (known to reach
# the settled picker reliably): SUNBRIGHT_HEADLESS=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15
# SUNBRIGHT_SCENARIO=0 + probe, 2x Start via /pad to leave PRESS-START, then settle. Adds
# SUNBRIGHT_DUMP=1 so Dolphin's FrameDumper writes PNGs to its dump dir (<home>/.local/share/dolphin-emu/Dump/Frames)
# even though nothing is presented to a window (headless still renders real Vulkan frames).
#
# Usage: tools/render/title_oracle_headless.sh [settle_s=55]
# Output: scratch/oracle/title_gx_oracle_headless.png
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
source .env 2>/dev/null || { echo "no .env (need SUNBRIGHT_ROM)"; exit 1; }

SETTLE="${1:-55}"
DUMP="$HOME/.local/share/dolphin-emu/Dump/Frames"
OUT="scratch/oracle/title_gx_oracle_headless.png"
mkdir -p scratch/oracle
rm -f "$DUMP"/*.png 2>/dev/null

pkill -9 -x sunbright 2>/dev/null; sleep 1

echo "[oracle-headless] launching build/sunbright HEADLESS -> stage 15 (probe, dump)..."
timeout -s KILL 240 setarch -R env \
  SUNBRIGHT_ROM="$SUNBRIGHT_ROM" SUNBRIGHT_HEADLESS=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 \
  SUNBRIGHT_SCENARIO=0 SUNBRIGHT_NGX_PRESENT=0 SUNBRIGHT_PROBE=1 SUNBRIGHT_TURBO=1 \
  SUNBRIGHT_DUMP=1 build/sunbright > scratch/oracle/title_oracle_headless.log 2>&1 &
OPID=$!

echo "[oracle-headless] waiting for probe..."
for i in $(seq 1 40); do
  sleep 2
  curl -s --max-time 2 http://127.0.0.1:17654/metrics >/dev/null 2>&1 && break
done

echo "[oracle-headless] pressing START x2 to leave PRESS-START..."
curl -s --max-time 5 "http://127.0.0.1:17654/pad?do=start&ms=250" >/dev/null; sleep 2
curl -s --max-time 5 "http://127.0.0.1:17654/pad?do=start&ms=250" >/dev/null

echo "[oracle-headless] letting the picker settle (${SETTLE}s)..."
sleep "$SETTLE"

# Grab the newest fully-written frame (skip the last few — may be mid-write).
python3 - "$DUMP" "$OUT" <<'PY'
import sys, glob, os
from PIL import Image
d, out = sys.argv[1], sys.argv[2]
fs = sorted(glob.glob(os.path.join(d, 'framedump_*.png')),
            key=lambda f: int(f.split('_')[-1].split('.')[0]))
for f in reversed(fs[:-3]):
    try:
        im = Image.open(f); im.load(); im.convert('RGB').save(out)
        print('[oracle-headless] saved', out, 'from', os.path.basename(f), im.size)
        break
    except Exception:
        pass
else:
    print('[oracle-headless] FAILED — no complete frame found'); sys.exit(1)
PY

pkill -9 -x sunbright 2>/dev/null
echo "[oracle-headless] done."

#!/usr/bin/env bash
# plaza_sbs.sh — side-by-side PNG of Delfino Plaza (SB_STAGE=1 SB_SCENARIO=0), REAL GAMEPLAY.
#
# Oracle side: build/sunbright + /loadstate on scratch/freeroam_plaza.sav (a Jun-19 save that
# lands past the arrival cutscene into free-roam gameplay). The `SUNBRIGHT_FASTBOOT=1`
# 30s-settle capture used before landed mid-shine-delivery-cutscene (letterboxed 401×216
# window, mean 2.7/3.0/42.3) which was NOT plaza gameplay — comparing that to native
# gameplay produced a fake fidelity Δ.
#
# Native side: build-native/sms-boot + SB_STAGE=1 SB_SCENARIO=0 fastboot. Native's own fastboot
# (reference/sms/src/System/Application.cpp:572) skips the intro cutscene flags and jumps
# straight to APP_STATE_GAMEPLAY on frame 0 — no savestate mechanism needed here (and none
# exists yet for sms-boot). SB_FRAME_DUMP_START=3000 gives it wall time to reach the settled
# gameplay render (frames >=3000 are byte-identical, so any specific start ≥3000 works).
#
# Both sides land in real Plaza gameplay. Δ is now meaningful fidelity.
#
# Usage: tools/render/plaza_sbs.sh [settle_secs=30]
# Output: scratch/screenshots/sbs_plaza.png
#         scratch/screenshots/plaza_delta.txt (title_overbright.py Δ report)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
source .env 2>/dev/null || { echo "no .env (need SUNBRIGHT_ROM)"; exit 1; }

SETTLE="${1:-30}"
SAVE="${PLAZA_SAVE:-$HERE/scratch/freeroam_plaza.sav}"
DUMP_ORACLE="$HOME/.local/share/dolphin-emu/Dump/Frames"
DUMP_NATIVE="$HERE/scratch/frames"
ORACLE_PNG="$HERE/scratch/oracle/plaza_gameplay_oracle.png"
OUT="$HERE/scratch/screenshots/sbs_plaza.png"
DELTA_OUT="$HERE/scratch/screenshots/plaza_delta.txt"
mkdir -p "$HERE/scratch/screenshots" "$HERE/scratch/passes" "$DUMP_NATIVE" "$HERE/scratch/oracle"

command -v magick >/dev/null || { echo "magick (ImageMagick) required"; exit 1; }
[ -s "$SAVE" ] || { echo "[plaza-sbs] save missing: $SAVE (PLAZA_SAVE=<path> to override)"; exit 1; }

pkill -9 -x sunbright 2>/dev/null
pkill -9 -x sms-boot  2>/dev/null
sleep 1
rm -f "$DUMP_NATIVE"/boot_*.ppm 2>/dev/null

# --- Oracle: fastboot to a running core, then /loadstate the plaza save. ------------------
rm -f "$DUMP_ORACLE"/framedump_*.png 2>/dev/null
echo "[plaza-sbs] launching oracle (build/sunbright, fastboot + /loadstate $SAVE)..."
SUNBRIGHT_HEADLESS=1 SUNBRIGHT_PROBE=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_BACKEND=Vulkan \
  SUNBRIGHT_TURBO=1 SUNBRIGHT_DUMP=1 SUNBRIGHT_NGX_PRESENT=0 \
  build/sunbright "$SUNBRIGHT_ROM" > "$HERE/scratch/passes/sbs_plaza_oracle.log" 2>&1 &
OPID=$!
UP=0
for i in $(seq 1 45); do
  sleep 2
  e=$(curl -s -m2 "http://127.0.0.1:17654/metrics" 2>/dev/null | grep -oE '"emu_secs": [0-9.]+' | grep -oE '[0-9.]+')
  if [ -n "$e" ] && awk "BEGIN{exit !($e>=8)}"; then UP=1; break; fi
done
if [ "$UP" != 1 ]; then
  echo "[plaza-sbs] FAIL: oracle never came up (check sbs_plaza_oracle.log)"
  pkill -9 -x sunbright; exit 2
fi
echo "[plaza-sbs] oracle up, loading save"
curl -s -m20 "http://127.0.0.1:17654/loadstate?f=$SAVE" > /dev/null
sleep 6  # let the loaded scene render + animations settle to a steady frame
pkill -9 -x sunbright 2>/dev/null
wait $OPID 2>/dev/null
# 3rd-newest dodges the SIGKILL-truncated tail (same discipline as oracle_png_cache.sh).
mapfile -t OP < <(ls -t "$DUMP_ORACLE"/framedump_*.png 2>/dev/null)
if [ "${#OP[@]}" -lt 3 ]; then
  echo "[plaza-sbs] FAIL: oracle produced <3 frames"; exit 2
fi
cp "${OP[2]}" "$ORACLE_PNG"
echo "[plaza-sbs] oracle: $ORACLE_PNG  (from ${OP[2]})"

# --- Native: SB_STAGE=1 fastboot, dump from frame 3000 (settled). -------------------------
echo "[plaza-sbs] launching native (build-native/sms-boot, SB_STAGE=1)..."
timeout -s KILL "$((SETTLE + 5))" setarch -R env \
  SUNBRIGHT_DISC="$HERE/scratch/disc/sms.iso" SB_THP_FAST=1 SB_TURBO=1 \
  SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=1 SB_SCENARIO=0 SB_OWN_GXLIST=1 \
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=3000 SB_FRAME_DUMP_MAX=8 \
  SB_WATCHDOG_SECS=0 \
  ./build-native/sms-boot > scratch/passes/sbs_plaza_native.log 2>&1 &
NPID=$!
sleep "$SETTLE"
pkill -9 -x sms-boot 2>/dev/null
wait $NPID 2>/dev/null

mapfile -t NP < <(ls -t "$DUMP_NATIVE"/boot_*.ppm 2>/dev/null)
NATIVE_PPM="${NP[0]:-}"
if [ -z "$NATIVE_PPM" ]; then
  echo "[plaza-sbs] FAIL: native produced no frames — check scratch/passes/sbs_plaza_native.log"
  magick "$ORACLE_PNG" -resize 640x480 -bordercolor red -border 4 "$OUT"
  exit 3
fi
echo "[plaza-sbs] native: $NATIVE_PPM"

# --- Δ report + composite -----------------------------------------------------------------
echo "[plaza-sbs] Δ report -> $DELTA_OUT"
python3 tools/render/title_overbright.py "$NATIVE_PPM" "$ORACLE_PNG" > "$DELTA_OUT"
cat "$DELTA_OUT"

TMP=$(mktemp -d)
magick "$ORACLE_PNG" -resize 640x480 "$TMP/o.png"
magick "$NATIVE_PPM" -resize 640x480 "$TMP/n.png"
magick "$TMP/o.png" "$TMP/n.png" +append \
  -background black -splice 0x28 -pointsize 22 -fill white -gravity North \
  -annotate +0+2 'oracle plaza gameplay (savestate)                     native SB_STAGE=1 (fastboot)' \
  "$OUT"
rm -rf "$TMP"
echo "[plaza-sbs] wrote: $OUT"

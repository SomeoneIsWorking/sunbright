#!/usr/bin/env bash
# plaza_sbs.sh — side-by-side PNG of Delfino Plaza (SB_STAGE=1 SB_SCENARIO=0) from both engines.
# Sibling of title_sbs.sh; models the same 3-part pipeline: oracle-PNG cache, native run, magick
# +append with a header. Plaza-specific:
#   - No pad script needed: fastboot lands directly in APP_STATE_GAMEPLAY. Both engines just
#     settle in-scene for SETTLE seconds and we take the newest frame.
#   - Native must NOT use SB_FRAME_DUMP_START=0 — the first-frame Vulkan pipeline compile blocks
#     the game thread inside VIWaitForRetrace's cooperative-scheduler drain BEFORE
#     TMarDirector::setupThreadFunc is spawned, causing an indefinite deadlock (see
#     debug_journal/2026-07-03_plaza_stage1_first_frame.md). Late start (>=1000) is safe: by
#     then setupThreadFunc has finished, the scene is populated (280k+ verts on Plaza), and
#     present_hook can dump normally.
#   - The scheduler drain-vs-spawn ordering bug is a scoped follow-up; the late-start recipe is
#     the immediate unblock.
#
# Usage: tools/render/plaza_sbs.sh [settle_secs=30]
# Output: scratch/screenshots/sbs_plaza.png
#         + plaza_delta.txt (title_overbright.py Δ report, Plaza-agnostic — pixel-diffs 2 PPMs)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
source .env 2>/dev/null || { echo "no .env (need SUNBRIGHT_ROM)"; exit 1; }

SETTLE="${1:-30}"
DUMP_ORACLE="$HOME/.local/share/dolphin-emu/Dump/Frames"
DUMP_NATIVE="$HERE/scratch/frames"
OUT="$HERE/scratch/screenshots/sbs_plaza.png"
DELTA_OUT="$HERE/scratch/screenshots/plaza_delta.txt"
mkdir -p "$HERE/scratch/screenshots" "$HERE/scratch/passes" "$DUMP_NATIVE" "$HERE/scratch/oracle"

command -v magick >/dev/null || { echo "magick (ImageMagick) required"; exit 1; }

pkill -9 -x sunbright 2>/dev/null
pkill -9 -x sms-boot  2>/dev/null
sleep 1
rm -f "$DUMP_NATIVE"/boot_*.ppm 2>/dev/null

# Oracle: cache under scratch/oracle/plaza_gx_oracle.png. Same cache-key discipline as title
# (oracle_png_cache.sh includes STAGE/SCENARIO in the stamp).
ORACLE_PNG="$(bash tools/render/oracle_png_cache.sh scratch/oracle/plaza_gx_oracle.png 1 0 "$SETTLE" 2>&1 | tee /dev/stderr | tail -1)"
if [ ! -f "$ORACLE_PNG" ]; then
    echo "[plaza-sbs] FAIL: oracle PNG cache failed"; exit 2
fi
echo "[plaza-sbs] oracle: $ORACLE_PNG"

echo "[plaza-sbs] launching native (build-native/sms-boot, SB_STAGE=1 SB_OWN_GXLIST=1, dump from frame 1000)..."
# SB_FRAME_DUMP_MAX=4 gives us a few frames so we can pick the newest (last is most-settled).
# SB_FRAME_DUMP_START=3000 lands AFTER the Delfino approach cutscene (Mario riding the shine
# into the plaza; frame 1000 is still mid-flight = tiny content region, mean=1.2). By 3000
# gameplay has started (imm_batches 11→60, textured 4→52, real Plaza content — mean 12.5,
# 16535 unique colors).
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

# Native: NEWEST boot_XXXX.ppm (highest frame index = most-settled).
mapfile -t NP < <(ls -t "$DUMP_NATIVE"/boot_*.ppm 2>/dev/null)
NATIVE_PPM="${NP[0]:-}"

if [ -z "$NATIVE_PPM" ]; then
  echo "[plaza-sbs] FAIL: native produced no frames — check scratch/passes/sbs_plaza_native.log"
  magick "$ORACLE_PNG" -resize 640x480 -bordercolor red -border 4 "$OUT"
  echo "[plaza-sbs] wrote oracle-only image: $OUT"
  exit 3
fi
echo "[plaza-sbs] native: $NATIVE_PPM"

# Δ report (plaza-agnostic — title_overbright.py just pixel-diffs 2 images).
echo "[plaza-sbs] Δ report -> $DELTA_OUT"
python3 tools/render/title_overbright.py "$NATIVE_PPM" "$ORACLE_PNG" > "$DELTA_OUT"
cat "$DELTA_OUT"

# SBS composite.
TMP=$(mktemp -d)
magick "$ORACLE_PNG" -resize 640x480 "$TMP/o.png"
magick "$NATIVE_PPM" -resize 640x480 "$TMP/n.png"
magick "$TMP/o.png" "$TMP/n.png" +append \
  -background black -splice 0x28 -pointsize 22 -fill white -gravity North \
  -annotate +0+2 'oracle (Dolphin GX)                                                     native (sms-boot / nvk)' \
  "$OUT"
rm -rf "$TMP"
echo "[plaza-sbs] wrote: $OUT"

#!/usr/bin/env bash
# fileselect_sbs.sh — side-by-side PNG of the file-select frame from both engines.
#
# Left  = oracle  (build/sunbright,  Dolphin GX headless, SUNBRIGHT_STAGE=15 fastboot).
# Right = native  (build-native/sms-boot, SB_STAGE=15 fastboot).
# Both run HEADLESS with widescreen OFF so the projection matches (per session16
# fingerprint discipline — SUNBRIGHT_WIDESCREEN=0 in oracle disables the ov_gx_projection
# horizontal squeeze if it becomes purejit-safe later; native has no widescreen path).
#
# Reliability caveat: sms-boot has a non-deterministic ~settle-frame SIGSEGV (2-frame
# backtrace ending in libc signal delivery — stack corruption / no-FP callback fault).
# Sometimes it dumps a handful of frames before dying, sometimes it dies pre-settle. The
# script waits SETTLE seconds; if native produced ANY valid frame, we use it. If not, we
# fall back to whatever most-recent frame it did produce, and report the crash frame count.
# Oracle is reliable — Dolphin's FrameDumper dumps every presented frame.
#
# Usage: tools/render/fileselect_sbs.sh [settle_secs=40]
# Output: scratch/screenshots/sbs_fileselect.png
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
source .env 2>/dev/null || { echo "no .env (need SUNBRIGHT_ROM)"; exit 1; }

SETTLE="${1:-40}"
DUMP_ORACLE="$HOME/.local/share/dolphin-emu/Dump/Frames"
DUMP_NATIVE="$HERE/scratch/frames"
OUT="$HERE/scratch/screenshots/sbs_fileselect.png"
mkdir -p "$HERE/scratch/screenshots" "$HERE/scratch/passes" "$DUMP_NATIVE"

command -v magick >/dev/null || { echo "magick (ImageMagick) required"; exit 1; }

pkill -9 -x sunbright 2>/dev/null
pkill -9 -x sms-boot  2>/dev/null
sleep 1
rm -f "$DUMP_ORACLE"/framedump_*.png "$DUMP_NATIVE"/boot_*.ppm 2>/dev/null

echo "[sbs] launching oracle (build/sunbright, STAGE=15, widescreen off, DUMP=1)..."
timeout -s KILL "$((SETTLE + 5))" setarch -R env \
  SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_BACKEND=Vulkan \
  SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_SCENARIO=0 \
  SUNBRIGHT_WIDESCREEN=0 SUNBRIGHT_DUMP=1 \
  ./build/sunbright "$SUNBRIGHT_ROM" > scratch/passes/sbs_oracle.log 2>&1 &
OPID=$!

echo "[sbs] launching native (build-native/sms-boot, SB_FILESELECT=1 SB_STAGE=15 SB_OWN_GXLIST=1)..."
# SB_FILESELECT=1 routes native's fastboot through APP_STATE_TITLE → TSelectDir (the actual
# file-select), NOT APP_STATE_GAMEPLAY (SB_STAGE=15 alone lands somewhere ELSE — session 16
# SBS reveal). SB_OWN_GXLIST=1 uses the REAL master GX perform-list (the geometry-complete
# path — session 15 memory [[fileselect-geometry-gap-is-ownlist]]) so the scene matches
# what value-oracle measures. Same recipe as tools/render/fileselect_value_oracle.sh.
timeout -s KILL "$((SETTLE + 5))" setarch -R env \
  SUNBRIGHT_DISC="$HERE/scratch/disc/sms.iso" SB_THP_FAST=1 SB_TURBO=1 \
  SB_HOST_ALLOC_CAP_MB=3072 SB_FILESELECT=1 SB_STAGE=15 SB_SCENARIO=0 SB_OWN_GXLIST=1 \
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=250 SB_FRAME_DUMP_MAX=4 SB_WATCHDOG_SECS=0 \
  ./build-native/sms-boot > scratch/passes/sbs_native.log 2>&1 &
NPID=$!

sleep "$SETTLE"
pkill -9 -x sunbright 2>/dev/null
pkill -9 -x sms-boot  2>/dev/null
wait $OPID 2>/dev/null
wait $NPID 2>/dev/null

# Oracle: pick the SECOND-newest PNG (newest may be truncated by SIGKILL mid-write).
mapfile -t OP < <(ls -t "$DUMP_ORACLE"/framedump_*.png 2>/dev/null)
ORACLE_PNG=""
if   [ "${#OP[@]}" -ge 2 ]; then ORACLE_PNG="${OP[1]}"
elif [ "${#OP[@]}" -ge 1 ]; then ORACLE_PNG="${OP[0]}"; fi

# Native: LAST valid boot_XXXX.ppm (all should be complete — sms-boot writes atomically per frame).
mapfile -t NP < <(ls -t "$DUMP_NATIVE"/boot_*.ppm 2>/dev/null)
NATIVE_PPM="${NP[0]:-}"

if [ -z "$ORACLE_PNG" ]; then
  echo "[sbs] FAIL: oracle produced no frames — check scratch/passes/sbs_oracle.log"
  exit 2
fi
if [ -z "$NATIVE_PPM" ]; then
  echo "[sbs] FAIL: native produced no frames (crashed before frame $SB_FRAME_DUMP_START?)"
  echo "        check scratch/passes/sbs_native.log (SIGSEGV expected; see memory session16 followup)"
  # Emit oracle-only so the SBS still has something to show.
  magick "$ORACLE_PNG" -resize 640x480 -bordercolor red -border 4 "$OUT"
  echo "[sbs] wrote oracle-only image: $OUT"
  exit 3
fi

echo "[sbs] oracle: $ORACLE_PNG"
echo "[sbs] native: $NATIVE_PPM"
TMP=$(mktemp -d)
magick "$ORACLE_PNG" -resize 640x480 "$TMP/o.png"
magick "$NATIVE_PPM" -resize 640x480 "$TMP/n.png"
magick "$TMP/o.png" "$TMP/n.png" +append \
  -background black -splice 0x28 -pointsize 22 -fill white -gravity North \
  -annotate +0+2 'oracle (Dolphin GX)                                                     native (sms-boot / nvk)' \
  "$OUT"
rm -rf "$TMP"
echo "[sbs] wrote: $OUT"

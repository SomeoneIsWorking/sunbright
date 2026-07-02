#!/usr/bin/env bash
# title_sbs.sh — side-by-side PNG of the retail title screen (PUSH START / save-file
# picker) from both engines. Taxonomy note (memory [[title-vs-scenario-select-taxonomy]]):
# stage-15 gameplay = TITLE SCREEN; TSelectDir/APP_STATE_TITLE = SCENARIO SELECT (unrelated).
#
# Left  = oracle  (build/sunbright,  Dolphin GX headless, SUNBRIGHT_STAGE=15 fastboot).
# Right = native  (build-native/sms-boot, SB_STAGE=15 fastboot).
# Both land at APP_STATE_GAMEPLAY stage 15 — STATE-MATCH under the parity_sweep gate.
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
# Usage: tools/render/title_sbs.sh [settle_secs=40]
# Output: scratch/screenshots/sbs_title.png
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
source .env 2>/dev/null || { echo "no .env (need SUNBRIGHT_ROM)"; exit 1; }

SETTLE="${1:-40}"
DUMP_ORACLE="$HOME/.local/share/dolphin-emu/Dump/Frames"
DUMP_NATIVE="$HERE/scratch/frames"
OUT="$HERE/scratch/screenshots/sbs_title.png"
mkdir -p "$HERE/scratch/screenshots" "$HERE/scratch/passes" "$DUMP_NATIVE"

command -v magick >/dev/null || { echo "magick (ImageMagick) required"; exit 1; }

pkill -9 -x sunbright 2>/dev/null
pkill -9 -x sms-boot  2>/dev/null
sleep 1
rm -f "$DUMP_NATIVE"/boot_*.ppm 2>/dev/null

# Oracle: reuse cached PNG unless the build/capture-code/fastboot mtimes changed.
# Caches under scratch/oracle/title_gx_oracle.png (see oracle_png_cache.sh). This
# skips ~55s of oracle capture on cache HIT — significant when iterating native.
ORACLE_PNG_CACHED="$(bash tools/render/oracle_png_cache.sh 2>&1 | tee /dev/stderr | tail -1)"
if [ ! -f "$ORACLE_PNG_CACHED" ]; then
    echo "[sbs] FAIL: oracle PNG cache failed"; exit 2
fi

echo "[sbs] using cached oracle: $ORACLE_PNG_CACHED"

echo "[sbs] launching native (build-native/sms-boot, SB_STAGE=15 SB_OWN_GXLIST=1, VI-timed +Start x2 via SB_PAD_SCRIPT)..."
# Native's TCardLoad-frame counting differs from oracle's VI counting — native's own
# per-perform trace confirms press 1 at frame 86 (state 9→3), press 2 at frame 586
# (state 3→8), landed at state 0 by frame 746 (verified 2026-07-02 with SB_SEL_DBG=1).
# Native VI counting is closer to TCardLoad's; use frame 250 / 500 (same interval).
# SB_SEL_DUMP_SETTLED=8 gates dump on mState==0 && unk10==2 && camera_view_settled().
timeout -s KILL "$((SETTLE + 5))" setarch -R env \
  SUNBRIGHT_DISC="$HERE/scratch/disc/sms.iso" SB_THP_FAST=1 SB_TURBO=1 \
  SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_OWN_GXLIST=1 \
  SB_PAD_SCRIPT="250:START 282:- 500:START 532:-" \
  SB_SEL_DUMP_SETTLED=8 SB_WATCHDOG_SECS=0 \
  ./build-native/sms-boot > scratch/passes/sbs_native.log 2>&1 &
NPID=$!

sleep "$SETTLE"
pkill -9 -x sms-boot  2>/dev/null
wait $NPID 2>/dev/null

ORACLE_PNG="$ORACLE_PNG_CACHED"

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

#!/usr/bin/env bash
# boot_vs_vanilla.sh — render-divergence ORACLE: sms-boot (PC-native engine) vs VANILLA Dolphin-GX.
#
# The user directive (2026-06-23): "get both games into the same game state then compare render
# layers." Both builds run the SAME engine from the SAME deterministic fastboot (File 1 → Delfino
# Plaza), so they reach the same FIRST-PLAZA-FRAME — sms-boot just renders a fraction of it. We
# anchor both on the scene-load event, capture each, normalize to a common size, and report the
# per-region pixel divergence (ab_diff.py: overall + 4x4 grid + heatmap) so we can see WHICH screen
# region / render layer diverges (e.g. "the building band is black in sms-boot").
#
# The vanilla side renders the REAL guest GX (NGX off) at 4:3 with NO widescreen / NO gecko codes =
# a pure-vanilla reference (verified: GMSE01.ini has no [Gecko_Enabled]; GFX_WIDESCREEN_HACK=false).
#
# ⚠ SYNC CAVEAT (known limitation): the two engines are NOT yet frame-synced. Vanilla plays the
# Entrance.thp movie + opening cutscene (animated), while sms-boot freezes at its scene-dump frame.
# So the diff conflates the RENDERING gap with a SCENE-MOMENT gap — the number is indicative, not
# exact. A clean oracle needs both at the IDENTICAL frame (capture vanilla's FIRST plaza render, or
# drive both to a deterministic settle). Until then, read the region grid for GROSS divergence
# (which layer/region is wholly missing), not small deltas.
#
# Usage:  tools/render/boot_vs_vanilla.sh [vanilla_settle_secs]
#   env:  SUNBRIGHT_DISC (default scratch/disc/sms.iso), SB_BUILD (default build-native),
#         SB_REUSE_VANILLA=1 / SB_REUSE_BOOT=1 (skip a capture, reuse scratch/oracle/*.{png,ppm})
set -u
HERE="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$HERE"
SETTLE="${1:-6}"
ISO="${SUNBRIGHT_DISC:-scratch/disc/sms.iso}"
SB_BUILD="${SB_BUILD:-build-native}"
OUT="$HERE/scratch/oracle"; mkdir -p "$OUT"
DUMP="$HOME/.local/share/dolphin-emu/Dump/Frames"
log(){ printf '\033[36m[oracle]\033[0m %s\n' "$*"; }

# ── 1. VANILLA Dolphin-GX (NGX off, 4:3) — capture a settled plaza frame ──────────────────────────
# Timing: SUNBRIGHT_FASTBOOT still plays the Entrance.thp movie + opening, so the plaza render
# doesn't appear until ~165s. We wait until dolpic*.szs loads (the plaza scene file) AND a healthy
# frame count has accumulated, then add SETTLE, then grab a NON-latest frame (the newest may be
# mid-write → truncated PNG). Do NOT match "Delfino Plaza" (that's the early fastboot log line).
if [ "${SB_REUSE_VANILLA:-0}" = 1 ] && [ -s "$OUT/vanilla.png" ]; then
  log "reusing existing vanilla.png"
else
  log "vanilla Dolphin-GX: fastboot → plaza (movie+settle, ~${SETTLE}s after scene load)…"
  pkill -9 -x sunbright 2>/dev/null; sleep 1
  rm -f "$DUMP"/framedump_*.png 2>/dev/null; mkdir -p "$DUMP"
  SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_NGX_PRESENT=0 SUNBRIGHT_WIDESCREEN=0 SUNBRIGHT_DUMP=1 \
    timeout 320 ./run.sh "$ISO" > "$OUT/vanilla.log" 2>&1 &
  for i in $(seq 1 60); do
    if grep -qaE "dolpic[0-9]+\.szs" "$OUT/vanilla.log" 2>/dev/null; then
      n=$(ls "$DUMP"/framedump_*.png 2>/dev/null | wc -l); [ "$n" -gt 300 ] && break
    fi
    pgrep -x sunbright >/dev/null || { log "vanilla process gone (check vanilla.log)"; break; }
    sleep 5
  done
  sleep "$SETTLE"
  # grab the 10th-newest (safely fully written), validate it's a real PNG
  van="$(ls -t "$DUMP"/framedump_*.png 2>/dev/null | sed -n '10p')"
  if [ -n "$van" ] && magick identify "$van" >/dev/null 2>&1; then
    cp "$van" "$OUT/vanilla.png"; log "vanilla frame: $(basename "$van")"
  else
    log "NO valid vanilla frame!"
  fi
  pkill -9 -x sunbright 2>/dev/null
fi

# ── 2. sms-boot (native engine) — its first scene frame ──────────────────────────────────────────
if [ "${SB_REUSE_BOOT:-0}" = 1 ] && [ -s "$OUT/boot.ppm" ]; then
  log "reusing existing boot.ppm"
else
  log "sms-boot: fastboot → scene dump…"
  pkill -9 -x sms-boot 2>/dev/null; sleep 1
  rm -f scratch/frames/boot_*.ppm
  timeout 450 setarch -R env SUNBRIGHT_DISC="$ISO" SB_THP_FAST=1 SB_WATCHDOG_SECS=0 \
    SB_FRAME_DUMP=1 SB_FRAME_DUMP_ON_SCENE=1 SB_FRAME_DUMP_MAX=1 SB_HOST_ALLOC_CAP_MB=3072 \
    "./$SB_BUILD/sms-boot" > "$OUT/boot.log" 2>&1 &
  for i in $(seq 1 60); do
    ls scratch/frames/boot_*.ppm >/dev/null 2>&1 && grep -qa "present.*frame" "$OUT/boot.log" && break
    pgrep -x sms-boot >/dev/null || { log "sms-boot EXITED early"; break; }
    sleep 8
  done
  boot="$(ls -t scratch/frames/boot_*.ppm 2>/dev/null | head -1)"
  [ -n "$boot" ] && cp "$boot" "$OUT/boot.ppm" && log "sms-boot frame: $(basename "$boot")" || log "NO sms-boot frame!"
  pkill -9 -x sms-boot 2>/dev/null
fi

# ── 3. normalize to a common size + per-region divergence ────────────────────────────────────────
[ -f "$OUT/vanilla.png" ] && [ -f "$OUT/boot.ppm" ] || { log "missing a capture — aborting diff"; exit 2; }
magick "$OUT/vanilla.png" -resize 640x480\! "$OUT/vanilla640.ppm"
magick "$OUT/boot.ppm"    -resize 640x480\! "$OUT/boot640.ppm"
magick "$OUT/vanilla640.ppm" "$OUT/boot640.ppm" +append "$OUT/side_by_side.png"
log "side-by-side: $OUT/side_by_side.png   (left=VANILLA, right=sms-boot)"
log "per-region divergence (which layer/region differs):"
python3 tools/render/ab_diff.py "$OUT/vanilla640.ppm" "$OUT/boot640.ppm" --heat "$OUT/heat.ppm"

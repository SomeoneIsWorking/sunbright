#!/usr/bin/env bash
# fs_oracle — phase-robust FILE-SELECT oracle: ngx (NGX_PRESENT=1) vs the Dolphin-GX
# baseline (NGX_PRESENT=0), TIME-AVERAGED to defeat animation-phase confounds, then
# per-region diff + heatmap. This is the file-select counterpart of ab_oracle.sh
# (which is gameplay/save-state only).
#
# WHY time-average: file-select has scrolling clouds, animated water, and a running
# Mario. A single-frame ngx-vs-GX diff is dominated by where those happened to be, not
# by a real renderer gap — the documented file-select "wash" was largely this artifact
# (memory fileselect-cloud-wash-drift-artifact). Averaging N frames over a few seconds
# converges each side to its time-mean; periodic animation cancels, systematic material/
# shading/blend differences survive. The diff number is then trustworthy.
#
# Both sides are held at file-select deterministically: AUTOSTART drives title->file-select,
# then /pad?do=autostop stops the A-press so it never loads a save (stays on the menu).
#
# Usage: tools/render/fs_oracle.sh [n_frames=12] [interval_s=0.4] [hold_s=24]
# Output: scratch/screenshots/fso_{gx,ngx}_avg.ppm + .png, fso_heat.ppm, per-region delta.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
[ -f .env ] && { set -a; . ./.env; set +a; }
ROM="${SUNBRIGHT_ROM:-$HERE/rom.rvz}"
# Prefer the documented current build (build-freshtest), else SUNBRIGHT_BIN, else newest.
# Pinning matters: after a fix you rebuild ONE dir — the tool must run THAT binary, not a
# stale build/ that merely has a newer mtime (memory: stale-build trap).
BIN="${SUNBRIGHT_BIN:-}"
[ -z "$BIN" ] && [ -x build-freshtest/sunbright ] && BIN=build-freshtest/sunbright
[ -z "$BIN" ] && BIN="$(ls -t build*/sunbright 2>/dev/null | head -1)"
[ -x "$BIN" ] || { echo "no sunbright binary"; exit 1; }
[ -f "$ROM" ] || { echo "no ROM: $ROM"; exit 1; }
PORT=17654; SHOTS="$HERE/scratch/screenshots"; mkdir -p "$SHOTS" "$HERE/scratch/logs"

N=${1:-12}; IV=${2:-0.4}; HOLD=${3:-24}
echo "fs_oracle: bin=$BIN  N=$N frames  interval=${IV}s  hold=${HOLD}s"

capture_side() {  # $1=label(gx|ngx)  $2=present(0|1)  $3=ext(gx|ngx)
  local label=$1 present=$2 ext=$3
  pkill -9 -x sunbright 2>/dev/null; sleep 1
  rm -f "$SHOTS"/fso_${label}_*.png
  local env="SUNBRIGHT_HEADLESS=1 SUNBRIGHT_AUTOSTART=1 SUNBRIGHT_PROBE=1"
  [ "$present" = 1 ] && env="$env SUNBRIGHT_NGX_PRESENT=1" || env="$env SUNBRIGHT_BACKEND=OGL"
  env $env "$BIN" "$ROM" > "$HERE/scratch/logs/fso_${label}.log" 2>&1 &
  # hold at file-select: wait past the title/THP skip, before the 28s A-press, then autostop
  sleep "$HOLD"
  curl -s --max-time 5 "http://127.0.0.1:$PORT/pad?do=autostop" >/dev/null 2>&1
  echo "  [$label] autostop fired"
  sleep 2
  local i
  for i in $(seq 1 "$N"); do
    curl -s --max-time 8 "http://127.0.0.1:$PORT/abshot?name=fso_${label}_$i" >/dev/null 2>&1
    sleep "$IV"
  done
  local got; got=$(ls "$SHOTS"/fso_${label}_*.${ext}.png 2>/dev/null | wc -l)
  echo "  [$label] captured $got frames (.${ext}.png)"
  pkill -9 -x sunbright 2>/dev/null; sleep 1
}

capture_side gx  0 gx
capture_side ngx 1 ngx

python3 "$HERE/tools/render/img_avg.py" "$SHOTS/fso_gx_avg.ppm"  "$SHOTS"/fso_gx_*.gx.png   || exit $?
python3 "$HERE/tools/render/img_avg.py" "$SHOTS/fso_ngx_avg.ppm" "$SHOTS"/fso_ngx_*.ngx.png || exit $?
magick "$SHOTS/fso_gx_avg.ppm"  "$SHOTS/fso_gx_avg.png"  2>/dev/null
magick "$SHOTS/fso_ngx_avg.ppm" "$SHOTS/fso_ngx_avg.png" 2>/dev/null
echo "=== time-averaged file-select A/B (ngx vs GX) ==="
python3 "$HERE/tools/render/ab_diff.py" "$SHOTS/fso_gx_avg.ppm" "$SHOTS/fso_ngx_avg.ppm" --heat "$SHOTS/fso_heat.ppm"
magick "$SHOTS/fso_heat.ppm" "$SHOTS/fso_heat.png" 2>/dev/null
echo "-> $SHOTS/fso_{gx,ngx}_avg.png  fso_heat.png"

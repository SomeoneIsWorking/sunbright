#!/usr/bin/env bash
# oracle_ab.sh — frame-EXACT Dolphin-GX-oracle vs ngx A/B for the no-recomp engine.
#
# The zero-drift same-present /abshot2 oracle is dead under no-recomp (ngx owns the frame, so Dolphin
# GX is black in the ngx run). This restores a reliable oracle by running TWO deterministic processes
# of OUR binary and syncing them on EMULATED time (not wall time):
#   - oracle: SUNBRIGHT_NGX_PRESENT=0  -> pure Dolphin GX renders the real frame (the sanctioned
#     DISABLE_RECOMP oracle; engine overrides are inert, so it's the unmodified Dolphin render path).
#   - ngx:    SUNBRIGHT_NGX_PRESENT=1  -> the native renderer.
# Fastboot is deterministic and idle Delfino has no input, so at the SAME emu_secs both processes are
# in the SAME game state => the two captures are frame-exact (the earlier wall-time sync drifted
# because the two modes run at different speeds). Each side captures via /abshot2 the moment the probe
# reports emu_secs >= T, then we diff with ab_diff.py (overall + 4x4 region grid + heatmap).
#
# Usage:  tools/render/oracle_ab.sh [EMU_SECONDS] [extra SUNBRIGHT_* env passed to BOTH]
#   tools/render/oracle_ab.sh 14
#   SUNBRIGHT_NGX_NOLIGHT=1 tools/render/oracle_ab.sh 14     # bisect: compare with lighting off
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
BIN="${SUNBRIGHT_BIN:-$HERE/build/sunbright}"
[ -x "$BIN" ] || BIN="$HERE/build-freshtest/sunbright"   # fallback to the bundled-config dev build
ROM="${SUNBRIGHT_ROM:-$HERE/rom.rvz}"
T="${1:-14}"            # target emulated-seconds for the capture (after fastboot reaches Delfino)
OUT="$HERE/scratch/screenshots"; mkdir -p "$OUT"
PORT=17654

# $1=mode label, $2=NGX_PRESENT value, $3=dest ppm
capture() {
  local label="$1" ngxp="$2" dest="$3"
  pkill -9 -x sunbright 2>/dev/null; sleep 2
  echo "[oracle_ab] $label: launching (NGX_PRESENT=$ngxp), waiting for emu_secs>=$T ..." >&2
  SUNBRIGHT_HEADLESS=1 SUNBRIGHT_PROBE=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_BACKEND=Vulkan \
    SUNBRIGHT_NGX_PRESENT="$ngxp" "$BIN" "$ROM" >"$HERE/scratch/logs/oracle_ab_$label.log" 2>&1 &
  local pid=$!
  local ok=0
  for i in $(seq 1 120); do
    sleep 2
    local e; e=$(curl -s -m3 "http://127.0.0.1:$PORT/metrics" 2>/dev/null | grep -o '"emu_secs": [0-9.]*' | grep -o '[0-9.]*')
    if [ -n "$e" ] && awk "BEGIN{exit !($e>=$T)}"; then ok=1; break; fi
  done
  if [ "$ok" != 1 ]; then echo "[oracle_ab] $label: never reached emu_secs $T" >&2; kill -9 $pid 2>/dev/null; return 1; fi
  # capture: /abshot2 writes ab2.gx.ppm (Dolphin XFB) + ab2.ngx.ppm (ngx). In GX-off it times out
  # waiting for the (empty) ngx side but still writes ab2.gx.ppm; ignore the timeout.
  curl -s -m12 "http://127.0.0.1:$PORT/abshot2" >/dev/null 2>&1
  sleep 1
  if [ "$ngxp" = 0 ]; then cp "$OUT/ab2.gx.ppm" "$dest"; else cp "$OUT/ab2.ngx.ppm" "$dest"; fi
  kill -9 $pid 2>/dev/null
  echo "[oracle_ab] $label: captured $dest at emu_secs~$T" >&2
}

capture oracle 0 "$OUT/ab_oracle.ppm" || exit 1
capture ngx     1 "$OUT/ab_ngx.ppm"    || exit 1
pkill -9 -x sunbright 2>/dev/null
echo "[oracle_ab] diff (oracle vs ngx, frame-exact @ emu_secs $T):" >&2
python3 "$HERE/tools/render/ab_diff.py" --heat "$OUT/ab_wash.ppm" "$OUT/ab_oracle.ppm" "$OUT/ab_ngx.ppm"

#!/usr/bin/env bash
# ab_oracle.sh — SAVE-STATE, frame-EXACT Dolphin-GX-oracle vs ngx A/B for the no-recomp engine.
#
# The zero-drift same-present /abshot2 oracle is dead under no-recomp (ngx owns the frame, so the
# Dolphin GX XFB is black in the ngx run). oracle_ab.sh restores an oracle by syncing two fastboot
# processes on EMULATED time, but that only ever reaches idle Delfino plaza. THIS tool loads a save
# STATE in two processes instead:
#   - oracle: SUNBRIGHT_NGX_PRESENT=0  -> pure Dolphin GX renders the real frame (sanctioned oracle).
#   - ngx:    SUNBRIGHT_NGX_PRESENT=1  -> the native renderer.
# State::LoadAs restores byte-identical guest RAM, so after the load both processes are in the EXACT
# same game state — the capture is frame-exact (no emu_secs drift), and it reaches ANY saved scene
# (sun occlusion / sphere sky / specific camera) that fastboot-plaza cannot. The load runs on the CPU
# thread via the /loadstate probe endpoint (loading from the SDL main thread deadlocks the governor).
#
# Usage:  tools/render/ab_oracle.sh <save.sav> [SETTLE_SECS] [extra SUNBRIGHT_* env to BOTH]
#   tools/render/ab_oracle.sh scratch/delfino.sav
#   tools/render/ab_oracle.sh scratch/delfino.sav 4
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
# Pick the binary: SUNBRIGHT_BIN wins; else the NEWEST of build*/sunbright (a stale build/ that
# lacks /loadstate would silently fail with "unknown path" — always run the freshest).
BIN="${SUNBRIGHT_BIN:-}"
if [ -z "$BIN" ]; then
  for c in "$HERE"/build-freshtest/sunbright "$HERE"/build/sunbright; do [ -x "$c" ] || continue; [ -z "$BIN" ] || [ "$c" -nt "$BIN" ] && BIN="$c"; done
fi
[ -x "$BIN" ] || { echo "ab_oracle: no sunbright binary found (build it first)" >&2; exit 1; }
echo "[ab_oracle] using binary: $BIN" >&2
ROM="${SUNBRIGHT_ROM:-$HERE/rom.rvz}"

SAVE="${1:?usage: ab_oracle.sh <save.sav> [settle_secs]}"
case "$SAVE" in /*) : ;; *) SAVE="$HERE/$SAVE" ;; esac
[ -s "$SAVE" ] || { echo "ab_oracle: save not found / empty: $SAVE" >&2; exit 1; }
SETTLE="${2:-4}"        # seconds to let the loaded scene render+settle before capture
OUT="$HERE/scratch/screenshots"; mkdir -p "$OUT" "$HERE/scratch/logs"
PORT=17654

# $1=mode label, $2=NGX_PRESENT value, $3=dest ppm
capture() {
  local label="$1" ngxp="$2" dest="$3"
  pkill -9 -x sunbright 2>/dev/null; sleep 2
  echo "[ab_oracle] $label: launching (NGX_PRESENT=$ngxp), fastboot to a running core ..." >&2
  # Fastboot just brings the core up to a deterministic running state quickly; /loadstate then
  # overwrites RAM with the requested save. (No SUNBRIGHT_STATE — that path is the broken one.)
  SUNBRIGHT_HEADLESS=1 SUNBRIGHT_PROBE=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_BACKEND=Vulkan \
    SUNBRIGHT_NGX_PRESENT="$ngxp" "$BIN" "$ROM" >"$HERE/scratch/logs/ab_oracle_$label.log" 2>&1 &
  local pid=$!
  # Wait for the core to be running (emu time advancing past fastboot reaching gameplay).
  local up=0
  for i in $(seq 1 90); do
    sleep 2
    local e; e=$(curl -s -m3 "http://127.0.0.1:$PORT/metrics" 2>/dev/null | grep -o '"emu_secs": [0-9.]*' | grep -o '[0-9.]*')
    if [ -n "$e" ] && awk "BEGIN{exit !($e>=6)}"; then up=1; break; fi
  done
  if [ "$up" != 1 ]; then echo "[ab_oracle] $label: core never came up" >&2; kill -9 $pid 2>/dev/null; return 1; fi
  echo "[ab_oracle] $label: core up, loading $SAVE ..." >&2
  curl -s -m20 "http://127.0.0.1:$PORT/loadstate?f=$SAVE" >&2
  sleep "$SETTLE"        # let the loaded scene render + animations settle to a steady frame
  # /abshot2 writes ab2.gx.ppm (Dolphin XFB) AND ab2.ngx.ppm (ngx). In GX-off mode it times out on
  # the (empty) ngx side but still writes ab2.gx.ppm; in ngx mode ab2.ngx.ppm is the live render.
  curl -s -m12 "http://127.0.0.1:$PORT/abshot2" >/dev/null 2>&1
  sleep 1
  if [ "$ngxp" = 0 ]; then cp "$OUT/ab2.gx.ppm" "$dest"; else cp "$OUT/ab2.ngx.ppm" "$dest"; fi
  kill -9 $pid 2>/dev/null
  echo "[ab_oracle] $label: captured $dest" >&2
}

capture oracle 0 "$OUT/abo_oracle.ppm" || exit 1
capture ngx     1 "$OUT/abo_ngx.ppm"    || exit 1
pkill -9 -x sunbright 2>/dev/null
echo "[ab_oracle] diff (oracle vs ngx, save=$(basename "$SAVE")):" >&2
python3 "$HERE/tools/render/ab_diff.py" --heat "$OUT/abo_wash.ppm" "$OUT/abo_oracle.ppm" "$OUT/abo_ngx.ppm"

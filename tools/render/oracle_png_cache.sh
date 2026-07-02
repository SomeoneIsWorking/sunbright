#!/usr/bin/env bash
# oracle_png_cache.sh — capture a Dolphin-GX PNG at the SETTLED post-Start title
# (file-block sub-screen — the state where the reflective sea is visible) and
# cache it. Sibling of oracle_cache.sh (which caches the parity JSONL).
#
# Cache key: (build/sunbright mtime, runtime/gx_capture.cpp mtime,
#             runtime/overrides/fastboot_native.cpp mtime, STAGE, SCENARIO).
# The oracle is deterministic once state pinned (fastboot + probe /pad Start ×2).
#
# Usage: tools/render/oracle_png_cache.sh [OUT_PNG] [STAGE] [SCENARIO] [SETTLE_SECS]
#   defaults: scratch/oracle/title_gx_oracle.png 15 0 55
# SB_ORACLE_FORCE=1 forces recapture.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"

OUT="${1:-scratch/oracle/title_gx_oracle.png}"
STAGE="${2:-15}"
SCENARIO="${3:-0}"
SETTLE="${4:-55}"

mkdir -p "$(dirname "$OUT")"
STAMP="${OUT}.stamp"
DUMP_ORACLE="$HOME/.local/share/dolphin-emu/Dump/Frames"

_hash_inputs() {
    local h
    h="$(stat -c %Y build/sunbright 2>/dev/null || echo 0)"
    h="$h|$(stat -c %Y runtime/gx_capture.cpp 2>/dev/null || echo 0)"
    h="$h|$(stat -c %Y runtime/overrides/fastboot_native.cpp 2>/dev/null || echo 0)"
    h="$h|STAGE=$STAGE|SCENARIO=$SCENARIO|SETTLED_POST_START=1"
    echo "$h"
}

CURRENT="$(_hash_inputs)"
if [ -z "${SB_ORACLE_FORCE:-}" ] && [ -f "$OUT" ] && [ -f "$STAMP" ]; then
    CACHED="$(cat "$STAMP" 2>/dev/null || echo)"
    if [ "$CACHED" = "$CURRENT" ]; then
        echo "[oracle-png-cache] HIT $OUT" >&2
        echo "$OUT"; exit 0
    fi
    echo "[oracle-png-cache] MISS $OUT (stamp changed)" >&2
fi

source .env 2>/dev/null || { echo "[oracle-png-cache] no .env" >&2; exit 1; }
[ -x "build/sunbright" ] || { echo "[oracle-png-cache] build/sunbright missing" >&2; exit 1; }

echo "[oracle-png-cache] capturing PNG at settled post-Start (${SETTLE}s) → $OUT" >&2
pkill -9 -x sunbright 2>/dev/null; sleep 1
rm -f "$DUMP_ORACLE"/framedump_*.png

timeout -s KILL "$((SETTLE + 5))" env \
    SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_BACKEND=Vulkan \
    SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE="$STAGE" SUNBRIGHT_SCENARIO="$SCENARIO" \
    SUNBRIGHT_PROBE=1 SUNBRIGHT_DUMP=1 \
    ./build/sunbright "$SUNBRIGHT_ROM" > "${OUT%.png}.log" 2>&1 &
OPID=$!
# Wait for probe, press Start ×2.
for i in $(seq 1 30); do
    sleep 1
    curl -s --max-time 1 http://127.0.0.1:17654/metrics >/dev/null 2>&1 && break
done
# ONE Start press — a second would confirm file 1 and drop into gameplay.
curl -s --max-time 3 "http://127.0.0.1:17654/pad?do=start&ms=250" >/dev/null 2>&1 || true
sleep "$SETTLE"
pkill -9 -x sunbright 2>/dev/null
wait $OPID 2>/dev/null

# Pick the 2nd-newest PNG (newest may be truncated by SIGKILL mid-write).
mapfile -t OP < <(ls -t "$DUMP_ORACLE"/framedump_*.png 2>/dev/null)
if [ "${#OP[@]}" -lt 1 ]; then
    echo "[oracle-png-cache] FAIL no PNGs (log: ${OUT%.png}.log)" >&2; exit 2
fi
SRC="${OP[1]:-${OP[0]}}"
cp "$SRC" "$OUT"
echo "$CURRENT" > "$STAMP"
echo "[oracle-png-cache] wrote $OUT (from $SRC)" >&2
echo "$OUT"

#!/usr/bin/env bash
# oracle_cache.sh — cache the Dolphin-GX oracle parity capture keyed by
# (oracle binary mtime, gx_capture.cpp mtime, gx_parse.h mtime, capture config).
# The oracle is deterministic (headless fastboot, no user input), so once captured
# with a given (binary, code) it can be reused across divergence-hunt iterations
# without paying the 40s+ capture cost every time.
#
# Usage:
#   tools/render/oracle_cache.sh [OUT_JSONL] [STAGE] [SCENARIO] [SETTLE_SECS]
#     defaults: scratch/passes/cache/oracle_s15_c0.jsonl 15 0 40
#   Prints the path to a valid JSONL (either cached or freshly captured).
#   SB_ORACLE_FORCE=1 forces a recapture even if the cache is valid.
#
# The cache stores the JSONL alongside a .stamp file recording the input hash.
# Any mismatch (binary rebuilt, capture code changed, config changed) invalidates.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"

OUT="${1:-scratch/passes/cache/oracle_s15_c0.jsonl}"
STAGE="${2:-15}"
SCENARIO="${3:-0}"
SETTLE="${4:-40}"

mkdir -p "$(dirname "$OUT")"
STAMP="${OUT}.stamp"

# Cache key: the things that, if unchanged, guarantee the same JSONL output.
_hash_inputs() {
    local h
    h="$(stat -c %Y build/sunbright 2>/dev/null || echo 0)"
    h="$h|$(stat -c %Y runtime/gx_capture.cpp 2>/dev/null || echo 0)"
    h="$h|$(stat -c %Y runtime/gx_parse.h 2>/dev/null || echo 0)"
    h="$h|$(stat -c %Y runtime/overrides/fastboot_native.cpp 2>/dev/null || echo 0)"
    h="$h|STAGE=$STAGE|SCENARIO=$SCENARIO"
    echo "$h"
}

CURRENT="$(_hash_inputs)"
if [ -z "${SB_ORACLE_FORCE:-}" ] && [ -f "$OUT" ] && [ -f "$STAMP" ]; then
    CACHED="$(cat "$STAMP" 2>/dev/null || echo)"
    if [ "$CACHED" = "$CURRENT" ]; then
        echo "[oracle-cache] HIT $OUT ($(wc -l < "$OUT") frames)" >&2
        echo "$OUT"
        exit 0
    fi
    echo "[oracle-cache] MISS $OUT (stamp changed)" >&2
fi

source .env 2>/dev/null || { echo "[oracle-cache] no .env (need SUNBRIGHT_ROM)" >&2; exit 1; }
[ -x "build/sunbright" ] || { echo "[oracle-cache] build/sunbright missing (cmake --build build)" >&2; exit 1; }

# Drive Start via the probe HTTP endpoint so the oracle settles at the file-block
# sub-screen (where the reflective sea is visible), not the pre-Start PUSH-START
# screen. Set SB_ORACLE_NO_START=1 to skip the presses (keep raw title state).
echo "[oracle-cache] capturing (${SETTLE}s, with Start press unless SB_ORACLE_NO_START) → $OUT" >&2
pkill -9 -x sunbright 2>/dev/null; sleep 1
rm -f "$OUT"
timeout -s KILL "$((SETTLE + 5))" env \
    SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_BACKEND=Vulkan \
    SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE="$STAGE" SUNBRIGHT_SCENARIO="$SCENARIO" \
    SUNBRIGHT_PROBE=1 \
    SUNBRIGHT_PARITY_DUMP="$OUT" SUNBRIGHT_PARITY_DRAWS=1 \
    SUNBRIGHT_DUMP="${SUNBRIGHT_DUMP:-0}" \
    ./build/sunbright "$SUNBRIGHT_ROM" > "${OUT%.jsonl}.log" 2>&1 &
OPID=$!
# Wait for the probe to accept connections, then drive Start (twice — the first
# advances TCardLoad past PUSH-START, second gates file-slot picker).
if [ -z "${SB_ORACLE_NO_START:-}" ]; then
    for i in $(seq 1 30); do
        sleep 1
        curl -s --max-time 1 http://127.0.0.1:17654/metrics >/dev/null 2>&1 && break
    done
    # ONE press: PUSH-START → file-block sub-screen. A second press would confirm
    # file 1 and drop into gameplay/attract (verified 2026-07-02 by user against SBS).
    curl -s --max-time 3 "http://127.0.0.1:17654/pad?do=start&ms=250" >/dev/null 2>&1 || true
fi
sleep "$SETTLE"
pkill -9 -x sunbright 2>/dev/null
wait $OPID 2>/dev/null

FRAMES="$(wc -l < "$OUT" 2>/dev/null || echo 0)"
if [ "$FRAMES" -lt 5 ]; then
    echo "[oracle-cache] FAIL only $FRAMES frames (log: ${OUT%.jsonl}.log)" >&2
    exit 2
fi
echo "$CURRENT" > "$STAMP"
echo "[oracle-cache] wrote $OUT ($FRAMES frames)" >&2
echo "$OUT"

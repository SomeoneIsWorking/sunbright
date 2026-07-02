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

echo "[oracle-png-cache] capturing PNG at settled post-Start (${SETTLE}s, VI-timed press) → $OUT" >&2
pkill -9 -x sunbright 2>/dev/null; sleep 1
rm -f "$DUMP_ORACLE"/framedump_*.png

# VI-timed Start presses (deterministic — see oracle_cache.sh for the timing rationale
# and memory [[tcardload-title-to-fileselect-state-machine]]).
timeout -s KILL "$((SETTLE + 5))" env \
    SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_BACKEND=Vulkan \
    SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE="$STAGE" SUNBRIGHT_SCENARIO="$SCENARIO" \
    SUNBRIGHT_DUMP=1 \
    SUNBRIGHT_PAD_SCRIPT="700:START 732:- 1200:START 1232:-" \
    ./build/sunbright "$SUNBRIGHT_ROM" > "${OUT%.png}.log" 2>&1 &
OPID=$!
sleep "$SETTLE"
pkill -9 -x sunbright 2>/dev/null
wait $OPID 2>/dev/null

# Pick the 3rd-newest PNG (newest 2 often truncated by SIGKILL mid-write; verified with
# PIL — even numeric-name "second newest" was corrupted in one run).
mapfile -t OP < <(ls -t "$DUMP_ORACLE"/framedump_*.png 2>/dev/null)
if [ "${#OP[@]}" -lt 3 ]; then
    echo "[oracle-png-cache] FAIL <3 PNGs (log: ${OUT%.png}.log)" >&2; exit 2
fi
SRC="${OP[2]}"
cp "$SRC" "$OUT"
echo "$CURRENT" > "$STAMP"
echo "[oracle-png-cache] wrote $OUT (from $SRC)" >&2
echo "$OUT"

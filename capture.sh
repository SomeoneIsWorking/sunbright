#!/usr/bin/env bash
# Cheat-search for Mario's transform in RAM, driven entirely by you with F5.
#
# Dolphin's chatter is sent to a log; only the [mario] prompts show in this terminal,
# and the results land in ./mario_candidates.txt (not lost in the noise).
#
# Steps (window focused, after you reach the file-select where Mario is walkable):
#   1. Mario STILL            → press F5   (S1)
#   2. still STILL            → press F5   (S2  — baseline: kills animation noise)
#   3. move RIGHT, stop       → press F5   (S3)
#   4. STILL again            → press F5   (S4)
#   5. tiny move LEFT, stop   → press F5   (S5 → writes mario_candidates.txt)
# Repeat anytime; the file is overwritten each run. Quit the game (close window) when done.

set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/build/sunbright"
ROM="${1:-$SUNBRIGHT_ROM}"
mkdir -p "$HERE/scratch/logs"
LOG="${SUNBRIGHT_LOG:-$HERE/scratch/logs/sunbright_capture.log}"
OUT="$HERE/mario_candidates.txt"

[[ -x "$BIN" ]] || { echo "build first: cmake --build \"$HERE/build\" --target sunbright -j\$(nproc)" >&2; exit 1; }
[[ -f "$ROM" ]] || { echo "ROM not found: $ROM" >&2; exit 1; }

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
export SUNBRIGHT_BACKEND="${SUNBRIGHT_BACKEND:-OGL}"
export DISPLAY="${DISPLAY:-:0}"
export SUNBRIGHT_FINDMARIO=1

: > "$OUT"
cat <<EOF
[capture] Dolphin logs → $LOG ; results → $OUT
[capture] F5 sequence: still, still, →RIGHT, still, ←LEFT(tiny). Watch the [mario] prompts.
EOF

# stdout (the [mario] prompts) stays on the terminal; stderr (Dolphin) goes to the log.
"$BIN" "$ROM" 2>"$LOG"

echo
echo "===== mario_candidates.txt ====="
cat "$OUT"

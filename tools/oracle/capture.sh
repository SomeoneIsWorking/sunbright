#!/usr/bin/env bash
# tools/oracle/capture.sh — pixel ground truth from the Dolphin oracle.
#
# Runs Super Mario Sunshine under Dolphin with frame dumping enabled for
# DURATION seconds (default 75 — enough to reach the title screen from cold
# boot), then extracts per-second PNG frames into scratch/oracle/frames/.
#
# Usage:
#   tools/oracle/capture.sh [DURATION] [ROM]
# ROM defaults to $SUNBRIGHT_ROM / .env / rom.rvz (same resolution as run.sh).
# Dolphin binary: $ORACLE_DOLPHIN, else the extern/dolphin fork build if
# present, else system dolphin-emu.
#
# NOTE (perf trap, memory [[perf-dump-config-and-probe]]): DumpFrames is passed
# as a -C override, NOT persisted to <home>/.config/dolphin-emu — do not enable it
# in the saved config or every later run crawls under the FrameDumper.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }

DURATION="${1:-75}"
ROM="${2:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"
[[ -f "$ROM" ]] || { echo "[oracle] ROM not found: $ROM" >&2; exit 1; }

BIN="${ORACLE_DOLPHIN:-}"
if [[ -z "$BIN" ]]; then
    for c in "$HERE/extern/dolphin/build/Binaries/dolphin-emu" /usr/bin/dolphin-emu; do
        [[ -x "$c" ]] && { BIN="$c"; break; }
    done
fi
[[ -n "$BIN" ]] || { echo "[oracle] no dolphin binary found" >&2; exit 1; }

DUMPDIR="$HOME/.local/share/dolphin-emu/Dump/Frames"
OUT="$HERE/scratch/oracle"
mkdir -p "$OUT/frames"
rm -f "$DUMPDIR"/framedump* 2>/dev/null || true

echo "[oracle] $BIN  \"$ROM\"  ${DURATION}s (EmulationSpeed=0 = unthrottled)"
SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}" timeout -s TERM "$DURATION" "$BIN" -b -e "$ROM" \
    -C Dolphin.Movie.DumpFrames=True \
    -C Dolphin.Core.EmulationSpeed=0 \
    > "$OUT/dolphin_run.log" 2>&1 || true

DUMP="$(ls -t "$DUMPDIR"/framedump*.avi 2>/dev/null | head -1)"
[[ -n "$DUMP" ]] || { echo "[oracle] no framedump produced — see $OUT/dolphin_run.log" >&2; exit 2; }

# One frame per second of footage, named by index.
rm -f "$OUT"/frames/oracle_*.png
ffmpeg -loglevel error -i "$DUMP" -vf fps=1 "$OUT/frames/oracle_%04d.png"
N=$(ls "$OUT"/frames/oracle_*.png | wc -l)
echo "[oracle] extracted $N frames -> $OUT/frames/"
# Refuse-degenerate guard: an all-black capture means the run never rendered.
LAST="$(ls "$OUT"/frames/oracle_*.png | tail -1)"
MEAN=$(magick "$LAST" -format "%[fx:mean]" info:)
awk -v m="$MEAN" 'BEGIN { exit (m < 0.01) ? 0 : 1 }' && {
    echo "[oracle] REFUSING: last frame is black (mean=$MEAN) — capture is degenerate" >&2
    exit 3
}
echo "[oracle] last frame: $LAST (mean=$MEAN)"

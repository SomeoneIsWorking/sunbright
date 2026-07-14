#!/usr/bin/env bash
# tools/oracle/record_fifo.sh — HEADLESS Dolphin FIFO (.dff) recording. No GUI.
#
# Replaces the retired xdrive.py GUI-driving hack. Uses the Dolphin fork's custom
# `--fifo-record` NoGUI flag (SomeoneIsWorking/dolphin@sunbright,
# Source/Core/DolphinNoGUI/MainNoGUI.cpp). Boot is deterministic under
# EmulationSpeed=0, so a VI-field count reliably lands on a scene.
#
# Usage:
#   record_fifo.sh <out.dff> [after_fields=7500] [frames=3] [rom]
#
# Boot field landmarks (GMSE01, deterministic): intro (GC logo + Peach movie) runs
# through ~field 6000 (a THP movie = ~3.6 KB/frame); TITLE reached ~6600; SETTLED
# title ~7500 (before the ~45 s / ~2700-field attract loop). To find a NEW scene,
# sweep `after` and accept when the parsed fifo_data_size jumps to ~275 KB (title)
# — a movie stays ~3.6 KB. Validate a capture with tools/oracle/parse_fifo_dff.py
# (title >1000 draws) and/or SB_FIFO_REPLAY through aurora.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }

OUT="${1:?usage: record_fifo.sh <out.dff> [after_fields] [frames] [rom]}"
AFTER="${2:-7500}"
FRAMES="${3:-3}"
ROM="${4:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"
[[ -f "$ROM" ]] || { echo "[record_fifo] ROM not found: $ROM" >&2; exit 1; }

BIN="${SB_DOLPHIN_NOGUI:-$HERE/extern/dolphin_fork/build/Binaries/dolphin-emu-nogui}"
[[ -x "$BIN" ]] || {
    echo "[record_fifo] headless Dolphin not built: $BIN" >&2
    echo "  build it: cd extern/dolphin_fork && git submodule update --init --depth 1 &&" >&2
    echo "    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=OFF -DENABLE_TESTS=OFF \\" >&2
    echo "      -DENABLE_EVDEV=OFF -DUSE_MGBA=OFF && cmake --build build --target dolphin-emu-nogui" >&2
    exit 1
}

mkdir -p "$(dirname "$OUT")"
echo "[record_fifo] $BIN -> $OUT  (after $AFTER fields, $FRAMES frame(s))"
# -v Null: FifoRecorder taps the GP FIFO upstream of the video backend, so the null
# backend records the exact GX stream and is fastest. DISPLAY unset = no window.
DISPLAY= timeout -s KILL 180 "$BIN" -p headless -v Null -e "$ROM" \
    --fifo-record="$OUT" --fifo-record-after="$AFTER" --fifo-record-frames="$FRAMES" \
    -C Dolphin.Core.EmulationSpeed=0 -C Dolphin.Interface.UsePanicHandlers=False \
    2>&1 | grep -a "sb-fifo" || true

[[ -f "$OUT" ]] || { echo "[record_fifo] FAILED: no .dff written" >&2; exit 1; }
python3 - "$OUT" <<'PY'
import sys; sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__) if False else '.', 'tools/oracle'))
sys.path.insert(0, 'tools/oracle')
import parse_fifo_dff as p
buf = open(sys.argv[1], 'rb').read()
h = p.parse_header(buf)
sizes = [p.parse_frame_info(buf, h.frame_list_offset + i*p.FRAME_SIZE).fifo_data_size
         for i in range(h.frame_count)]
scene = 'TITLE/3D' if sizes and sizes[0] > 100000 else 'movie/blank'
print(f"[record_fifo] {h.frame_count} frame(s), {sizes} cmd bytes -> {scene}")
PY

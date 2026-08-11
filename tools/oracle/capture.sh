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
# Dolphin binary: $ORACLE_DOLPHIN, else the extern/dolphin_fork build, else a system
# dolphin-emu — and it SAYS which, because they are not the same oracle (see below).
#
# NOTE (perf trap, memory [[perf-dump-config-and-probe]]): DumpFrames is passed
# as a -C override, NOT persisted to the Dolphin config dir — do not enable it
# in the saved config or every later run crawls under the FrameDumper.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }

DURATION="${1:-75}"
ROM="${2:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"
[[ -f "$ROM" ]] || { echo "[oracle] ROM not found: $ROM" >&2; exit 1; }

# WHICH DOLPHIN, AND SAY SO. This searched `extern/dolphin/build/Binaries/dolphin-emu`, which is
# wrong twice over: `extern/dolphin` is the pinned UPSTREAM submodule and has never been
# initialised on this machine (0 bytes), and the fork does not build a target by that name — it
# builds `dolphin-emu-nogui` under `extern/dolphin_fork`. So the loop never matched and every run
# silently fell through to /usr/bin/dolphin-emu, which exists here.
#
# That fallback is not a lesser oracle, it is a DIFFERENT one: the fork
# (SomeoneIsWorking/dolphin@sunbright) carries the instrumentation this project's oracle work
# depends on — the SB_ORACLE_DRAWLOG hook in VertexManagerBase::Flush, and the --fifo-record NoGUI
# flag that tools/oracle/record_fifo.sh relies on. A capture taken from stock Dolphin looks exactly
# like a capture from the fork and answers a different question. record_fifo.sh, written later,
# always had the right path; this script was never updated to match.
BIN="${ORACLE_DOLPHIN:-}"
BIN_SRC="ORACLE_DOLPHIN"
if [[ -z "$BIN" ]]; then
    FORK="$HERE/extern/dolphin_fork/build/Binaries/dolphin-emu-nogui"
    if [[ -x "$FORK" ]]; then
        BIN="$FORK"; BIN_SRC="extern/dolphin_fork (instrumented)"
    else
        for c in /usr/bin/dolphin-emu /usr/bin/dolphin-emu-nogui; do
            [[ -x "$c" ]] && { BIN="$c"; BIN_SRC="SYSTEM dolphin (NOT instrumented)"; break; }
        done
    fi
fi
[[ -n "$BIN" ]] || {
    echo "[oracle] no dolphin binary found. Build the fork:" >&2
    echo "  cd extern/dolphin_fork && git submodule update --init --depth 1 &&" >&2
    echo "  cmake -B build -DENABLE_QT=OFF -DENABLE_EVDEV=OFF -DUSE_MGBA=OFF &&" >&2
    echo "  cmake --build build --target dolphin-emu-nogui" >&2
    exit 1
}
echo "[oracle] dolphin: $BIN   <- $BIN_SRC"
if [[ "$BIN_SRC" == SYSTEM* ]]; then
    echo "[oracle] WARNING: this is a STOCK Dolphin, not the sunbright fork. It has no" >&2
    echo "         SB_ORACLE_DRAWLOG hook and no --fifo-record support, so any result that" >&2
    echo "         depends on those is not merely missing here — it will be ABSENT while the" >&2
    echo "         capture still looks complete. Set ORACLE_DOLPHIN, or build the fork." >&2
fi

DUMPDIR="$HOME/.local/share/dolphin-emu/Dump/Frames"
OUT="$HERE/scratch/oracle"
mkdir -p "$OUT/frames"

# BUG (found 2026-07-10): /usr/bin/dolphin-emu is a wrapper script that execs the real
# dolphin-emu-x11 core; `timeout -s TERM <wrapper>` only signals the wrapper — the core
# survives as an orphan, keeps running (and keeps writing framedump_*.png) forever after
# this script returns. Stray instances then fight the NEXT run for the GPU/window and race
# on $DUMPDIR, producing 0-frame / silently-truncated captures. Kill any stragglers before
# AND after every run — do not rely on `timeout` alone for this wrapper.
kill_dolphin_stragglers() {
    pkill -9 -x dolphin-emu-x11 2>/dev/null || true
    pkill -9 -x dolphin-emu 2>/dev/null || true
}
kill_dolphin_stragglers
find "$DUMPDIR" -maxdepth 1 -name 'framedump*' -delete 2>/dev/null || true

echo "[oracle] $BIN  \"$ROM\"  ${DURATION}s (EmulationSpeed=0 = unthrottled)"
SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}" timeout -s TERM "$DURATION" "$BIN" -b -e "$ROM" \
    -C Dolphin.Movie.DumpFrames=True \
    -C Dolphin.Core.EmulationSpeed=0 \
    > "$OUT/dolphin_run.log" 2>&1 || true
kill_dolphin_stragglers

# NOTE: under `set -eo pipefail`, `ls` on a non-matching glob exits non-zero and — even
# piped into `head` — becomes the pipeline's (and thus this assignment's) exit status,
# which `set -e` treats as a script-ending failure. This is silent and easy to miss: the
# script exits right here with NO error message, right after the "[oracle] ... booting"
# line. The `|| true` is load-bearing — do not remove it.
DUMP="$(ls -t "$DUMPDIR"/framedump*.avi 2>/dev/null | head -1 || true)"
rm -f "$OUT"/frames/oracle_*.png
if [[ -n "$DUMP" ]]; then
    # AVI path (Dolphin built with FFmpeg): one frame per second of footage.
    ffmpeg -loglevel error -i "$DUMP" -vf fps=1 "$OUT/frames/oracle_%04d.png"
else
    # This Dolphin build has no FFmpeg (VideoCommon/FrameDumper.cpp falls back to
    # PNG-per-VI-frame: $DUMPDIR/framedump_<N>.png, one file per presented field —
    # actually BETTER than the AVI path since it's frame-exact, not 1fps-sampled.
    # Symlink them into scratch/oracle/frames/ under a stable, numerically-sortable name.
    shopt -s nullglob
    pngs=("$DUMPDIR"/framedump_*.png)
    shopt -u nullglob
    [[ ${#pngs[@]} -gt 0 ]] || { echo "[oracle] no framedump*.avi AND no framedump_*.png — see $OUT/dolphin_run.log" >&2; exit 2; }
    # Sort numerically by the frame index embedded in the filename.
    i=0
    for f in "${pngs[@]}"; do
        n="${f##*/framedump_}"; n="${n%.png}"
        # Zero-pad to 8 digits so both numeric AND lexicographic (ls/sort/tail -1)
        # order agree — the refuse-degenerate check below relies on plain `ls | tail -1`.
        ln -f "$f" "$(printf '%s/oracle_vi%08d.png' "$OUT/frames" "$n")"
        i=$((i+1))
    done
    echo "[oracle] no FFmpeg in this Dolphin build — used PNG-per-VI-frame fallback ($i frames, frame-exact)"
fi
N=$(ls "$OUT"/frames/oracle_*.png 2>/dev/null | wc -l)
echo "[oracle] extracted $N frames -> $OUT/frames/"
[[ "$N" -gt 0 ]] || { echo "[oracle] REFUSING: 0 frames extracted" >&2; exit 2; }
# Refuse-degenerate guard: an all-black capture means the run never rendered.
LAST="$(ls "$OUT"/frames/oracle_*.png | tail -1)"
MEAN=$(magick "$LAST" -format "%[fx:mean]" info:)
awk -v m="$MEAN" 'BEGIN { exit (m < 0.01) ? 0 : 1 }' && {
    echo "[oracle] REFUSING: last frame is black (mean=$MEAN) — capture is degenerate" >&2
    exit 3
}
echo "[oracle] last frame: $LAST (mean=$MEAN)"

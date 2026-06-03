#!/usr/bin/env bash
# Run Sunbright and drive the GameCube pad with live, timed commands over a FIFO
# control channel (SUNBRIGHT_REPL). Launches the game, then either plays a command
# script, reads piped commands, or gives you an interactive prompt — and stays
# attached so the game keeps running while your commands play out.
#
# Usage:
#   ./repl.sh                       # interactive: type commands live (Ctrl-D = quit)
#   ./repl.sh into_game.txt         # play a script, then drop to interactive
#   echo "start 120" | ./repl.sh    # pipe commands, then wait for the game
#   ./repl.sh [rom.rvz] [script]    # explicit ROM and/or script (any order)
#
# Command grammar (one per line; '#' starts a comment):
#   <combo> [ms]   hold a button combo for ms (default 150). parts joined by '+':
#                  a b x y z start l r up down left right.   e.g.  left+a 80
#   jump [ms]      alias for 'a' (Z key / A button)
#   wait <ms>      idle for ms (also: w <ms>)
#   snap           run the find-Mario cheat-search snapshot step
#   mtx <hexaddr>  print the 3x4 transform translation at <addr>
#   say <text>     echo a progress marker
#   quit           stop the game
#
# Tweak timings in a script file (e.g. into_game.txt) and re-run: ./repl.sh into_game.txt
set -eo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/build/sunbright"
mkdir -p "$HERE/scratch/logs"
FIFO="${SUNBRIGHT_REPL:-$HERE/scratch/sunbright.ctl}"

# Args (any order): a ROM image and/or a command-script file.
ROM="$SUNBRIGHT_ROM"
SCRIPT=""
for arg in "$@"; do
    case "$arg" in
        *.rvz|*.iso|*.gcm|*.dol) ROM="$arg" ;;
        *) [[ -f "$arg" ]] && SCRIPT="$arg" ;;
    esac
done

[[ -x "$BIN" ]] || { echo "build first: cmake --build \"$HERE/build\" --target sunbright -j\$(nproc)" >&2; exit 1; }
[[ -f "$ROM" ]] || { echo "ROM not found: $ROM" >&2; exit 1; }

# Working video path (see run.sh) + the control FIFO.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
export SUNBRIGHT_BACKEND="${SUNBRIGHT_BACKEND:-OGL}"
export DISPLAY="${DISPLAY:-:0}"
export SUNBRIGHT_REPL="$FIFO"
LOG="${SUNBRIGHT_LOG:-$HERE/scratch/logs/sunbright_repl.log}"

echo "[repl] FIFO=$FIFO   log=$LOG"
echo "[repl] ROM=$ROM"

# Launch the game in the background; Dolphin chatter (stderr) → log, the
# [repl]/[mario]/[interp] markers (stdout) → this terminal.
rm -f "$FIFO"
"$BIN" "$ROM" 2>"$LOG" &
GAME_PID=$!

cleanup() {
    # Only send quit if the game is still alive; then reap and tidy the FIFO.
    if kill -0 "$GAME_PID" 2>/dev/null; then
        printf 'quit\n' > "$FIFO" 2>/dev/null || true
        sleep 0.3
        kill "$GAME_PID" 2>/dev/null || true
    fi
    rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

# Wait for the game to create the FIFO (it does so once SDL is up).
for _ in $(seq 1 150); do
    [[ -p "$FIFO" ]] && break
    kill -0 "$GAME_PID" 2>/dev/null || { echo "[repl] game exited during boot; see $LOG" >&2; exit 1; }
    sleep 0.1
done
[[ -p "$FIFO" ]] || { echo "[repl] game never opened the FIFO; see $LOG" >&2; exit 1; }

send() { printf '%s\n' "$1" > "$FIFO"; }

# 1) Play a script file if given (commands are queued instantly; the game runs
#    them out over wall-clock time per the wait/ms values).
if [[ -n "$SCRIPT" ]]; then
    echo "[repl] playing $SCRIPT"
    while IFS= read -r line || [[ -n "$line" ]]; do send "$line"; done < "$SCRIPT"
fi

# 2) Then take live input.
if [[ -t 0 ]]; then
    echo "[repl] type commands (Ctrl-D quits). e.g.  start 120 | left 90 | jump 100 | snap"
    while IFS= read -r -p "sb> " line; do send "$line"; done
elif [[ -z "$SCRIPT" ]]; then
    # Piped commands (no script arg, not a tty): forward them all.
    while IFS= read -r line; do send "$line"; done
fi

# 3) Stay attached so queued actions finish — wait for the game to exit on its
#    own (a `quit` command or you closing the window). Ctrl-C also stops it.
echo "[repl] input closed; staying attached until the game exits (Ctrl-C to stop)"
wait "$GAME_PID" 2>/dev/null || true

#!/usr/bin/env bash
# Launch Sunbright (statically-recompiled Super Mario Sunshine) with a video setup
# that actually works on this machine.
#
# Why this is needed: Dolphin's GL/Vulkan backends can't create a surface on a
# *native Wayland* window (you get "failed to initialize video backend"). Forcing
# SDL to the x11 driver routes the window through XWayland, and the OpenGL backend
# works there. So we pin SDL_VIDEODRIVER=x11 and SUNBRIGHT_BACKEND=OGL.
#
# Usage:
#   ./run.sh [rom.rvz] [extra sunbright args...]
#   SUNBRIGHT_BACKEND=Vulkan ./run.sh         # override the backend
#   SUNBRIGHT_DUMP=1 ./run.sh                 # dump frames to <home>/.local/share/dolphin-emu/Dump/Frames
#   SUNBRIGHT_AUTOSTART=1 ./run.sh            # auto-press Start/A (headless demo)
#   (any SUNBRIGHT_* debug var set in your env is passed through)
#
# Keyboard → GameCube pad (window must be focused):
#   Enter=Start  Z=A(jump)  X=B  C=X  V=Y  Q=Z  A=L  S=R(spray)  arrows=control stick
#   F11=fullscreen

set -eo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/build/sunbright"
ROM="${1:-$SUNBRIGHT_ROM}"

if [[ ! -x "$BIN" ]]; then
    echo "sunbright not built. Build it with:" >&2
    echo "  cmake --build \"$HERE/build\" --target sunbright -j\$(nproc)" >&2
    exit 1
fi
if [[ ! -f "$ROM" ]]; then
    echo "ROM not found: $ROM" >&2
    echo "Pass one explicitly:  ./run.sh /path/to/game.rvz" >&2
    exit 1
fi

# Pin the working video path (override by exporting these before calling).
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
export SUNBRIGHT_BACKEND="${SUNBRIGHT_BACKEND:-OGL}"
export DISPLAY="${DISPLAY:-:0}"

echo "[run] SDL_VIDEODRIVER=$SDL_VIDEODRIVER  SUNBRIGHT_BACKEND=$SUNBRIGHT_BACKEND  DISPLAY=$DISPLAY"
echo "[run] $BIN \"$ROM\""
exec "$BIN" "$ROM" "${@:2}"

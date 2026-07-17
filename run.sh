#!/usr/bin/env bash
# Launch sms-boot — the Super Mario Sunshine PC port (decomp/sms + Aurora).
#
# Usage:
#   ./run.sh [rom.rvz]        # ROM also via $SUNBRIGHT_ROM, .env, or rom.rvz drop-in
#   SB_STAGE=15 ./run.sh      # fastboot destination (15 = title screen)
#   SB_TURBO=1 ./run.sh       # unpaced (no NTSC-field frame pacing)
#   SB_W=1920 SB_H=1080 ./run.sh
#
# Default (no SB_STAGE/SB_SCENARIO/SB_NO_FASTBOOT set): the vanilla boot flow
# (GC logo -> title/attract) — the currently WORKING part of the port. The
# game's built-in fastboot default is Delfino Plaza, which still OSPanics at
# the unported TMapObjTree::initMapObj, so run.sh opts out of fastboot unless
# a destination is explicitly requested.
#
# Keyboard drives pad 0. Closing the window quits.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Native Wayland can't create the Vulkan surface on this machine; XWayland works.
if [[ "$(uname)" != "Darwin" ]]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
    export DISPLAY="${DISPLAY:-:0}"
fi

BIN="$HERE/build/sms-boot/sms-boot"
if [[ ! -x "$BIN" ]]; then
    echo "[run] building sms-boot ..." >&2
    cmake -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release >&2
    cmake --build "$HERE/build" --target sms-boot -j"$NCPU" >&2
fi

ROM="${1:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"
[[ -f "$ROM" ]] || { echo "[run] ROM not found: $ROM (set SUNBRIGHT_ROM or drop rom.rvz)" >&2; exit 1; }

# No explicit destination -> vanilla boot flow (see header comment).
if [[ -z "${SB_STAGE:-}" && -z "${SB_SCENARIO:-}" && -z "${SB_NO_FASTBOOT:-}" ]]; then
    export SB_NO_FASTBOOT=1
fi

export SUNBRIGHT_ROM="$ROM"
echo "[run] sms-boot  \"$ROM\""
exec "$BIN"

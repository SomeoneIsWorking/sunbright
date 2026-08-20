#!/usr/bin/env bash
# Launch sms-boot — the decomp/sms + Aurora verification oracle.
#
# Usage:
#   ./run-decomp.sh [rom.rvz]
#   SB_STAGE=15 ./run-decomp.sh
#   SB_TURBO=1 ./run-decomp.sh
#   SB_W=1920 SB_H=1080 ./run-decomp.sh
#
# This is a development oracle, not the default product. ./run.sh launches the standalone recomp
# runtime, which runs the whole game and owns the RmlUi prelaunch settings screen.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

. "$HERE/tools/launch/sdl_video.sh"
configure_sunbright_sdl_video

BIN="$HERE/build/sms-boot/sms-boot"
if [[ ! -x "$BIN" ]]; then
    echo "[run-decomp] building sms-boot ..." >&2
    cmake -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release >&2
    cmake --build "$HERE/build" --target sms-boot -j"$NCPU" >&2
fi

ROM="${1:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"
[[ -f "$ROM" ]] || {
    echo "[run-decomp] ROM not found: $ROM (set SUNBRIGHT_ROM or drop rom.rvz)" >&2
    exit 1
}

if [[ -z "${SB_STAGE:-}" && -z "${SB_SCENARIO:-}" && -z "${SB_NO_FASTBOOT:-}" ]]; then
    export SB_NO_FASTBOOT=1
fi

export SUNBRIGHT_ROM="$ROM"
echo "[run-decomp] sms-boot  \"$ROM\""
exec "$BIN"

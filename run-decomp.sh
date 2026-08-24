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
# runtime, which runs the whole game and owns the in-game Escape settings screen.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

. "$HERE/tools/launch/sdl_video.sh"
configure_sunbright_sdl_video

BIN="$HERE/build/sms-boot/sms-boot"
ROM="${1:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"
[[ -f "$ROM" ]] || {
    echo "[run-decomp] ROM not found: $ROM (set SUNBRIGHT_ROM or drop rom.rvz)" >&2
    exit 1
}

# The launcher is a shipping interface: an executable merely existing does not prove that it was
# built from the current decomp, Aurora, or host sources. Configure with the required compiler and
# ask CMake to prove the real sms-boot target current on every launch. The top-level CMake project
# independently rejects any compiler whose detected ID is not Clang.
echo "[run-decomp] configuring sms-boot with clang++ ..." >&2
cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++ >&2
echo "[run-decomp] ensuring sms-boot is current ..." >&2
cmake --build "$HERE/build" --target sms-boot -j"$NCPU" >&2

if [[ -z "${SB_STAGE:-}" && -z "${SB_SCENARIO:-}" && -z "${SB_NO_FASTBOOT:-}" ]]; then
    export SB_NO_FASTBOOT=1
fi

export SUNBRIGHT_ROM="$ROM"
echo "[run-decomp] sms-boot  \"$ROM\""
exec "$BIN"

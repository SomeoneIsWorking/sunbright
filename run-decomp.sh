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
readonly _SUNBRIGHT_SAFE_RUN_CAPTURE="${SUNBRIGHT_SAFE_RUN:-0}"
readonly _SUNBRIGHT_SAFE_RENDERER_CAPTURE="${SUNBRIGHT_SAFE_RENDERER:-}"
readonly _SUNBRIGHT_SAFE_HEADLESS_CAPTURE="${SUNBRIGHT_SAFE_HEADLESS:-}"
readonly _SUNBRIGHT_SAFE_MUTE_CAPTURE="${SUNBRIGHT_SAFE_MUTE:-}"
readonly _SUNBRIGHT_SAFE_HZ_CAPTURE="${SUNBRIGHT_SAFE_MAX_PRESENT_HZ:-}"
readonly _SUNBRIGHT_SAFE_RADV_DEBUG_CAPTURE="${SUNBRIGHT_SAFE_RADV_DEBUG-}"
readonly _SUNBRIGHT_SAFE_FASTBOOT_CAPTURE="${SUNBRIGHT_SAFE_FASTBOOT:-}"
readonly _SUNBRIGHT_SAFE_STAGE_CAPTURE="${SUNBRIGHT_SAFE_STAGE:-}"
readonly _SUNBRIGHT_SAFE_SCENARIO_CAPTURE="${SUNBRIGHT_SAFE_SCENARIO:-}"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
if [ "$_SUNBRIGHT_SAFE_RUN_CAPTURE" = "1" ]; then
    export SBR_RENDERER="$_SUNBRIGHT_SAFE_RENDERER_CAPTURE"
    export RADV_DEBUG="$_SUNBRIGHT_SAFE_RADV_DEBUG_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_HEADLESS_CAPTURE" ] || export SB_HEADLESS="$_SUNBRIGHT_SAFE_HEADLESS_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_MUTE_CAPTURE" ] || export SBR_MUTE="$_SUNBRIGHT_SAFE_MUTE_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_HZ_CAPTURE" ] || export SB_MAX_PRESENT_HZ="$_SUNBRIGHT_SAFE_HZ_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_FASTBOOT_CAPTURE" ] || export SBR_FASTBOOT="$_SUNBRIGHT_SAFE_FASTBOOT_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_STAGE_CAPTURE" ] || export SB_STAGE="$_SUNBRIGHT_SAFE_STAGE_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_SCENARIO_CAPTURE" ] || export SB_SCENARIO="$_SUNBRIGHT_SAFE_SCENARIO_CAPTURE"
fi
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
# built from the current decomp, Aurora, or host sources. Debug is deliberately optimized by the
# shared build policy, so assertions, symbols, Dawn validation/robustness, and GPU labels stay live
# without turning the game into an -O0 workload.
echo "[run-decomp] configuring optimized Debug with clang++ ..." >&2
cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER=clang++ >&2
echo "[run-decomp] ensuring sms-boot is current ..." >&2
cmake --build "$HERE/build" --target sms-boot -j"$NCPU" >&2

if [[ -z "${SB_STAGE:-}" && -z "${SB_SCENARIO:-}" && -z "${SB_NO_FASTBOOT:-}" ]]; then
    export SB_NO_FASTBOOT=1
fi

export SUNBRIGHT_ROM="$ROM"
echo "[run-decomp] sms-boot  \"$ROM\""
exec "$BIN"

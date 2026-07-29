#!/usr/bin/env bash
# Launch sms-recomp with the NATIVE SDL3-GPU renderer actually turned on, in the plaza.
#
# WHY THIS EXISTS. The native renderer needs SIX environment variables set together, and omitting
# any one produces a silent, plausible-looking failure rather than an error:
#
#   SBR_SDLGPU=1        without it the native renderer never runs (aurora renders, you see a
#                       correct frame, and every native measurement is of nothing)
#   SBR_J3D_CAPTURE=1   without it the scene has 0 drawables — GX still streams, so the logs look
#                       busy and the frame is empty
#   SBR_TEX=1           without it every surface is untextured flat colour
#   SBR_FASTBOOT=1      without it you sit in the menus
#   SBR_STAGE=1         plain fastboot derives the episode from the save; the save here is at
#   SBR_SCENARIO=0      episode 5, which does not render — you get an empty scene for 400+ presents
#
# Four separate debugging runs were lost to those, each looking like a render defect. Anything that
# needs a plaza frame should go through this script rather than reassembling the incantation.
#
# Usage:  ./run-render.sh [extra env assignments...] -- [args to run-recomp.sh]
#   ./run-render.sh SBR_AB=1 SBR_RENDER_DUMP=scratch/bin/f.rgba
#   ./run-render.sh SBR_LUCENT_DEBUG=nrender,ab SBR_ABLATE=1
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export SB_HEADLESS="${SB_HEADLESS:-1}"     # never show a window: required for automated runs
export SB_TURBO="${SB_TURBO:-1}"
export SBR_FASTBOOT="${SBR_FASTBOOT:-1}"
export SBR_STAGE="${SBR_STAGE:-1}"
export SBR_SCENARIO="${SBR_SCENARIO:-0}"
export SBR_SDLGPU="${SBR_SDLGPU:-1}"
export SBR_J3D_CAPTURE="${SBR_J3D_CAPTURE:-1}"
export SBR_TEX="${SBR_TEX:-1}"

# Pass through NAME=VALUE arguments as environment, everything after -- to run-recomp.sh.
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --) shift; ARGS=("$@"); break ;;
        *=*) export "${1?}" ;;
        *) ARGS+=("$1") ;;
    esac
    shift
done

# A render run without a timeout wedges the GPU for the next one; cap it by default.
SECS="${SBR_RUN_SECS:-330}"
exec timeout -s KILL "$SECS" "$HERE/run-recomp.sh" "${ARGS[@]}"

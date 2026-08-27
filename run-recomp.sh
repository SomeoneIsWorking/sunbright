#!/usr/bin/env bash
# Launch sms-recomp — Super Mario Sunshine's real PowerPC code, statically recompiled,
# running on native device models + Aurora. This is the raw development launcher; ./run.sh is the
# product path and ./run-decomp.sh launches the decomp verification oracle.
#
# Usage:
#   ./run-recomp.sh                  # ROM via $SUNBRIGHT_ROM, .env, or rom.rvz drop-in
#   ./run-recomp.sh [rom.rvz] [sms.dol]
#   SB_W=1920 SB_H=1080 ./run-recomp.sh
#   SBR_LUCENT_DEBUG=card,gxfifo ./run-recomp.sh    # per-channel diagnostics
#   SB_TURBO=1 ./run-recomp.sh                     # unpaced (no frame limiting)
#
# WHAT TO EXPECT: boots GC logo -> title. Press START (Enter) at the title to reach
# file-select, which mounts your real Dolphin memory card and shows your saves; choosing a
# file loads Delfino Plaza. Title, file-select and the plaza all render (title/file-select
# verified per-region against the decomp oracle and a retail Dolphin capture, 2026-07-22).
#
#   SBR_MUTE=1 ./run-recomp.sh            # silent to the speakers; the audio path still runs
#   SBR_QUIT_AFTER=320 ./run-recomp.sh    # quit after N presents (short, bounded automated runs)
#
#   SBR_FASTBOOT=1 ./run-recomp.sh        # skip the menus: File 1 -> Delfino Plaza
#   SBR_STAGE=6 ./run-recomp.sh           # boot a specific stage (SBR_SCENARIO=<n> too)
#
# Attract movies and cutscenes do not play (SBR_THP=all tries, and breaks on the second
# movie); the plaza's own video decodes for real.
#
# Runs at the game's own rate — it paces to the retrace count the game asks for, so the
# menus run at 30fps like the console. SB_TURBO=1 removes pacing (runs as fast as the host
# manages, ~120fps here); useful for automated runs, wrong for playing.
#
# Keyboard drives pad 0: Enter = START, X = A, Z = B, arrows = stick. Closing the window
# quits, as does Ctrl-C or SIGTERM. For an unattended run use SBR_PAD_SCRIPT="600:START,640:-" (keys on
# PAD read count) together with SB_HEADLESS=1.
#
# Memory card: slot A is a real Dolphin card image, shared with Dolphin so saves stay
# interchangeable. Auto-detected under ~/.local/share/dolphin-emu/GC/; override with
# SBR_CARD_A=/path/to/MemoryCardA.USA.raw, or point it at a nonexistent file to run
# with an empty slot.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly _SUNBRIGHT_SAFE_RUN_CAPTURE="${SUNBRIGHT_SAFE_RUN:-0}"
readonly _SUNBRIGHT_SAFE_RENDERER_CAPTURE="${SUNBRIGHT_SAFE_RENDERER:-}"
readonly _SUNBRIGHT_SAFE_HEADLESS_CAPTURE="${SUNBRIGHT_SAFE_HEADLESS:-}"
readonly _SUNBRIGHT_SAFE_MUTE_CAPTURE="${SUNBRIGHT_SAFE_MUTE:-}"
readonly _SUNBRIGHT_SAFE_HZ_CAPTURE="${SUNBRIGHT_SAFE_MAX_PRESENT_HZ:-}"
readonly _SUNBRIGHT_SAFE_RADV_DEBUG_CAPTURE="${SUNBRIGHT_SAFE_RADV_DEBUG-}"
readonly _SUNBRIGHT_SAFE_J3D_CAPTURE="${SUNBRIGHT_SAFE_J3D_CAPTURE:-}"
readonly _SUNBRIGHT_SAFE_TEX_CAPTURE="${SUNBRIGHT_SAFE_TEX:-}"
readonly _SUNBRIGHT_SAFE_FASTBOOT_CAPTURE="${SUNBRIGHT_SAFE_FASTBOOT:-}"
readonly _SUNBRIGHT_SAFE_STAGE_CAPTURE="${SUNBRIGHT_SAFE_STAGE:-}"
readonly _SUNBRIGHT_SAFE_SCENARIO_CAPTURE="${SUNBRIGHT_SAFE_SCENARIO:-}"
readonly _SUNBRIGHT_SAFE_AB_HANDOFF_CAPTURE="${SUNBRIGHT_SAFE_AB_HANDOFF:-0}"
readonly _SUNBRIGHT_SAFE_AB_SET_CAPTURE="${SUNBRIGHT_SAFE_AB_SET:-0}"
readonly _SUNBRIGHT_SAFE_AB_CAPTURE="${SUNBRIGHT_SAFE_AB-}"
readonly _SUNBRIGHT_SAFE_AB_EVERY_SET_CAPTURE="${SUNBRIGHT_SAFE_AB_EVERY_SET:-0}"
readonly _SUNBRIGHT_SAFE_AB_EVERY_CAPTURE="${SUNBRIGHT_SAFE_AB_EVERY-}"
readonly _SUNBRIGHT_SAFE_AB_SELFTEST_SET_CAPTURE="${SUNBRIGHT_SAFE_AB_SELFTEST_SET:-0}"
readonly _SUNBRIGHT_SAFE_AB_SELFTEST_CAPTURE="${SUNBRIGHT_SAFE_AB_SELFTEST-}"
readonly _SUNBRIGHT_SAFE_ABLATE_SET_CAPTURE="${SUNBRIGHT_SAFE_ABLATE_SET:-0}"
readonly _SUNBRIGHT_SAFE_ABLATE_CAPTURE="${SUNBRIGHT_SAFE_ABLATE-}"
readonly _SUNBRIGHT_SAFE_AB_AT_SET_CAPTURE="${SUNBRIGHT_SAFE_AB_AT_SET:-0}"
readonly _SUNBRIGHT_SAFE_AB_AT_CAPTURE="${SUNBRIGHT_SAFE_AB_AT-}"
readonly _SUNBRIGHT_SAFE_EXPLICIT_ENV_FILE_CAPTURE="${SUNBRIGHT_SAFE_EXPLICIT_ENV_FILE:-}"
readonly _SUNBRIGHT_SAFE_ISOLATED_CAPTURE="${SUNBRIGHT_SAFE_ISOLATED:-0}"

restore_protected_value() {
    local target="$1"
    local is_set="$2"
    local value="$3"
    if [ "$is_set" = "1" ]; then
        printf -v "$target" '%s' "$value"
        export "$target"
    else
        unset "$target"
    fi
}

restore_protected_ab_environment() {
    # These are the complete A/B allowlist. Do not generalize this to arbitrary SBR_* values: some
    # renderer diagnostics materially change GPU workload and must not cross the safe boundary
    # without their own validation and explicit addition here.
    restore_protected_value SBR_AB "$_SUNBRIGHT_SAFE_AB_SET_CAPTURE" "$_SUNBRIGHT_SAFE_AB_CAPTURE"
    restore_protected_value SBR_AB_EVERY \
        "$_SUNBRIGHT_SAFE_AB_EVERY_SET_CAPTURE" "$_SUNBRIGHT_SAFE_AB_EVERY_CAPTURE"
    restore_protected_value SBR_AB_SELFTEST \
        "$_SUNBRIGHT_SAFE_AB_SELFTEST_SET_CAPTURE" "$_SUNBRIGHT_SAFE_AB_SELFTEST_CAPTURE"
    restore_protected_value SBR_ABLATE \
        "$_SUNBRIGHT_SAFE_ABLATE_SET_CAPTURE" "$_SUNBRIGHT_SAFE_ABLATE_CAPTURE"
    restore_protected_value SBR_AB_AT \
        "$_SUNBRIGHT_SAFE_AB_AT_SET_CAPTURE" "$_SUNBRIGHT_SAFE_AB_AT_CAPTURE"
}

[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
if [ "$_SUNBRIGHT_SAFE_RUN_CAPTURE" = "1" ]; then
    if [ "$_SUNBRIGHT_SAFE_ISOLATED_CAPTURE" = "1" ]; then
        # Measurement runs must not inherit a forgotten ablation, renderer diagnostic, or pacing
        # knob from either the caller or .env. Enumerate only project-owned prefixes; compiler,
        # desktop, asset-location, and system variables remain untouched. Explicit launcher
        # assignments are sourced below after this clearing pass.
        while IFS= read -r name; do
            case "$name" in
                SB_*|SBR_*|AURORA_*|RADV_*|LUCENT_*) unset "$name" ;;
            esac
        done < <(compgen -A variable)
    fi
    export SBR_RENDERER="$_SUNBRIGHT_SAFE_RENDERER_CAPTURE"
    export RADV_DEBUG="$_SUNBRIGHT_SAFE_RADV_DEBUG_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_HEADLESS_CAPTURE" ] || export SB_HEADLESS="$_SUNBRIGHT_SAFE_HEADLESS_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_MUTE_CAPTURE" ] || export SBR_MUTE="$_SUNBRIGHT_SAFE_MUTE_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_HZ_CAPTURE" ] || export SB_MAX_PRESENT_HZ="$_SUNBRIGHT_SAFE_HZ_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_J3D_CAPTURE" ] || export SBR_J3D_CAPTURE="$_SUNBRIGHT_SAFE_J3D_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_TEX_CAPTURE" ] || export SBR_TEX="$_SUNBRIGHT_SAFE_TEX_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_FASTBOOT_CAPTURE" ] || export SBR_FASTBOOT="$_SUNBRIGHT_SAFE_FASTBOOT_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_STAGE_CAPTURE" ] || export SBR_STAGE="$_SUNBRIGHT_SAFE_STAGE_CAPTURE"
    [ -z "$_SUNBRIGHT_SAFE_SCENARIO_CAPTURE" ] || export SBR_SCENARIO="$_SUNBRIGHT_SAFE_SCENARIO_CAPTURE"
    if [ -n "$_SUNBRIGHT_SAFE_EXPLICIT_ENV_FILE_CAPTURE" ]; then
        if [ ! -f "$_SUNBRIGHT_SAFE_EXPLICIT_ENV_FILE_CAPTURE" ]; then
            echo "[run-recomp] protected launcher environment is missing" >&2
            exit 1
        fi
        # The guarded Python launcher wrote this mode-0600 file from already validated NAME=VALUE
        # arguments. Reapply it AFTER .env so an explicit diagnostic/capture contract cannot be
        # silently replaced by a developer's persistent interactive settings.
        . "$_SUNBRIGHT_SAFE_EXPLICIT_ENV_FILE_CAPTURE"
    fi
    # run-render's validated A/B contract is final. Other safe launchers do not set this marker and
    # retain their existing explicit-environment behavior.
    if [ "$_SUNBRIGHT_SAFE_AB_HANDOFF_CAPTURE" = "1" ]; then
        restore_protected_ab_environment
    fi
fi

if [ "${1:-}" = "--selftest-safe-ab-handoff" ]; then
    if [ "$_SUNBRIGHT_SAFE_RUN_CAPTURE" != "1" ] ||
        [ "$_SUNBRIGHT_SAFE_AB_HANDOFF_CAPTURE" != "1" ]; then
        echo "[run-recomp] safe A/B handoff selftest requires run-render handoff" >&2
        exit 1
    fi
    # Simulate a hostile .env layer after capture, prove that it changed every field, then reapply
    # the same shipping handoff used above. This is the negative control: without the restoration,
    # all five assertions below would observe these planted values.
    export SBR_AB="hostile"
    export SBR_AB_EVERY="hostile"
    export SBR_AB_SELFTEST="hostile"
    export SBR_ABLATE="hostile"
    export SBR_AB_AT="hostile"
    restore_protected_ab_environment
    for protected in AB AB_EVERY AB_SELFTEST ABLATE AB_AT; do
        set_name="_SUNBRIGHT_SAFE_${protected}_SET_CAPTURE"
        value_name="_SUNBRIGHT_SAFE_${protected}_CAPTURE"
        target="SBR_${protected}"
        if [ "${!set_name}" = "1" ]; then
            [ "${!target+x}" = "x" ] && [ "${!target}" = "${!value_name}" ] || {
                echo "[run-recomp] safe A/B handoff selftest failed for $target" >&2
                exit 1
            }
        elif [ "${!target+x}" = "x" ]; then
            echo "[run-recomp] safe A/B handoff selftest failed to unset $target" >&2
            exit 1
        fi
    done
    echo "[run-recomp] safe A/B handoff selftest PASS"
    exit 0
fi
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

. "$HERE/tools/launch/sdl_video.sh"
configure_sunbright_sdl_video

BIN="$HERE/build-sms-recomp/sms-recomp"
ROM="${1:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"
[[ -f "$ROM" ]] || { echo "[run-recomp] ROM not found: $ROM (set SUNBRIGHT_ROM or drop rom.rvz)" >&2; exit 1; }

# The recompiled code needs the DOL it was generated from: its data sections are loaded
# at runtime. Not in the repo (it is game code); it lives beside the other scratch data.
DOL="${2:-${SUNBRIGHT_DOL:-$HERE/scratch/bin/sms.dol}}"
if [[ ! -f "$DOL" ]]; then
    echo "[run-recomp] DOL not found: $DOL" >&2
    echo "[run-recomp] extract the executable from the disc image (its main.dol) and put it" >&2
    echo "[run-recomp] there, or pass a path: ./run-recomp.sh \"$ROM\" /path/to/sms.dol" >&2
    exit 1
fi

# The launcher is a shipping interface: never run a stale executable merely because one exists.
# Configure once, then ask the build system to prove the target is current on every launch; an
# up-to-date incremental build is cheap and a changed source file can no longer be ignored.
# Debug is intentionally optimized by cmake/SunbrightBuildPolicy.cmake. It retains assertions,
# symbols, Dawn validation/robustness, and GPU labels without making the game an -O0 workload.
# Reconfigure every launch so an older Release cache cannot silently keep those diagnostics off.
echo "[run-recomp] configuring optimized Debug with clang++ ..." >&2
cmake -S "$HERE/sms-recomp" -B "$HERE/build-sms-recomp" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER=clang++ >&2
echo "[run-recomp] ensuring sms-recomp is current ..." >&2
cmake --build "$HERE/build-sms-recomp" --target sms-recomp -j"$NCPU" >&2

# A WINDOWLESS RUN MUST ALSO BE SILENT. SB_HEADLESS=1 suppressed the window but not the audio
# device, so every automated/diagnostic run — which is every run that sets it — played the game
# out of the speakers of whoever happened to be at the machine. Headless means "this run is not
# for a human to watch", and that has to include "or listen to".
#
# Overridable: SBR_MUTE=0 with SB_HEADLESS=1 still gives audio, which is what an audio-path test
# wants. This only supplies the default. The mute itself is honoured in the AI device and keeps
# the whole audio path running (mixer, pacing, silence counters) — it silences the output, it does
# not disable the subsystem being measured.
if [ "${SB_HEADLESS:-0}" != "0" ]; then
    export SBR_MUTE="${SBR_MUTE:-1}"
fi

export SUNBRIGHT_ROM="$ROM"
echo "[run-recomp] sms-recomp  \"$ROM\"  \"$DOL\""
exec "$BIN" "$DOL"

#!/usr/bin/env bash
# run-safe.sh — run the game with conservative GPU settings, and REPORT whether the run disturbed
# the GPU.
#
# WHY THIS EXISTS. On 2026-08-12 this project made the machine unusable twice. The first cause was
# the native renderer's own submissions; the second, and the one that applied to EVERY automated
# run, was that SB_TURBO removed the only bound on how fast frames reached the driver. Both are
# fixed in the code, but "fixed" was claimed before and the evidence for it was a log line from the
# process that was doing the damage.
#
# So this wrapper does two things the plain scripts do not:
#
#   1. It picks settings that cannot saturate the card — a 60 Hz present ceiling, headless, no
#      native renderer, a hard wall-clock cap — instead of leaving them to whoever writes the
#      command line. Every crash in that session came from a hand-assembled command.
#
#   2. It reads the KERNEL's opinion of the run, before and after. amdgpu logs ring timeouts and
#      resets whether or not our process notices, so counting those lines across the run is an
#      external check that does not depend on the thing being checked. A run that trips one says
#      so, loudly, and exits non-zero even if the game itself exited cleanly.
#
# It cannot pass vacuously: if the kernel log cannot be read, that is reported as UNKNOWN rather
# than clean, because "nothing happened" and "I could not look" must not share an output.
#
# Usage:  ./run-safe.sh [NAME=VALUE ...] [-- args to the runner]
#   ./run-safe.sh SBR_STAGE=1 SBR_QUIT_AFTER=400
#   ./run-safe.sh SBR_LERP60=1 SB_MAX_PRESENT_HZ=30 SBR_QUIT_AFTER=600
#   SB_RUNNER=run-decomp.sh ./run-safe.sh SB_STAGE=1 # the decomp runtime, same protections
#
# SB_RUNNER picks which runtime to launch (default run-recomp.sh). It exists because the decomp
# runtime had NO safe launcher at all, so verifying an upstream convergence at runtime — which a
# green build does not do, since a converged file can compile and still have dropped a native
# LP64/BE fix — meant hand-assembling a command line, and every run that made this machine
# unusable was hand-assembled.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Conservative by default, overridable by an explicit assignment on the command line.
export SB_HEADLESS="${SB_HEADLESS:-1}"
export SB_TURBO="${SB_TURBO:-1}"
# 60, not the runtime's default 120: this wrapper is for runs whose point is to be SAFE, and the
# game's own frame rate is 30, so a 60 Hz ceiling costs nothing that matters and halves the load.
export SB_MAX_PRESENT_HZ="${SB_MAX_PRESENT_HZ:-60}"
export SBR_MUTE="${SBR_MUTE:-1}"
export SBR_FASTBOOT="${SBR_FASTBOOT:-1}"
# SBR_SCENARIO=0 is NOT optional with fastboot, and omitting it fails in the worst possible way.
# Plain SBR_FASTBOOT=1 SBR_STAGE=1 derives a non-rendering episode from the save, so the stage
# loads, the run exits cleanly, and every interpolation seam reports "0 draws — either nothing
# drew or the hook never fired". That reads exactly like a broken hook, and on 2026-08-12 it cost
# three runs and a false "the pass-4 site discriminator never matched" conclusion before the cause
# was the episode. run-render.sh has always set it; this wrapper did not.
export SBR_SCENARIO="${SBR_SCENARIO:-0}"
export SBR_QUIT_AFTER="${SBR_QUIT_AFTER:-400}"

ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --) shift; ARGS=("$@"); break ;;
        *=*) export "${1?}" ;;
        *) ARGS+=("$1") ;;
    esac
    shift
done

# This wrapper owns the Aurora lane. Force the required renderer after parsing assignments so a
# persisted Native setting or command-line override cannot turn a "safe" run into a native run.
if [ "${SBR_RENDERER:-aurora}" != "aurora" ]; then
    echo "[run-safe] native renderer input was set. This wrapper runs the Aurora lane; use" >&2
    echo "           ./run-render.sh for the gated native preview. Forcing SBR_RENDERER=aurora." >&2
fi
export SBR_RENDERER=aurora

gpu_events() {
    # Snapshot the boot-wide total. This machine's wall clock stepped backward during the current
    # boot, so journalctl time windows return either stale history or nothing. Before/after totals
    # remain comparable because only newly appended kernel lines can increase the count.
    if ! command -v journalctl >/dev/null 2>&1; then echo UNKNOWN; return; fi
    local out
    if ! out=$(journalctl -k --no-pager 2>/dev/null); then echo UNKNOWN; return; fi
    printf '%s\n' "$out" | awk '
        /amdgpu/ && ($0 ~ /ring .* timeout/ || $0 ~ /ring reset/ ||
                     $0 ~ /device wedged/ || $0 ~ /GPUVM fault/) { count++ }
        END { print count + 0 }
    '
}

gpu_verdict() {
if [ "$AFTER" = "UNKNOWN" ] || [ "$BEFORE" = "UNKNOWN" ]; then
    echo "[run-safe] GPU HEALTH UNKNOWN — the kernel log could not be read, so this run is NOT" >&2
    echo "           certified clean. That is a different result from 'no events'." >&2
    exit 2
fi
if [ "$AFTER" -gt "$BEFORE" ]; then
    echo "[run-safe] *** THIS RUN DISTURBED THE GPU: $((AFTER - BEFORE)) new amdgpu" >&2
    echo "           timeout/reset line(s) (before=${BEFORE}, after=${AFTER}). ***" >&2
    echo "           Stop running. The first device loss ends GPU work for the session — see" >&2
    echo "           debug_journal/2026-08-12_gpu_hang_guards.md. Offending lines:" >&2
    journalctl -k --no-pager 2>/dev/null \
        | awk '/amdgpu/ && ($0 ~ /ring .* timeout/ || $0 ~ /ring reset/ ||
                            $0 ~ /device wedged/ || $0 ~ /GPUVM fault/ || $0 ~ /Process /)' \
        | tail -40 >&2
    exit 3
fi

echo "[run-safe] GPU clean: no NEW ring timeout, reset or fault during this run (delta 0; the"
echo "[run-safe] unfiltered boot total is ${AFTER} — time-filtered queries are unreliable here)."
}


BEFORE="$(gpu_events)"
if [ "$SB_HEADLESS" = "1" ]; then
    DISPLAY_MODE="headless"
else
    DISPLAY_MODE="windowed"
fi
echo "[run-safe] present ceiling ${SB_MAX_PRESENT_HZ} Hz, ${DISPLAY_MODE}, no native renderer, cap ${SBR_QUIT_AFTER} presents."
echo "[run-safe] amdgpu timeout/reset lines in boot log before this run: ${BEFORE}"

SECS="${SB_RUN_SECS:-240}"
set +e
RUNNER="${SB_RUNNER:-run-recomp.sh}"
if [ ! -x "$HERE/$RUNNER" ]; then
    echo "[run-safe] SB_RUNNER=$RUNNER is not an executable script in $HERE. Refusing to run" >&2
    echo "           anything rather than silently falling back to a different runtime." >&2
    exit 4
fi
echo "[run-safe] runner: $RUNNER"

# When a frame is being dumped, attach the texture mip decisions to it. Issue #5 is an
# intermittent frame that differs from the usual one only in texture SHARPNESS, and it resists
# reproduction — 7 consecutive instrumented runs failed to produce it. Provoking a rare state on
# demand is the wrong approach; recording enough alongside every dump that whichever run finally
# hits it is diagnosable after the fact is the right one. The channel emits ~131 lines a run, so
# this costs nothing, and it is scoped to dumping runs so ordinary runs are untouched.
if [ -n "${SB_DUMP_FRAME:-}" ]; then
    case ",${LUCENT_DEBUG:-}," in
        *,texresolve,*|*,all,*) ;;
        *) export LUCENT_DEBUG="${LUCENT_DEBUG:+$LUCENT_DEBUG,}texresolve" ;;
    esac
    MANIFEST="${SB_DUMP_FRAME}.textures.txt"
    timeout -s KILL "$SECS" "$HERE/$RUNNER" "${ARGS[@]}" 2>&1 | tee "$HERE/scratch/.run-safe-out.$$"
    RC=${PIPESTATUS[0]}
    grep -a "^\[texresolve\] static " "$HERE/scratch/.run-safe-out.$$" > "$MANIFEST" 2>/dev/null || true
    python3 "$HERE/tools/scratch_clean.py" --glob ".run-safe-out.$$" "$HERE/scratch" >/dev/null
    if [ -s "$MANIFEST" ]; then
        echo "[run-safe] texture manifest beside the dump: $MANIFEST ($(wc -l < "$MANIFEST") texture(s), \
$(grep -c 'mips=1 ' "$MANIFEST" || true) single-level)"
    else
        echo "[run-safe] NO texture manifest was captured, so this dump is NOT comparable against" >&2
        echo "           another one for issue #5. That is a broken capture, not a clean run." >&2
    fi
    set -e
    AFTER="$(gpu_events)"
    echo "[run-safe] game exit=${RC}; amdgpu timeout/reset lines now in boot log: ${AFTER} (before=${BEFORE})"
    gpu_verdict
    exit "$RC"
fi
timeout -s KILL "$SECS" "$HERE/$RUNNER" "${ARGS[@]}"
RC=$?
set -e

AFTER="$(gpu_events)"
echo "[run-safe] game exit=${RC}; amdgpu timeout/reset lines now in boot log: ${AFTER} (before=${BEFORE})"

gpu_verdict
exit "$RC"

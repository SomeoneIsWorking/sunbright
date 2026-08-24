#!/usr/bin/env bash
# play.sh — PLAY Super Mario Sunshine on this port. ./run.sh delegates here so the default path
# and this explicit product launcher cannot drift:
#
#   ./run.sh           boot the game immediately             <-- default
#   ./play.sh          the same product with command-line convenience flags
#   ./run-recomp.sh    the same runtime, raw: no defaults, no argument parsing, no controls printed
#   ./run-decomp.sh    the decomp verification oracle
#   ./run-render.sh    the in-progress native SDL3-GPU renderer, behind six env vars
#
# WHAT RUNS: the game's real PowerPC code, statically recompiled, on native device models plus
# Aurora (SDL3 + WebGPU/Dawn). The whole game runs — not a subset of hand-ported actors.
#
# WHAT DOES NOT: attract movies and most cutscenes (the plaza's own video does decode).
#
# AUDIO WORKS as of 2026-08-07 — music and sound effects. The mixing the GameCube DSP used to do is
# done natively (sms-recomp/runtime/devices/dsp_mixer.cpp); v1 renders the main L/R buses, so the
# aux/reverb sends, the IIR/FIR filters and the Dolby positional mix are absent and the output is
# CENTRE-PANNED. Streamed audio (DTK / movie soundtracks) is a separate path and still silent.
#
# ── USAGE ──────────────────────────────────────────────────────────────────────────────────────
#   ./play.sh                        boot normally: GC logo -> title -> file select
#   ./play.sh --60fps                interpolated 60fps (experimental; measures better than 30 — see below)
#   ./play.sh --fastboot             skip the menus: File 1 -> Delfino Plaza
#   ./play.sh --stage 6              boot straight into a stage (--scenario N picks the episode)
#   ./play.sh --size 1920x1080       window size (default 1280x960)
#   ./play.sh --rom /path/game.rvz   ROM path, if not in .env / $SUNBRIGHT_ROM / ./rom.rvz
#   ./play.sh --help
#
# RmlUi opens only when Escape is pressed during play and persists Renderer + Framerate in the
# platform user-data directory. Command-line options and environment variables override the
# persisted value for that session.
#
# Anything after `--` is passed through as environment, e.g.
#   ./play.sh --fastboot -- SBR_LUCENT_DEBUG=card
#
# ── THE 60fps NOTE, because it is not ready and should not be discovered by surprise ───────────
# `--60fps` presents an interpolated frame between each pair of the game's own 30Hz frames. The
# game logic still runs at 30Hz, exactly as on console — nothing about physics or timing changes.
#
# It is honest to call it experimental, and honest to say it MEASURES BETTER THAN 30fps: judder
# (how evenly consecutive presents advance the game — tools/interp/cadence.py) is 1.10 against the
# uninterpolated 1.18, at matched guest ticks with the camera rotating.
#
# WHAT INTERPOLATES, measured per population (SBR_LUCENT_DEBUG=interp prints this live):
#   ship/pass-4 shadows 99.8%  ·  world geometry 97.3%  ·  shine shadow 95.4%  ·  particles 95.3%
#   ·  shadow volume 94.8%  ·  2D/HUD correctly snaps
# Still snapping: flags and the sea ripple grid — they DEFORM per tick, so only their vertices carry
# the motion and no matrix can express it — and the shadow alpha cube. Those figures count every
# camera-only draw as a shortfall, which OVERSTATES it: for static geometry the camera delta alone is
# correct. docs/60fps/README.md has the table and a reason for every row.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

FPS60=0 FASTBOOT=0 STAGE="" SCENARIO="" SIZE="" ROM="" PASSTHRU=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --60fps|--fps60) FPS60=1; shift ;;
        --fastboot)      FASTBOOT=1; shift ;;
        --stage)         STAGE="${2:?--stage needs a number}"; shift 2 ;;
        --scenario)      SCENARIO="${2:?--scenario needs a number}"; shift 2 ;;
        --size)          SIZE="${2:?--size needs WxH}"; shift 2 ;;
        --rom)           ROM="${2:?--rom needs a path}"; shift 2 ;;
        -h|--help)       sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --)              shift; PASSTHRU=("$@"); break ;;
        *)               echo "play.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
done

# The shipping/default target is recomp + Aurora. Renderer choice is not an optional preference:
# Native owns a different GPU device and swapchain and remains behind run-render.sh until that path
# has its own complete UI and shutdown lifecycle. A stale persisted setting must not silently move
# ./run.sh onto the development renderer.
ENV=("SBR_RENDERER=aurora")

if [[ -n "$SIZE" ]]; then
    # Refuse a malformed size rather than silently ignoring it and opening a default window, which
    # looks exactly like the flag not existing.
    [[ "$SIZE" =~ ^([0-9]+)x([0-9]+)$ ]] || {
        echo "play.sh: --size wants WIDTHxHEIGHT, e.g. 1920x1080 (got '$SIZE')" >&2; exit 2; }
    ENV+=("SB_W=${BASH_REMATCH[1]}" "SB_H=${BASH_REMATCH[2]}")
fi

[[ "$FASTBOOT" == 1 ]] && ENV+=("SBR_FASTBOOT=1")
[[ -n "$STAGE"    ]] && ENV+=("SBR_STAGE=$STAGE")
[[ -n "$SCENARIO" ]] && ENV+=("SBR_SCENARIO=$SCENARIO")

if [[ "$FPS60" == 1 ]]; then
    # WHICH of the three 60fps paths (docs/60fps/README.md), and why this one — MEASURED, not chosen.
    #
    # tools/interp/cadence.py scores the thing a player actually reports. It takes the difference
    # between each pair of CONSECUTIVE presents and asks whether those steps are the same size:
    # judder = max(step)/min(step), 1.0 = every present advances the game equally. Same scenario,
    # same pad script, MATCHED GUEST TICKS (~4802-4818, camera rotating):
    #
    #   no interpolation, plain 30fps ............ judder 1.18   mean step 7.29
    #   A  stream interpolation (SBR_60FPS) ...... judder 1.10   mean step 5.13   <- this
    #   C  record-and-replace (SBR_INTERP60_*) ... judder 2.33   mean step 6.02
    #
    # C is TWICE AS JUDDERY AS NOT INTERPOLATING AT ALL. It lerps each J3DModel's draw matrices and
    # nothing else, so the HUD, particles, immediate-mode geometry, the dash-trail EFB feedback and
    # every screen-sampling effect step at 30Hz inside a 60Hz frame — and its sub-frame re-issues
    # draw lists that were never meant to run twice. A rewrites the RECORDED frame's matrices and
    # presents the packet again, running no game code in the sub-frame at all, which is why its
    # effects work and why it beats even the 30fps baseline.
    #
    # The cadence is REGULAR — from the runtime's own counters, one in-between frame per simulation
    # tick exactly. An earlier note here claimed 2-3 per tick; that came from grouping dumps by their
    # `-t<n>` filename label, which is the GAME's retrace counter and advances by however many fields
    # the game asked for, so consecutive ticks can share one label. The numbers were right and the
    # verdict was wrong.
    #
    # Interpolated runs also select a QUEUED present mode (strict Fifo). vsync=false gives Mailbox,
    # which DISCARDS a pending image when a newer one arrives before the display samples it — so a
    # tick emitting two images inside one refresh had its in-between frame thrown away by the
    # swapchain while every counter still read 60fps.
    ENV+=("SBR_60FPS=1")
fi

cat <<'CONTROLS'
────────────────────────────────────────────────────────────────────────────────
 Sunbright — Super Mario Sunshine

 Keyboard            Move WASD  ·  Camera IJKL  ·  A Space  ·  B LCtrl
                     X E  ·  Y Q  ·  Z C  ·  L F  ·  R (FLUDD) LShift
                     START Enter  ·  D-pad arrows
 Gamepad             plugged in and mapped as a GameCube pad; just use it.

 Quit                close the window, or Ctrl-C
 Audio               music + SFX. Centre-panned: aux/reverb/filters/Dolby are not
                     rendered yet, and streamed (movie) audio is still silent.
────────────────────────────────────────────────────────────────────────────────
CONTROLS

if [[ "$FPS60" == 1 ]]; then
    echo " 60fps interpolation: ON — game logic still runs at 30Hz, as on console."
    echo "                          Interpolating: shadows 94.8-99.8%, world 97.3%, particles 95.3%."
    echo "                          Still snapping: flags and the sea ripple grid (they deform, so"
    echo "                          only their vertices carry the motion) — docs/60fps/README.md."
    echo "────────────────────────────────────────────────────────────────────────────────"
fi
echo

ARGS=()
[[ -n "$ROM" ]] && ARGS=("$ROM")
exec env "${ENV[@]}" "${PASSTHRU[@]}" "$HERE/run-recomp.sh" "${ARGS[@]}"

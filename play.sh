#!/usr/bin/env bash
# play.sh — PLAY Super Mario Sunshine on this port. This is the entry point; the other three
# scripts in the repo root are development harnesses, not ways to play:
#
#   ./play.sh          the game                     <-- you want this one
#   ./run-recomp.sh    the same runtime, raw: no defaults, no argument parsing, no controls printed
#   ./run.sh           the DECOMP runtime — a second, hand-ported runtime that reaches the title,
#                      file-select and Delfino Plaza only. It is the verification oracle, not the
#                      playable build.
#   ./run-render.sh    the in-progress native SDL3-GPU renderer, behind six env vars
#
# WHAT RUNS: the game's real PowerPC code, statically recompiled, on native device models plus
# Aurora (SDL3 + WebGPU/Dawn). The whole game runs — not a subset of hand-ported actors.
#
# WHAT DOES NOT: attract movies and most cutscenes (the plaza's own video does decode), and the
# game is SILENT — the JAS DSP mixer is not ported yet, which is a known named gap, not a fault of
# your setup.
#
# ── USAGE ──────────────────────────────────────────────────────────────────────────────────────
#   ./play.sh                        boot normally: GC logo -> title -> file select
#   ./play.sh --60fps                interpolated 60fps (EXPERIMENTAL — read the note below)
#   ./play.sh --fastboot             skip the menus: File 1 -> Delfino Plaza
#   ./play.sh --stage 6              boot straight into a stage (--scenario N picks the episode)
#   ./play.sh --size 1920x1080       window size (default 1280x960)
#   ./play.sh --rom /path/game.rvz   ROM path, if not in .env / $SUNBRIGHT_ROM / ./rom.rvz
#   ./play.sh --help
#
# Anything after `--` is passed through as environment, e.g.
#   ./play.sh --fastboot -- SBR_LUCENT_DEBUG=card
#
# ── THE 60fps NOTE, because it is not ready and should not be discovered by surprise ───────────
# `--60fps` presents an interpolated frame between each pair of the game's own 30Hz frames. The
# game logic still runs at 30Hz, exactly as on console — nothing about physics or timing changes.
#
# It is honest to call it experimental. Measured 2026-08-06 with the camera swinging 65 units/tick
# (debug_journal/2026-08-06_motion_census_and_uncovered_residual.md): the interpolated frame
# responds correctly to time for the sky, and only partially for the ground, sea and buildings —
# at the midpoint it sits further from BOTH of its neighbours than they are from each other. In
# motion that reads as a slight shimmer on near geometry, and the 2D HUD steps at 30Hz regardless.
# It is smoother than 30fps and it is not yet correct. Off by default for that reason.
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

ENV=()

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
    # THE WHOLE SET, TOGETHER. Omit any one of these and the run still succeeds, still renders and
    # still looks plausible — it simply shows each 30Hz frame twice, which is indistinguishable
    # from working interpolation unless you are measuring. That failure mode has cost this project
    # entire sessions (see tools/interp/interp60_run.sh), so the switches live here as a set and
    # never in a user's shell history one at a time.
    #
    #   SBR_INTERP60            run the sub-frame at all
    #   SBR_INTERP60_REPLACE    use RECORD-AND-REPLACE (dusklight's model: the sim tick runs
    #                           untouched, final matrices are recorded, the presentation frame
    #                           lerps prev->cur). The older substitute-and-re-issue path leaks
    #                           into the game's own state; this one cannot, by construction.
    #   SBR_INTERP60_ALPHA      0.5 — the sub-frame is presented at the midpoint of a tick, and
    #                           exactly one is presented per tick, so the midpoint IS its time.
    #   SBR_INTERP60_COPY       the sub-frame copies its own EFB out to the XFB ...
    #   SBR_PRESENT_AFTER_COPY  ... and the present happens after that copy, not before it.
    ENV+=("SBR_INTERP60=1" "SBR_INTERP60_REPLACE=1" "SBR_INTERP60_ALPHA=0.5"
          "SBR_INTERP60_COPY=1" "SBR_PRESENT_AFTER_COPY=1")
fi

cat <<'CONTROLS'
────────────────────────────────────────────────────────────────────────────────
 Sunbright — Super Mario Sunshine

 Keyboard            Move WASD  ·  Camera IJKL  ·  A Space  ·  B LCtrl
                     X E  ·  Y Q  ·  Z C  ·  L F  ·  R (FLUDD) LShift
                     START Enter  ·  D-pad arrows
 Gamepad             plugged in and mapped as a GameCube pad; just use it.

 Quit                close the window, or Ctrl-C
 Audio               SILENT — the JAS mixer is not ported yet (known gap)
────────────────────────────────────────────────────────────────────────────────
CONTROLS

if [[ "$FPS60" == 1 ]]; then
    echo " 60fps interpolation: ON (experimental — game logic still runs at 30Hz;"
    echo "                          near geometry shimmers slightly, the HUD steps at 30Hz)"
    echo "────────────────────────────────────────────────────────────────────────────────"
fi
echo

ARGS=()
[[ -n "$ROM" ]] && ARGS=("$ROM")
exec env "${ENV[@]}" "${PASSTHRU[@]}" "$HERE/run-recomp.sh" "${ARGS[@]}"

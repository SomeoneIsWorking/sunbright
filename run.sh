#!/usr/bin/env bash
# Launch the Super Mario Sunshine PC port with a video setup that works on this machine.
#
# DEFAULT (2026-06-30): runs **sms-boot** — the PC-NATIVE renderer (the reference/sms decomp
# compiled as the game, drawn through our own SDL3-GPU GX backend; NO Dolphin, NO ngx in the
# render path). A live window (SB_WINDOW) shows the frame so you can inspect it directly.
#
# The Dolphin-GX ORACLE (build/sunbright, pure Dolphin GX, the ground-truth reference) is still
# reachable for A/B comparison:   SUNBRIGHT_BIN=build/sunbright ./run.sh
# (the ngx native-capture renderer that used to be the default is being eradicated — it is broken
# and must NOT be used as a reference; the oracle is Dolphin-GX with SUNBRIGHT_NGX_PRESENT=0.)
#
# Usage:
#   ./run.sh                               # native sms-boot in a live window (Delfino plaza fastboot)
#   SB_STAGE=15 ./run.sh                   # boot a specific stage (15 = title / file-select scene)
#   SB_SCENARIO=0 ./run.sh                 # episode within the stage
#   SUNBRIGHT_DISC=/path/to/game.iso ./run.sh   # disc image (default scratch/disc/sms.iso)
#   SUNBRIGHT_BIN=build/sunbright ./run.sh  # the Dolphin-GX oracle build instead (its own env below)
#   (any SB_* / SUNBRIGHT_* env var is passed through)
#
# Keyboard: Esc or closing the window quits.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# ── Video path: XWayland/x11 (Dolphin & SDL3-GPU both make surfaces there; native Wayland can't). ──
if [[ "$(uname)" == "Darwin" ]]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-cocoa}"
else
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
    export DISPLAY="${DISPLAY:-:0}"
fi

# ── Oracle path: SUNBRIGHT_BIN=build/sunbright → the legacy Dolphin build (ROM argv + banner). ──
if [[ -n "$SUNBRIGHT_BIN" ]]; then
    case "$SUNBRIGHT_BIN" in /*) BIN="$SUNBRIGHT_BIN";; *) BIN="$HERE/$SUNBRIGHT_BIN";; esac
    export SUNBRIGHT_BACKEND="${SUNBRIGHT_BACKEND:-Vulkan}"
    # The oracle is pure Dolphin GX — NGX present OFF (ngx is broken / being removed).
    export SUNBRIGHT_NGX_PRESENT="${SUNBRIGHT_NGX_PRESENT:-0}"
    ROM="${1:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"
    [[ -x "$BIN" ]] || { echo "[run] $BIN not built" >&2; exit 1; }
    [[ -f "$ROM" ]] || { echo "[run] ROM not found: $ROM" >&2; exit 1; }
    echo "[run] ORACLE $BIN (Dolphin-GX, NGX_PRESENT=$SUNBRIGHT_NGX_PRESENT)  \"$ROM\""
    exec "$BIN" "$ROM" "${@:1}"
fi

# ── Default path: the native PC renderer (sms-boot). ──
BIN="$HERE/build-native/sms-boot"
if [[ ! -x "$BIN" ]] || [[ -n "$(find "$HERE/native" "$HERE/reference/sms/src" -newer "$BIN" -print -quit 2>/dev/null)" ]]; then
    echo "[run] building sms-boot ..." >&2
    cmake -B "$HERE/build-native" >/dev/null 2>&1 || true
    cmake --build "$HERE/build-native" -j"$NCPU" --target sms-boot >&2 \
        || { echo "[run] sms-boot build FAILED" >&2; exit 1; }
fi

# Disc image (no machine-specific path committed): SUNBRIGHT_DISC, else scratch/disc/sms.iso.
export SUNBRIGHT_DISC="${SUNBRIGHT_DISC:-$HERE/scratch/disc/sms.iso}"
[[ -f "$SUNBRIGHT_DISC" ]] || { echo "[run] disc image not found: $SUNBRIGHT_DISC (set SUNBRIGHT_DISC=...)" >&2; exit 1; }

export SB_SDLGPU="${SB_SDLGPU:-1}"   # SDL3-GPU GX backend (the only renderer)
export SB_WINDOW="${SB_WINDOW:-1}"   # live inspection window
export SB_THP_FAST="${SB_THP_FAST:-1}"
export SB_HOST_ALLOC_CAP_MB="${SB_HOST_ALLOC_CAP_MB:-3072}"
# Default boot target: stage 15 = the title/file-select scene (not Delfino Plaza fastboot). A
# default SB_PAD_SCRIPT pulses Start a few times in the real-time PRESS-START window (verified
# this session: reliably carries TCardLoad from mState 3 "press start" through to mState 0, the
# settled 3-block "Select data." picker). Override SB_STAGE=1 (or any other value) to go elsewhere;
# set SB_PAD_SCRIPT="" to boot to the title screen only, with no auto-press.
export SB_STAGE="${SB_STAGE:-15}"
export SB_SCENARIO="${SB_SCENARIO:-0}"
export SB_PAD_SCRIPT="${SB_PAD_SCRIPT-300:START 315:- 340:START 355:- 380:START 395:- 420:START 435:- 460:START 475:-}"
# NOTE: do NOT default SB_TURBO here — windowed inspection wants REAL-TIME pacing (~60fps), not
# uncapped. Turbo out-runs the display swapchain and floods the GPU present queue (the watchdog
# hang). Set SB_TURBO=1 explicitly only for headless throughput runs.

if [[ "$SB_WINDOW" != "0" ]]; then
  echo "[run] NOTE: live window (SB_WINDOW=1) is WIP — it currently HANGS at boot. The SDL-main +"
  echo "      single-game-thread model is wired, but the cooperative scheduler still assumes the game"
  echo "      runs on the process main thread (sb_sched_drain_until_idle/cur_self_locked). Fix = the"
  echo "      worker-threads->synchronous conversion (memory sms-boot-threading-architecture). To"
  echo "      inspect NOW: SB_WINDOW=0 SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=600 ... (PPMs in scratch/frames/)."
  echo "      If it hangs, the 8s watchdog dumps all threads. Ctrl-C / Esc to quit."
fi
echo "[run] NATIVE sms-boot (SDL3-GPU)  disc=$SUNBRIGHT_DISC  stage=${SB_STAGE:-<fastboot>}  SB_WINDOW=$SB_WINDOW"
exec "$BIN" "${@:1}"

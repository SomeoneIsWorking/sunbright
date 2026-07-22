#!/usr/bin/env bash
# Launch sms-recomp — Super Mario Sunshine's real PowerPC code, statically recompiled,
# running on native device models + Aurora (the second of the project's two runtimes;
# ./run.sh launches the decomp one).
#
# Usage:
#   ./run-recomp.sh                  # ROM via $SUNBRIGHT_ROM, .env, or rom.rvz drop-in
#   ./run-recomp.sh [rom.rvz] [sms.dol]
#   SB_W=1920 SB_H=1080 ./run-recomp.sh
#   SBR_LUCENT_DEBUG=card,gxfifo ./run-recomp.sh    # per-channel diagnostics
#   SB_TURBO=1 ./run-recomp.sh                     # unpaced (no frame limiting)
#
# WHAT TO EXPECT: boots GC logo -> title. Press START (Enter) at the title to reach
# file-select, which mounts your real Dolphin memory card and shows your saves. Title and
# file-select both render faithfully (verified per-region against the decomp oracle and
# against a retail Dolphin capture, 2026-07-22).
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
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Native Wayland can't create the Vulkan surface on this machine; XWayland works.
if [[ "$(uname)" != "Darwin" ]]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
    export DISPLAY="${DISPLAY:-:0}"
fi

BIN="$HERE/build-sms-recomp/sms-recomp"
if [[ ! -x "$BIN" ]]; then
    echo "[run-recomp] building sms-recomp ..." >&2
    cmake -S "$HERE/sms-recomp" -B "$HERE/build-sms-recomp" -DCMAKE_BUILD_TYPE=Release >&2
    cmake --build "$HERE/build-sms-recomp" --target sms-recomp -j"$NCPU" >&2
fi

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

export SUNBRIGHT_ROM="$ROM"
echo "[run-recomp] sms-recomp  \"$ROM\"  \"$DOL\""
exec "$BIN" "$DOL"

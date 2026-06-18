#!/usr/bin/env bash
# Launch Sunbright (statically-recompiled Super Mario Sunshine) with a video setup
# that actually works on this machine.
#
# Why this is needed: Dolphin's GL/Vulkan backends can't create a surface on a
# *native Wayland* window (you get "failed to initialize video backend"). Forcing
# SDL to the x11 driver routes the window through XWayland, where both OpenGL and
# Vulkan work. We pin SDL_VIDEODRIVER=x11 and default to the Vulkan backend.
#
# Usage:
#   ./run.sh [rom.rvz] [extra sunbright args...]
#   SUNBRIGHT_BACKEND=OGL ./run.sh            # use OpenGL instead of Vulkan
#   SUNBRIGHT_RES_SCALE=2 ./run.sh            # internal resolution (default 3× native)
#   SUNBRIGHT_WIDESCREEN=0 ./run.sh           # 4:3 instead of 16:9 widescreen
#   SUNBRIGHT_DUMP=1 ./run.sh                 # dump frames to <home>/.local/share/dolphin-emu/Dump/Frames
#   SUNBRIGHT_AUTOSTART=1 ./run.sh            # auto-press Start/A (headless demo)
#   SUNBRIGHT_NGX_PRESENT=0 ./run.sh          # Dolphin-GX baseline (disable the native renderer)
#   SUNBRIGHT_BIN=build/sunbright ./run.sh    # pick a specific binary
#   (any SUNBRIGHT_* debug var set in your env is passed through)
#
# Defaults: the NATIVE PC renderer (SUNBRIGHT_NGX_PRESENT) is on — the on-screen frame
# is drawn by our own Vulkan renderer reading the game's J3D objects out of guest RAM
# (no Dolphin GX in the render path), with the J2D/HUD composited on top. Vulkan only.
# 3× internal resolution, 16:9 widescreen. F11 toggles fullscreen.
#
# Keyboard → GameCube pad (window must be focused):
#   Enter=Start  Z=A(jump)  X=B  C=X  V=Y  Q=Z  A=L  S=R(spray)  arrows=control stick

set -eo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The binary is build/sunbright (the single self-contained build; the native renderer lives
# in runtime/ and is linked in). Override with SUNBRIGHT_BIN=<path> (relative to repo dir or
# absolute). NOTE: do NOT auto-prefer build-j3dvirt/ — that was the RETIRED flip-architecture
# build (docs/DO_NOT_REVISIT_FLIP.md); a stale binary left there silently shadowed every rebuild
# (the "still-shredded headed Mario" was that stale binary, while build/ rendered correctly).
if [[ -n "$SUNBRIGHT_BIN" ]]; then
    case "$SUNBRIGHT_BIN" in /*) BIN="$SUNBRIGHT_BIN";; *) BIN="$HERE/$SUNBRIGHT_BIN";; esac
else
    BIN="$HERE/build/sunbright"
fi
# ROM source (no machine-specific path in the tree): explicit arg, else $SUNBRIGHT_ROM (set it in a
# gitignored .env next to this script, or in your env), else a drop-in rom.rvz in the repo dir.
[ -f "$HERE/.env" ] && { set -a; . "$HERE/.env"; set +a; }
ROM="${1:-${SUNBRIGHT_ROM:-$HERE/rom.rvz}}"

# Portable core count (macOS has no nproc).
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Auto-build the default binary when it's missing OR stale (a source changed since the last build).
# Only for the default build/ — a custom SUNBRIGHT_BIN is the caller's responsibility.
if [[ "$BIN" == "$HERE/build/sunbright" ]]; then
    NEEDS_BUILD=0
    if [[ ! -x "$BIN" ]]; then
        NEEDS_BUILD=1
    elif [[ -n "$(find "$HERE/runtime" "$HERE/CMakeLists.txt" "$HERE/run.sh" -newer "$BIN" -print -quit 2>/dev/null)" ]]; then
        echo "[run] sources changed since the last build — rebuilding build/sunbright" >&2
        NEEDS_BUILD=1
    fi
    if [[ "$NEEDS_BUILD" == "1" ]]; then
        echo "[run] building build/sunbright (this can take a while the first time) ..." >&2
        if ! cmake -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON >&2; then
            # A stale CMake cache can't always be reconfigured in place — e.g. switching to the
            # bundled fmt/glslang (needed so our TUs and Dolphin agree on versions; see CMakeLists)
            # trips Dolphin's system-vs-bundled "selection changed" guard. Self-heal: move the old
            # build dir aside (kept as build.stale.* — no data lost) and configure fresh.
            STALE="$HERE/build.stale.$(date +%s)"
            echo "[run] configure failed (likely a stale cache) — moving build/ to $(basename "$STALE") and configuring fresh" >&2
            mv "$HERE/build" "$STALE" 2>/dev/null || true
            cmake -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON >&2 \
                || { echo "[run] cmake configure FAILED (even fresh)" >&2; exit 1; }
        fi
        cmake --build "$HERE/build" -j"$NCPU" --target sunbright >&2 \
            || { echo "[run] build FAILED" >&2; exit 1; }
    fi
fi

if [[ ! -x "$BIN" ]]; then
    echo "sunbright not built ($BIN). Build it with:" >&2
    echo "  cmake -B \"$HERE/build\" -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON && cmake --build \"$HERE/build\" -j$NCPU" >&2
    exit 1
fi
# Sanity: refuse a STALE / non-native binary. The PC-native engine (no static recompiler in the game)
# carries this banner string; the retired recomp build (build-j3dvirt/) does NOT. Running the stale
# one is the "I see the Dolphin game, not the native render" trap (memory runsh-stale-j3dvirt-binary).
if ! strings -n 16 "$BIN" 2>/dev/null | grep -q "static recompiler eradicated"; then
    echo "[run] REFUSING $BIN — it is NOT the PC-native engine build (no no-recomp banner; likely the" >&2
    echo "      retired recomp build-j3dvirt/ or a stale binary). Rebuild build/ and unset SUNBRIGHT_BIN:" >&2
    echo "      cmake --build \"$HERE/build\" -j$NCPU   # then: ./run.sh" >&2
    exit 1
fi
if [[ ! -f "$ROM" ]]; then
    echo "ROM not found: $ROM" >&2
    echo "Pass one explicitly:  ./run.sh /path/to/game.rvz" >&2
    exit 1
fi

# Pin the working video path (override by exporting these before calling).
# macOS uses the cocoa SDL driver + MoltenVK; Linux/XWayland uses x11.
if [[ "$(uname)" == "Darwin" ]]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-cocoa}"
    export SUNBRIGHT_BACKEND="${SUNBRIGHT_BACKEND:-Vulkan}"
    # Dolphin on macOS skips the system Vulkan loader and dlopen's libMoltenVK directly. Since we're
    # not an app bundle, point it at the (homebrew or Vulkan SDK) MoltenVK dylib.
    export LIBVULKAN_PATH="${LIBVULKAN_PATH:-/opt/homebrew/lib/libMoltenVK.dylib}"
else
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
    export SUNBRIGHT_BACKEND="${SUNBRIGHT_BACKEND:-Vulkan}"
    export DISPLAY="${DISPLAY:-:0}"
fi

# Native PC renderer (our own Vulkan renderer of the game's J3D scene) is the on-screen
# image by default. It is Vulkan-only, so force Vulkan when it's enabled.
export SUNBRIGHT_NGX_PRESENT="${SUNBRIGHT_NGX_PRESENT:-1}"
if [[ "$SUNBRIGHT_NGX_PRESENT" != "0" ]]; then
    export SUNBRIGHT_NGX_SHAPE="${SUNBRIGHT_NGX_SHAPE:-1}"   # NGX_PRESENT implies capture; be explicit
    if [[ "$SUNBRIGHT_BACKEND" != "Vulkan" ]]; then
        echo "[run] native renderer (SUNBRIGHT_NGX_PRESENT) is Vulkan-only — forcing Vulkan" >&2
        export SUNBRIGHT_BACKEND="Vulkan"
    fi
    RENDER="native (NGX present)"
else
    RENDER="Dolphin GX"
fi

echo "[run] SDL_VIDEODRIVER=$SDL_VIDEODRIVER  SUNBRIGHT_BACKEND=$SUNBRIGHT_BACKEND  render=$RENDER"
echo "[run] $BIN \"$ROM\""
exec "$BIN" "$ROM" "${@:2}"

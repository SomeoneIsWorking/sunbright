#!/usr/bin/env bash
# Build the last Dolphin-backed Sunbright runtime as a fastboot-capable GX oracle.
#
# The product runtime is sms-recomp.  This tool deliberately reconstructs the retired
# Dolphin-backed runtime under gitignored scratch/ from the last commit that contained it;
# no generated source or copyrighted game data is copied into the tracked tree.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOURCE_REV="${SBR_DOLPHIN_ORACLE_REV:-9283f44^}"
SRC="$ROOT/scratch/oracle/dolphin_fastboot/src"
BUILD="$ROOT/scratch/oracle/dolphin_fastboot/build"
FORK="$ROOT/extern/dolphin_fork"

[[ -f "$FORK/CMakeLists.txt" ]] || {
    echo "[dolphin-fastboot] missing initialized extern/dolphin_fork" >&2
    exit 1
}
git -C "$ROOT" cat-file -e "$SOURCE_REV^{commit}" 2>/dev/null || {
    echo "[dolphin-fastboot] source revision is unavailable: $SOURCE_REV" >&2
    exit 1
}

mkdir -p "$SRC" "$BUILD"
if [[ ! -f "$SRC/runtime/overrides/fastboot_native.cpp" ]]; then
    git -C "$ROOT" archive "$SOURCE_REV" | tar -x -C "$SRC"
fi

mkdir -p "$SRC/externals"
ln -sfn "$FORK" "$SRC/externals/dolphin"

# That revision's native/ directory was a submodule. Its contents are neither needed nor
# available for the `sunbright` Dolphin oracle target. Alter only the reconstructed scratch
# copy, and verify the unavailable edge is gone rather than silently configuring the wrong tree.
if rg -qx 'add_subdirectory\(native\)' "$SRC/CMakeLists.txt"; then
    sed -i '/^add_subdirectory(native)$/d' "$SRC/CMakeLists.txt"
fi
if rg -q 'add_subdirectory\(native\)' "$SRC/CMakeLists.txt"; then
    echo "[dolphin-fastboot] failed to remove historical native/ build edge" >&2
    exit 1
fi
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target sunbright -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "[dolphin-fastboot] built $BUILD/sunbright"

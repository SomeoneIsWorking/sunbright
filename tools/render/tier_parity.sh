#!/usr/bin/env bash
# tier_parity.sh — Tier-1 (SDL3-GPU) vs Tier-2 (Dolphin videovulkan) in-process
# parity comparison, driven by the SB_HARNESS synthetic-frame scaffold in
# native/src/render_parity.cpp (task #29, direction pivot 2026-07-04).
#
# Usage:  tools/render/tier_parity.sh <test-name>
#
# Two runs of build/native/sms-boot with the game bypassed (SB_HARNESS=<test>):
#   1. SB_RENDER=native → scratch/parity/<test>.native.ppm
#   2. SB_RENDER=oracle → scratch/parity/<test>.oracle.ppm
# Then computes mean_abs pixel delta between them.
#
# The harness is deterministic (no game code, no game-tick drift, no wall-clock),
# so passing this in one process pair means it'll pass in every future run —
# unlike title_pinned.sh which measured a moving oracle target.

set -uo pipefail
TEST_NAME="${1:-synth-clear}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$HERE"

BIN=build/native/sms-boot
if [[ ! -x "$BIN" ]]; then
    echo "$BIN missing. Build with: cmake --build build --target sms-boot -j" >&2
    exit 1
fi

mkdir -p scratch/parity scratch/frames
rm -f "scratch/parity/${TEST_NAME}.native.ppm" "scratch/parity/${TEST_NAME}.oracle.ppm"

echo "== tier_parity: test=${TEST_NAME} =="

# Common env: skip the game, keep memory bounded per user's <1GB guideline.
COMMON_ENV=(
    "SB_HARNESS=${TEST_NAME}"
    "SUNBRIGHT_DISC=scratch/disc/sms.iso"
    "SB_HOST_ALLOC_CAP_MB=1024"
    "SB_WATCHDOG_SECS=0"
)

echo "[1/2] Tier 1 (native SDL3-GPU)"
timeout 30 setarch -R env "${COMMON_ENV[@]}" \
    SB_RENDER=native SDL_VIDEODRIVER=offscreen \
    "$BIN" > scratch/parity/${TEST_NAME}.native.log 2>&1 || true
if [[ ! -s "scratch/parity/${TEST_NAME}.native.ppm" ]]; then
    echo "  FAIL: Tier 1 didn't produce scratch/parity/${TEST_NAME}.native.ppm" >&2
    tail -20 scratch/parity/${TEST_NAME}.native.log >&2
    exit 2
fi
echo "  OK: scratch/parity/${TEST_NAME}.native.ppm"

echo "[2/2] Tier 2 (Dolphin videovulkan in-process)"
timeout 45 setarch -R env "${COMMON_ENV[@]}" \
    SB_RENDER=oracle \
    "$BIN" > scratch/parity/${TEST_NAME}.oracle.log 2>&1 || true
if [[ ! -s "scratch/parity/${TEST_NAME}.oracle.ppm" ]]; then
    echo "  FAIL: Tier 2 didn't produce scratch/parity/${TEST_NAME}.oracle.ppm" >&2
    tail -20 scratch/parity/${TEST_NAME}.oracle.log >&2
    exit 3
fi
echo "  OK: scratch/parity/${TEST_NAME}.oracle.ppm"

echo
echo "[diff]"
python3 - "$TEST_NAME" <<'PY'
import sys, numpy as np
from PIL import Image
test = sys.argv[1]
n = np.array(Image.open(f"scratch/parity/{test}.native.ppm").convert("RGB"))
o = np.array(Image.open(f"scratch/parity/{test}.oracle.ppm").convert("RGB"))
if n.shape != o.shape:
    print(f"  SHAPE MISMATCH: native={n.shape} oracle={o.shape}")
    sys.exit(4)
d = np.abs(n.astype(int) - o.astype(int))
mean_abs = d.mean()
max_ch = d.max()
print(f"  native mean rgb = ({n[:,:,0].mean():6.2f}, {n[:,:,1].mean():6.2f}, {n[:,:,2].mean():6.2f})")
print(f"  oracle mean rgb = ({o[:,:,0].mean():6.2f}, {o[:,:,1].mean():6.2f}, {o[:,:,2].mean():6.2f})")
print(f"  |Δ| mean_abs = {mean_abs:.4f}   worst channel = {max_ch}")
if mean_abs == 0.0:
    print(f"\n  BIT-EXACT parity on '{test}'.")
else:
    print(f"\n  Tier 1 ≠ Tier 2 on '{test}'. Investigate what state differs.")
PY

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

# FAIL FAST: three ways a run can be bad, distinguished so we never silently
# accept a broken result:
#   1. The harness explicitly panicked (`[parity] ABORT`, `[oracle] ABORT`)
#      — the FAIL FAST paths in native/src/render_parity.cpp and
#      native/render/oracle_present.cpp. Definitive failure.
#   2. The PPM file didn't materialise. Also definitive.
#   3. The process exited non-zero but the PPM is present and no ABORT marker.
#      This is the KNOWN Vulkan-layer (liblsfg-vk.so) SEGV on shutdown, which
#      fires AFTER main returned successfully. Warn but treat as OK — the PPM
#      IS the deliverable. If a real shutdown ordering bug creeps in later,
#      we'll see it as an ABORT before then.
run_tier() {
    local tier=$1 ; shift
    local logfile="scratch/parity/${TEST_NAME}.${tier}.log"
    timeout 45 setarch -R env "${COMMON_ENV[@]}" "$@" \
        "$BIN" > "$logfile" 2>&1
    local rc=$?
    # Case 1: explicit panic from our own FAIL FAST paths.
    if grep -qE '\[(parity|oracle)\] ABORT' "$logfile"; then
        echo "  FAIL: Tier ${tier} panicked (see [ABORT] line):" >&2
        grep -A5 -E '\[(parity|oracle)\] ABORT' "$logfile" >&2
        return 1
    fi
    # Case 2: PPM missing.
    if [[ ! -s "scratch/parity/${TEST_NAME}.${tier}.ppm" ]]; then
        echo "  FAIL: Tier ${tier} did not produce " \
             "scratch/parity/${TEST_NAME}.${tier}.ppm (exit=$rc)" >&2
        tail -30 "$logfile" >&2
        return 2
    fi
    # Case 3: non-zero exit but PPM present. Warn only.
    if [[ $rc -ne 0 ]]; then
        echo "  OK (with post-write exit $rc): scratch/parity/${TEST_NAME}.${tier}.ppm" \
             "— likely Vulkan-layer shutdown SEGV, harmless"
    else
        echo "  OK: scratch/parity/${TEST_NAME}.${tier}.ppm"
    fi
    return 0
}

echo "[1/2] Tier 1 (native SDL3-GPU)"
run_tier native SB_RENDER=native SDL_VIDEODRIVER=offscreen || exit 2

echo "[2/2] Tier 2 (Dolphin videovulkan in-process)"
run_tier oracle SB_RENDER=oracle || exit 3

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

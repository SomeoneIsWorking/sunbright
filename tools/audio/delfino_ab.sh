#!/usr/bin/env bash
# Delfino oracle-vs-native voice comparison driver.
#
#   tools/audio/delfino_ab.sh [seconds-per-side]
#
# Runs BOTH sides sequentially (never overlap two instances), headless with
# SUNBRIGHT_AUTOSTART driving boot → file select → Delfino, polling the probe REPL the
# whole run, then prints the per-wave pitch/volume comparison.
#   oracle: SUNBRIGHT_DISABLE_RECOMP=1 SUNBRIGHT_BACKEND=OGL (Vulkan oracle dies at 3D entry)
#   native: default backend
# Captures land in scratch/logs/delfino_{oracle,native}.jsonl.
set -euo pipefail
cd "$(dirname "$0")/../.."
SECS="${1:-150}"
PROBE=http://127.0.0.1:17654
mkdir -p scratch/logs
set -a; [ -f .env ] && . ./.env; set +a

run_side() { # name, extra-env...
    local name="$1"; shift
    echo "=== $name run (${SECS}s) ==="
    env SUNBRIGHT_HEADLESS=1 SUNBRIGHT_AUTOSTART=1 SUNBRIGHT_PROBE=1 "$@" \
        timeout -s KILL $((SECS + 25)) ./build/sunbright \
        > "scratch/logs/delfino_${name}.log" 2>&1 &
    local pid=$!
    sleep 10   # let the probe come up
    local mode=capture-native
    [ "$name" = oracle ] && mode=capture-oracle
    python3 tools/audio/vpb_compare.py "$mode" "$PROBE" \
        "scratch/logs/delfino_${name}.jsonl" "$((SECS - 10))" || true
    wait "$pid" || true
}

run_side native
run_side oracle SUNBRIGHT_DISABLE_RECOMP=1 SUNBRIGHT_BACKEND=OGL

echo
python3 tools/audio/vpb_compare.py compare \
    scratch/logs/delfino_oracle.jsonl scratch/logs/delfino_native.jsonl

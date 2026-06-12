#!/usr/bin/env bash
# Dev run: fastboot straight into File 1 / Delfino Plaza, audio diagnostics on,
# log captured to scratch/logs/. Usage: ./run-dev.sh [extra ENV=VAL ...]
#   ./run-dev.sh                       # headed dev run
#   SUNBRIGHT_HEADLESS=1 ./run-dev.sh  # headless
cd "$(dirname "$0")"
set -a; [ -f .env ] && source .env; set +a
mkdir -p scratch/logs
LOG="scratch/logs/dev_$(date +%H%M%S).log"
echo "log: $LOG"
SUNBRIGHT_FASTBOOT=1 \
SUNBRIGHT_DBG_NJAS=1 \
SUNBRIGHT_PROBE=1 \
SDL_VIDEODRIVER=x11 \
"$@" ./build/sunbright "${SUNBRIGHT_ROM:-rom.rvz}" 2>&1 | tee "$LOG"

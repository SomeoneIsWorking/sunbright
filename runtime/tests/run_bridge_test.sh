#!/usr/bin/env bash
# Standalone unit test for runtime/bridge.h (the native<->recomp marshalling
# thunk). No Dolphin / no recomp build needed — the test stubs the runtime
# externs. Plain C++17.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p ../../scratch/bin
out=../../scratch/bin/bridge_test
g++ -std=c++17 -O0 -g -Wall -Wextra bridge_test.cpp -o "$out"
"$out"

#!/usr/bin/env bash
# Probe which decomp sources compile cleanly with the port compat shims.
# Usage: port/try_compile.sh [-p] <file1> [file2 ...]   (paths relative to repo root)
#   -p  add -fpermissive (CodeWarrior-lax template/two-phase-lookup tolerance)
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
OUT="$ROOT/port/build/probe"
mkdir -p "$OUT"
EXTRA=()
if [ "${1:-}" = "-p" ]; then EXTRA+=(-fpermissive); shift; fi
INCS=(-I port/compat/include -I reference/sms/include)
FORCE=(-include port/compat/intrinsics.h -include port/compat/msl_shim.h)
pass=0; fail=0
: > "$OUT/pass.txt"; : > "$OUT/fail.txt"
for f in "$@"; do
  tag=$(echo "$f" | tr '/' '_')
  obj="$OUT/$tag.o"
  err="$OUT/$tag.err"
  g++ -std=gnu++17 -w "${EXTRA[@]}" -c "${INCS[@]}" "${FORCE[@]}" "$f" -o "$obj" 2>"$err"
  if [ $? -eq 0 ]; then
    echo "PASS $f"; echo "$f" >> "$OUT/pass.txt"; pass=$((pass+1))
  else
    first=$(grep -m1 -E "error:" "$err" | sed 's/^[^ ]*: //' | head -c 160)
    echo "FAIL $f :: $first"; printf '%s :: %s\n' "$f" "$first" >> "$OUT/fail.txt"; fail=$((fail+1))
  fi
done
echo "----- pass=$pass fail=$fail"

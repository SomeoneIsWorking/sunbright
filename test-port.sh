#!/usr/bin/env bash
# =============================================================================
# test-port.sh — build & test the NATIVE SMS PC port (port/), end to end.
#
# This exercises the work-in-progress source-port of the decompilation
# (reference/sms) into a native x86-64/arm64 binary — NOT the Dolphin-based
# recompiler (that's ./run.sh). The port does not yet link a runnable game
# executable (the decomp's connected link graph is still being compiled in +
# the recompiler gap-filler isn't wired), so "test" here means:
#   1. build the native engine libraries (proves the decomp compiles natively),
#   2. run the unit/integration tests that DO run today (math PAL + the
#      endian-safe Yaz0/RARC/BTI asset pipeline on real GameCube assets),
#   3. print the live link-progress metrics (TUs compiled, undefined refs).
#
# Usage:
#   ./test-port.sh              # incremental build + tests + status
#   ./test-port.sh --clean      # full clean rebuild first (use after header edits;
#                               # cmake/ccache can serve stale objects otherwise)
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")"

BUILD=port/build/test
CLEAN_FIRST=""
[ "${1:-}" = "--clean" ] && CLEAN_FIRST="--clean-first"

# The decomp's shadow-header edits don't always invalidate cmake/ccache caches;
# disable ccache so a build is always faithful to the current sources.
export CCACHE_DISABLE=1

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
rule() { printf '%s\n' "------------------------------------------------------------"; }

bold "==> [1/4] Configure (port/ -> $BUILD)"
cmake -S port -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null || { echo "CONFIGURE FAILED"; exit 1; }

bold "==> [2/4] Build native engine libraries + tests"
if ! cmake --build "$BUILD" -j"$(nproc)" $CLEAN_FIRST; then
    echo "BUILD FAILED"; exit 1
fi

rule
bold "==> [3/4] Native build status"
core_tus=$(ar t "$BUILD/libsmsport_core.a" 2>/dev/null | wc -l)
printf '    %-22s %s decompiled engine TUs compiled natively\n' "libsmsport_core.a:" "$core_tus"
for L in smsport_pal smsport_assets smsport_texdecode; do
    [ -f "$BUILD/lib$L.a" ] && printf '    %-22s %s objects\n' "lib$L.a:" "$(ar t "$BUILD/lib$L.a" | wc -l)"
done

# Link-progress probe: smsport_main is EXCLUDE_FROM_ALL and not expected to link
# yet — its undefined-reference count is the remaining-work metric.
probe=$(cmake --build "$BUILD" --target smsport_main 2>&1)
mdef=$(printf '%s' "$probe" | grep -c 'multiple definition')
undef=$(printf '%s' "$probe" | grep -c 'undefined reference')
printf '    %-22s %s\n' "link multiple-defs:" "$mdef (must stay 0)"
printf '    %-22s %s  (the connected-graph worklist; climbs with coverage)\n' "link undefined refs:" "$undef"

rule
bold "==> [4/4] Tests"
# bti_test needs a real GameCube archive (copyrighted, gitignored). Skip it
# gracefully if the extracted asset isn't present; mtx_test + rarc_test are
# self-contained and always run (rarc has a synthetic fixture fallback).
TESTS_FILTER=""
if [ ! -f scratch/audiores/data/nintendo.szs ]; then
    echo "    (note: bti_test needs an extracted GC archive at"
    echo "     scratch/audiores/data/nintendo.szs — absent, so skipping it.)"
    TESTS_FILTER="-E bti_test"
fi
if ( cd "$BUILD" && ctest --output-on-failure $TESTS_FILTER ); then
    rule; bold "RESULT: PASS — native libs built ($core_tus TUs), tests green."
    exit 0
else
    rule; bold "RESULT: a test FAILED (see output above)."
    exit 1
fi

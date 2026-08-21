#!/usr/bin/env bash
# Sunbright's shipping entry point. No arguments boot the game immediately; Escape opens RmlUi
# settings during play. Development runtimes stay behind explicit script names.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$HERE/play.sh" "$@"

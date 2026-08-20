#!/usr/bin/env bash
# Sunbright's shipping entry point. No arguments launch the current recomp product and its RmlUi
# prelaunch settings screen; development runtimes stay behind explicit script names.
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$HERE/play.sh" "$@"

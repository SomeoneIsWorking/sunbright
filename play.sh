#!/usr/bin/env bash
# Compatibility name for the shipping launcher. All policy lives in the locked Python owner reached
# by run.sh, so this path cannot bypass the live GPU watcher or drift from the default product.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$HERE/run.sh" "$@"

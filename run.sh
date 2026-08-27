#!/usr/bin/env bash
# Stable shipping shim. Locked Python owns argument parsing, launch policy, and the live GPU guard.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec uv run --frozen --project "$HERE" python "$HERE/tools/launch/run.py" "$@"

#!/usr/bin/env bash
# Stable native-preview interface; all policy lives in the locked Python owner.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec uv run --frozen --project "$HERE" python "$HERE/tools/render/run_render.py" "$@"

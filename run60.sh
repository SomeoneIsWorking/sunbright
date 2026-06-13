#!/usr/bin/env bash
# Fast-boot straight into gameplay (File 1, Delfino Plaza) with 60 fps
# interpolation ON — convenience wrapper over run.sh for testing the 60fps path.
#
# Usage:
#   ./run60.sh [rom.rvz] [extra sunbright args...]
#   SUNBRIGHT_BACKEND=OGL ./run60.sh     # OpenGL instead of Vulkan (default)
#   SUNBRIGHT_DBG_INTERP60=0 ./run60.sh  # quiet (debug accounting is on by default)
#
# Everything 60-fps gates on the single SSOT flag SUNBRIGHT_INTERP60 (runtime/
# interp60.h). The renderer is the same FIFO-safe tailored frontend used by
# vanilla — 60fps just adds the in-between redraw.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export SUNBRIGHT_FASTBOOT="${SUNBRIGHT_FASTBOOT:-1}"
export SUNBRIGHT_INTERP60="${SUNBRIGHT_INTERP60:-1}"
export SUNBRIGHT_DBG_INTERP60="${SUNBRIGHT_DBG_INTERP60:-1}"

exec "$HERE/run.sh" "$@"

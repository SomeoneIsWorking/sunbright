#pragma once
// Single source of truth for the 60 fps interpolation feature.
//
// SUNBRIGHT_INTERP60=1 turns it on. Everything 60-fps-related gates on this one
// function — there are deliberately no sub-flags (no per-list bisect mask, no
// blend on/off). Defined in runtime/overrides/interp_redraw.cpp.
bool sunbright_interp60();

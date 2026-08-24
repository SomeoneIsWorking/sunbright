#pragma once

#include "scene.h"

// Evaluate one GX texture-coordinate generator from the raw model-space vertex and the
// rasterized colour that the XF lighting stage produced for it.
void sbr_texgen(const SbrXfState& xf, unsigned generator, const SbrGeomVert& vertex,
                const float rasterColor[4], float out[2]);

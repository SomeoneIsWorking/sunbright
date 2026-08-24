#pragma once

#include "scene.h"

#include <cstdint>
#include <vector>

// Geometry-cache record used by the scene renderer after capture-time geometry has been interned.
// This is an internal renderer seam: public capture hooks remain declared in scene.h.
struct SbrSceneGeometry {
    std::vector<SbrGeomVert> verts;
    bool multislot = false;
};

// Resolve a 1-based geometry handle. Returns nullptr for handle 0 or an out-of-range handle.
const SbrSceneGeometry* sbr_scene_find_geometry(uint32_t handle);

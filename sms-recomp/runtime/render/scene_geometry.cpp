#include "scene_geometry.h"

#include <cstddef>
#include <unordered_map>
#include <utility>

namespace {

// Handles are 1-based so 0 keeps meaning "no geometry".
std::vector<SbrSceneGeometry> g_geometry{1};
std::unordered_map<uint64_t, uint32_t> g_geometryIndex;
int g_multislot = 0;
int g_splitTriangles = 0;

} // namespace

bool sbr_scene_has_geometry(uint64_t key) {
    return g_geometryIndex.contains(key);
}

// Overwrite an already-interned geometry in place, or intern it if new. 2D geometry CHANGES every
// frame under a stable identity (a counter digit keeps its screen slot while its glyph changes), so
// content-keyed interning minted a fresh entry per frame — 387k of them in one run. The cache is a
// vector and callers hold a reference into it while consuming a geometry, so that growth
// reallocates and dangles those references: undefined behaviour, and the reason enabling 2D capture
// collapsed the whole frame rather than just leaking. Updating in place keeps the entry count
// bounded by the number of distinct elements.
uint32_t sbr_scene_update_geometry(uint64_t key, const SbrGeomVert* verts, int count) {
    if (const auto it = g_geometryIndex.find(key); it != g_geometryIndex.end()) {
        SbrSceneGeometry& geometry = g_geometry[it->second];
        geometry.verts.assign(verts, verts + count);
        geometry.multislot = false;
        for (int i = 1; i < count; ++i) {
            if (verts[i].slot != verts[0].slot) {
                geometry.multislot = true;
                break;
            }
        }
        return it->second;
    }
    return sbr_scene_intern_geometry(key, verts, count);
}

uint32_t sbr_scene_intern_geometry(uint64_t key, const SbrGeomVert* verts, int count) {
    if (const auto it = g_geometryIndex.find(key); it != g_geometryIndex.end())
        return it->second;

    const uint32_t id = static_cast<uint32_t>(g_geometry.size());
    g_geometry.push_back({});
    SbrSceneGeometry& geometry = g_geometry.back();
    geometry.verts.assign(verts, verts + count);

    // A shape whose vertices select more than one matrix slot is SKINNED. The drawable carries one
    // matrix, so such a mesh renders with the wrong transform on every vertex outside the first
    // slot. Count it instead of hiding it: the per-vertex matrix path is the next step, and this
    // number is how big it is.
    for (int i = 1; i < count; ++i) {
        if (verts[i].slot != verts[0].slot) {
            geometry.multislot = true;
            ++g_multislot;
            break;
        }
    }

    g_geometryIndex.emplace(key, id);
    return id;
}

uint32_t sbr_scene_geometry_for_slot(uint64_t baseKey, uint32_t baseGeom, uint32_t slot) {
    const SbrSceneGeometry* base = sbr_scene_find_geometry(baseGeom);
    if (base == nullptr)
        return 0;
    // Unskinned fast path: every vertex is on one slot, so the element IS the slot's geometry.
    if (!base->multislot)
        return baseGeom;

    const uint64_t key = (baseKey << 8) | static_cast<uint64_t>(slot) | 0x8000000000000000ull;
    if (const auto it = g_geometryIndex.find(key); it != g_geometryIndex.end())
        return it->second;

    std::vector<SbrGeomVert> sub;
    // Whole TRIANGLES only: a triangle whose vertices span two bones cannot be drawn by a single
    // matrix. Keeping it with the slot of its first vertex is what the hardware does per-vertex
    // only approximately — flagged rather than hidden (see sbr_scene_split_triangles).
    for (size_t triangle = 0; triangle + 2 < base->verts.size(); triangle += 3) {
        if (base->verts[triangle].slot != slot && base->verts[triangle + 1].slot != slot &&
            base->verts[triangle + 2].slot != slot)
            continue;
        if (base->verts[triangle].slot != base->verts[triangle + 1].slot ||
            base->verts[triangle].slot != base->verts[triangle + 2].slot)
            ++g_splitTriangles;
        if (base->verts[triangle].slot != slot)
            continue;
        sub.push_back(base->verts[triangle]);
        sub.push_back(base->verts[triangle + 1]);
        sub.push_back(base->verts[triangle + 2]);
    }
    if (sub.empty())
        return 0;

    const uint32_t id = static_cast<uint32_t>(g_geometry.size());
    g_geometry.push_back({});
    g_geometry.back().verts = std::move(sub);
    g_geometryIndex.emplace(key, id);
    return id;
}

const SbrSceneGeometry* sbr_scene_find_geometry(uint32_t handle) {
    if (handle == 0 || handle >= g_geometry.size())
        return nullptr;
    return &g_geometry[handle];
}

int sbr_scene_multislot_count() {
    return g_multislot;
}

int sbr_scene_split_triangles() {
    return g_splitTriangles;
}

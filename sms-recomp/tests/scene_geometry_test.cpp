#include "runtime/render/scene_geometry.h"

#include <iterator>
#include <stdexcept>

namespace {

SbrGeomVert vertex(float x, uint32_t slot) {
    SbrGeomVert result{};
    result.x = x;
    result.slot = slot;
    return result;
}

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main() {
    constexpr uint64_t kStaticKey = 0x12340001;
    SbrGeomVert staticTriangle[] = {vertex(1.0f, 2), vertex(2.0f, 2), vertex(3.0f, 2)};

    require(!sbr_scene_has_geometry(kStaticKey), "new key was already interned");
    const uint32_t staticHandle =
        sbr_scene_intern_geometry(kStaticKey, staticTriangle, std::size(staticTriangle));
    require(staticHandle != 0, "interning returned reserved handle zero");
    require(sbr_scene_has_geometry(kStaticKey), "interned key was not found");
    require(sbr_scene_intern_geometry(kStaticKey, staticTriangle, std::size(staticTriangle)) ==
                staticHandle,
            "re-interning a key did not reuse its handle");
    require(sbr_scene_geometry_for_slot(kStaticKey, staticHandle, 2) == staticHandle,
            "unskinned geometry did not use its base handle");
    require(sbr_scene_find_geometry(0) == nullptr, "reserved handle zero resolved to geometry");

    staticTriangle[0].x = 9.0f;
    require(sbr_scene_update_geometry(kStaticKey, staticTriangle, std::size(staticTriangle)) ==
                staticHandle,
            "updating geometry changed its stable handle");
    const SbrSceneGeometry* updated = sbr_scene_find_geometry(staticHandle);
    require(updated != nullptr, "updated handle did not resolve");
    require(updated->verts[0].x == 9.0f, "geometry update did not replace vertex data");

    constexpr uint64_t kSkinnedKey = 0x12340002;
    SbrGeomVert skinnedTriangle[] = {vertex(4.0f, 0), vertex(5.0f, 1), vertex(6.0f, 0)};
    const int multislotBefore = sbr_scene_multislot_count();
    const uint32_t skinnedHandle =
        sbr_scene_intern_geometry(kSkinnedKey, skinnedTriangle, std::size(skinnedTriangle));
    require(sbr_scene_multislot_count() == multislotBefore + 1,
            "multislot geometry was not counted");

    const int splitBefore = sbr_scene_split_triangles();
    const uint32_t slotHandle = sbr_scene_geometry_for_slot(kSkinnedKey, skinnedHandle, 0);
    require(slotHandle != 0, "occupied skinned slot returned no geometry");
    require(slotHandle != skinnedHandle, "skinned slot reused unsplit base geometry");
    const SbrSceneGeometry* slotGeometry = sbr_scene_find_geometry(slotHandle);
    require(slotGeometry != nullptr, "split geometry handle did not resolve");
    require(slotGeometry->verts.size() == 3, "split geometry did not preserve the triangle");
    require(sbr_scene_split_triangles() == splitBefore + 1, "cross-slot triangle was not counted");

    // Slot geometry is cached, and requesting a slot absent from the element returns no handle.
    require(sbr_scene_geometry_for_slot(kSkinnedKey, skinnedHandle, 0) == slotHandle,
            "split geometry was not cached");
    require(sbr_scene_geometry_for_slot(kSkinnedKey, skinnedHandle, 7) == 0,
            "absent matrix slot returned geometry");
    return 0;
}

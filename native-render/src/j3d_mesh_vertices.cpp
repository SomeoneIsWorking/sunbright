#include <sunbright/native_render/j3d_mesh_vertices.h>

namespace sb::native_render {

bool build_j3d_mesh_vertices(std::span<const J3dDecodedVertex> decoded,
                             std::span<const std::uint8_t> slotToPoseIndex,
                             std::vector<MeshVertex>& out) {
    out.clear();
    if (decoded.empty()) {
        return false;
    }
    out.reserve(decoded.size());
    for (const J3dDecodedVertex& vertex : decoded) {
        if (vertex.positionMatrixSlot >= slotToPoseIndex.size()) {
            out.clear();
            return false;
        }
        const std::uint8_t index = slotToPoseIndex[vertex.positionMatrixSlot];
        if (index == kUnmappedMatrixSlot) {
            out.clear();
            return false;
        }
        out.push_back({.position = {vertex.x, vertex.y, vertex.z},
                       .uv = {vertex.uv[0][0], vertex.uv[0][1]},
                       .uv1 = {vertex.uv[1][0], vertex.uv[1][1]},
                       .uv2 = {vertex.uv[2][0], vertex.uv[2][1]},
                       .uv3 = {vertex.uv[3][0], vertex.uv[3][1]},
                       .color = color_from_rgba8(vertex.rgba),
                       .normal = {vertex.nx, vertex.ny, vertex.nz},
                       .matrixIndex = index});
    }
    return true;
}

} // namespace sb::native_render

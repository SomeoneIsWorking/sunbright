#include <sunbright/title_adapter/guest_shape_geometry.h>

namespace sb::title_adapter {

GuestShapeGeometry
read_guest_shape_geometry(const GuestMemory& memory, native_render::J3dByteReader reader,
                          const GuestShape& shape, std::uint16_t element, GuestAddress system,
                          const GuestMatrixGroupVtables& vtables, GuestMatrixRegisters& registers,
                          std::vector<native_render::J3dDecodedVertex>& triangles) noexcept {
    GuestShapeGeometry geometry{};
    triangles.clear();
    geometry.elementError = read_guest_shape_element(memory, shape, element, geometry.element);
    if (geometry.elementError != GuestShapeError::None) {
        return geometry;
    }
    geometry.mesh = decode_j3d_mesh_element(
        guest_mesh_element_source(shape, geometry.element, reader), triangles);
    // The pose is read even when the mesh did not decode. The pose reader carries the matrix
    // registers forward, so skipping it would leave the groups drawn after this one believing they
    // inherited slots that were never loaded -- a later group's vertices would then be posed with
    // another shape's matrices.
    geometry.poseError =
        read_guest_shape_pose(memory, shape, element, system, vtables, registers, geometry.pose);
    return geometry;
}

} // namespace sb::title_adapter

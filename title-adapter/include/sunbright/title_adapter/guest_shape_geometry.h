#pragma once

#include <sunbright/native_render/j3d_mesh_decode.h>
#include <sunbright/title_adapter/guest_j3d_pose.h>
#include <sunbright/title_adapter/guest_j3d_shape.h>

#include <cstdint>
#include <vector>

namespace sb::title_adapter {

// One matrix group of a shape, read whole: the group itself, the triangles its display list
// decodes to, and the pose those triangles are drawn under.
//
// The three are read together because they are one answer. A group's vertices name matrix
// registers, and only its pose says which registers it loaded and which it inherited from the
// groups drawn before it, so decoding the mesh without the pose produces vertices whose matrix
// indices mean nothing. Keeping the composition in one place is also what stops two callers
// drifting into two orders of the same reads -- the pose reader carries state across groups, and
// calling it in a different order than the title draws them silently loses the inherited slots.
struct GuestShapeGeometry {
    GuestShapeElement element{};
    GuestShapeError elementError = GuestShapeError::None;
    native_render::J3dMeshDecodeResult mesh{};
    GuestShapePose pose{};
    GuestPoseError poseError = GuestPoseError::None;

    // Whether every part came back. A caller that wants only the triangles still has to ask,
    // because a decoded mesh with an unread pose is not a drawable group.
    [[nodiscard]] bool complete() const noexcept {
        return elementError == GuestShapeError::None &&
               mesh.error == native_render::J3dMeshDecodeError::None &&
               poseError == GuestPoseError::None;
    }
};

// `triangles` is cleared and filled with this group's vertices. `registers` is read and written:
// pass the same one across every group of every shape, in the order the title draws them.
[[nodiscard]] GuestShapeGeometry
read_guest_shape_geometry(const GuestMemory& memory, native_render::J3dByteReader reader,
                          const GuestShape& shape, std::uint16_t element, GuestAddress system,
                          const GuestMatrixGroupVtables& vtables, GuestMatrixRegisters& registers,
                          std::vector<native_render::J3dDecodedVertex>& triangles) noexcept;

} // namespace sb::title_adapter

#pragma once

#include <sunbright/native_render/j3d_mesh_decode.h>
#include <sunbright/native_render/model.h>

#include <cstdint>
#include <span>
#include <vector>

namespace sb::native_render {

// Turns one matrix group's decoded vertices into renderer-neutral mesh vertices.
//
// The one thing this does beyond copying fields is remap each vertex's matrix slot. A decoded
// vertex names a hardware matrix register; a `MeshVertex` names an index into the draw's own pose.
// `slotToPoseIndex` is that mapping, and a slot outside it, or one the pose never filled, means the
// group is drawn with a matrix the pose does not contain. Both runtimes refuse such a group rather
// than clamping the index, because clamping would draw the geometry with the wrong transform --
// visibly wrong, and silently so.
constexpr std::uint8_t kUnmappedMatrixSlot = 0xFF;

[[nodiscard]] bool build_j3d_mesh_vertices(std::span<const J3dDecodedVertex> decoded,
                                           std::span<const std::uint8_t> slotToPoseIndex,
                                           std::vector<MeshVertex>& out);

} // namespace sb::native_render

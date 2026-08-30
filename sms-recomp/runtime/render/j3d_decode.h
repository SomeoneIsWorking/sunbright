#pragma once

// Guest-memory adapter for the shared J3D asset decoder. The primitive parser and vertex-array
// rules live in native-render so the recomp and decomp runtimes cannot drift into two meanings of
// the same BMD bytes.

#include <sunbright/native_render/j3d_mesh_decode.h>

#include <intrinsics.h>

#include <cstdint>
#include <vector>

using J3DVertexLayout = sb::native_render::J3dVertexLayout;
using J3DVert = sb::native_render::J3dDecodedVertex;

constexpr std::uint32_t J3D_TEXCOORD_SETS = sb::native_render::kJ3dTextureCoordinateSets;

bool j3d_build_layout(u32 shape, J3DVertexLayout& out);
bool j3d_decode_element(u32 shape, std::uint32_t element, const J3DVertexLayout& layout,
                        std::vector<J3DVert>& out);

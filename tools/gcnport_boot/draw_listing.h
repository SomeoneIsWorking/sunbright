// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>

#include <sunbright/native_render/j3d_material_family.h>
#include <sunbright/native_render/j3d_mesh_decode.h>
#include <sunbright/native_render/model.h>
#include <sunbright/title_adapter/guest_j3d_pose.h>
#include <sunbright/title_adapter/guest_j3d_texgen.h>

namespace sunbright::gcnport_boot {

// Names one draw of one frame, in full, on the run's output.
//
// Bounding a frame's draws attributes a defect to a position in the submission order; it cannot say
// what is at that position. This does: the family and raster policy the material classified into,
// the mesh it came from, how its texture coordinates are generated, what its first texture holds,
// the range of coordinates the geometry actually samples it over, and the colour the draw resolves
// to at its vertices. Those are the facts that separate "this surface computes the wrong colour"
// from "this surface samples the right texture in the wrong place", which are different repairs in
// different owners. The colour is taken from the draw as submitted, through the shipping
// `transform_vertex`, so it is the value the vertex shader is handed rather than one restated here
// per material family.
//
// It is deliberately verbose and deliberately rare: one frame of a run, named by
// `--draw-log-frame`. A listing cheap enough to leave on for every frame would have to leave out
// the parts that take work to produce, and those are the parts a bisection needs.
// Everything one line of the listing is made of, as one argument. It is a parameter object rather
// than nine parameters because the facts it carries come from four different owners and keep being
// added to: named fields at the call site say which is which, where a positional list of spans and
// integers had begun to rely on the reader's memory.
struct DrawListing {
    std::uint64_t frame = 0;
    std::uint64_t ordinal = 0;
    const sb::native_render::ClassifiedJ3dMaterial* classified = nullptr;
    const sb::title_adapter::GuestTexGenBlock* texGen = nullptr;
    const sb::title_adapter::GuestShapePose* pose = nullptr;
    std::span<const sb::native_render::DecodedImageView> images;
    std::span<const sb::native_render::J3dDecodedVertex> triangles;
    const sb::native_render::ModelDraw* draw = nullptr;
    std::span<const sb::native_render::MeshVertex> vertices;
    std::uint64_t mesh = 0;
};

void print_draw_listing(const DrawListing& listing);

} // namespace sunbright::gcnport_boot

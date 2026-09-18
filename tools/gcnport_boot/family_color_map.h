// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <sunbright/native_render/j3d_material_family.h>
#include <sunbright/native_render/model.h>

// Which material family drew which part of the image.
//
// A rendered frame says the geometry arrived and the shading is wrong; it does not say which of the
// fifteen material families GMSE01 runs is responsible for any particular part of it. This replaces
// every draw's material with a flat colour naming its family, keeping the culling and depth test
// the classifier produced but drawing opaque: a blended map answers in mixtures, and a mixture of
// two legend colours is a third colour that names no family. What that costs is exact: a surface
// the title draws translucently covers what is behind it here, so the map attributes the frontmost
// geometry rather than everything that contributed to a pixel. The result is not a picture of the
// game and is never a rendering result -- it is a map, and the only question it answers is
// attribution.

namespace sunbright::gcnport_boot {

// The colour a family is drawn in. Exhaustive over `J3dMaterialFamily`, so a family added later
// fails to compile here rather than silently sharing a neighbour's colour and being read as it.
[[nodiscard]] sb::native_render::Color
family_map_color(sb::native_render::J3dMaterialFamily family) noexcept;

// The family's colour as an unlit material carrying `raster`'s culling and depth test, drawn
// opaque and with no alpha test.
[[nodiscard]] sb::native_render::ModelMaterial
family_map_material(sb::native_render::J3dMaterialFamily family,
                    const sb::native_render::ModelRasterPolicy& raster) noexcept;

// The same material with blending and the alpha test off, so what it computes is written to the
// target whole. A frame rendered this way separates a surface that computes the wrong colour from
// one whose colour is right and is being combined wrongly with what is behind it.
[[nodiscard]] sb::native_render::ModelMaterial
opaque_material(sb::native_render::ModelMaterial material) noexcept;

} // namespace sunbright::gcnport_boot

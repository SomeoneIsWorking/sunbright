// SPDX-License-Identifier: GPL-2.0-or-later
#include "draw_listing.h"

#include <sunbright/native_render/model.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace sunbright::gcnport_boot {
namespace {

const char* address_mode_name(sb::native_render::AddressMode mode) noexcept {
    switch (mode) {
    case sb::native_render::AddressMode::Clamp:
        return "clamp";
    case sb::native_render::AddressMode::Repeat:
        return "repeat";
    case sb::native_render::AddressMode::Mirror:
        return "mirror";
    }
    return "unknown";
}

const char* filter_mode_name(sb::native_render::FilterMode mode) noexcept {
    return mode == sb::native_render::FilterMode::Linear ? "linear" : "nearest";
}

// What the cull mode removes, named the way the clip description reads it: alongside a count of
// clockwise and counter-clockwise triangles, so the two can be compared without knowing which
// winding this pass calls front.
const char* culled_faces(sb::native_render::ModelCullMode mode) noexcept {
    switch (mode) {
    case sb::native_render::ModelCullMode::None:
        return "nothing";
    case sb::native_render::ModelCullMode::Front:
        return "front (clockwise)";
    case sb::native_render::ModelCullMode::Back:
        return "back (counter-clockwise)";
    case sb::native_render::ModelCullMode::All:
        return "every triangle, so the pass submits no batch at all";
    }
    return "unknown";
}

// What the draw's first bound texture is and what it holds.
//
// A draw that paints white where nothing should be is either drawing a white texture, drawing a
// coloured one wrongly, or sampling a correct one outside its authored range -- three different
// repairs. The mean separates the first from the rest; the addressing modes decide what happens
// past the edge, which is the difference between a tiled pattern and a stretched edge texel.
std::string describe_first_texture(const sb::native_render::ClassifiedJ3dMaterial& classified,
                                   std::span<const sb::native_render::DecodedImageView> images) {
    if (images.empty()) {
        return {};
    }
    const sb::native_render::DecodedImageView& image = images.front();
    const std::size_t pixels = image.rgba8.size() / 4U;
    if (pixels == 0) {
        return " [first image has no pixels]";
    }
    std::array<std::uint64_t, 4> sums{};
    std::array<std::uint8_t, 4> lowest{255, 255, 255, 255};
    std::array<std::uint8_t, 4> highest{};
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        for (std::size_t channel = 0; channel < 4U; ++channel) {
            const std::uint8_t value = image.rgba8[(pixel * 4U) + channel];
            sums[channel] += value;
            lowest[channel] = std::min(lowest[channel], value);
            highest[channel] = std::max(highest[channel], value);
        }
    }
    const sb::native_render::PictureTexture& texture = classified.textures.front().texture;
    std::array<char, 320> text{};
    const int written = std::snprintf(
        text.data(), text.size(),
        " [0x%08llx %ux%u %s/%s %s/%s mean rgba %llu,%llu,%llu,%llu range r%u-%u g%u-%u b%u-%u "
        "a%u-%u]",
        static_cast<unsigned long long>(image.resource), image.width, image.height,
        address_mode_name(texture.addressU), address_mode_name(texture.addressV),
        filter_mode_name(texture.minFilter), filter_mode_name(texture.magFilter),
        static_cast<unsigned long long>(sums[0] / pixels),
        static_cast<unsigned long long>(sums[1] / pixels),
        static_cast<unsigned long long>(sums[2] / pixels),
        static_cast<unsigned long long>(sums[3] / pixels), lowest[0], highest[0], lowest[1],
        highest[1], lowest[2], highest[2], lowest[3], highest[3]);
    if (written <= 0) {
        return " [first image could not be described]";
    }
    return std::string(text.data(), static_cast<std::size_t>(written));
}

// How the material generates the texture coordinates this draw is about to use.
//
// The vertices carry authored coordinates; GX does not draw with them directly. Each generator
// names a source and a matrix, and a material that scales or scrolls a small texture over a large
// surface does it entirely through that matrix. A listing that omits this cannot tell a draw whose
// coordinates are the ones in the vertex stream from one whose are a transform of them.
std::string describe_tex_gen(const sb::title_adapter::GuestTexGenBlock& texGen) {
    if (!texGen.recognised) {
        return " texgen: block unrecognised";
    }
    if (texGen.texGenCount > sb::title_adapter::kGuestMaxTexGenCount) {
        return " texgen: count unsupported";
    }
    if (texGen.texGenCount == 0) {
        return " texgen: none";
    }
    std::string text = " texgen";
    for (std::uint32_t coordinate = 0; coordinate < texGen.texGenCount; ++coordinate) {
        const sb::title_adapter::GuestTexCoordDefinition& definition =
            texGen.coordinates[coordinate];
        const std::uint32_t slot =
            sb::title_adapter::guest_tex_gen_matrix_slot(definition.texGenMatrix);
        std::array<char, 96> head{};
        const int written =
            std::snprintf(head.data(), head.size(), " %u:type=%u/src=%u/mtx=%u", coordinate,
                          definition.texGenType, definition.texGenSrc, definition.texGenMatrix);
        if (written <= 0) {
            return " texgen: could not be described";
        }
        text.append(head.data(), static_cast<std::size_t>(written));
        if (slot >= sb::title_adapter::kGuestMaxTexMatrixCount) {
            text += "(identity)";
            continue;
        }
        const sb::title_adapter::GuestTexMatrix& matrix = texGen.matrices[slot];
        if (!matrix.present) {
            text += "(slot empty)";
            continue;
        }
        std::array<char, 256> body{};
        const int size = std::snprintf(
            body.data(), body.size(),
            "(proj=%u info=%u total=[%g %g %g %g; %g %g %g %g; %g %g %g %g])", matrix.projection,
            matrix.info, matrix.total[0], matrix.total[1], matrix.total[2], matrix.total[3],
            matrix.total[4], matrix.total[5], matrix.total[6], matrix.total[7], matrix.total[8],
            matrix.total[9], matrix.total[10], matrix.total[11]);
        if (size <= 0) {
            return " texgen: matrix could not be described";
        }
        text.append(body.data(), static_cast<std::size_t>(size));
    }
    return text;
}

// The range of texture coordinates this draw samples over, per generated set, after the material's
// generators have been applied.
//
// A texture is only as magnified as the span it is sampled over. A set whose range is a small
// fraction of one covers the whole surface with a handful of texels; one that runs to several is
// tiled or clamped. Neither can be read from the texture or the material alone.
std::string
describe_coordinate_range(std::span<const sb::native_render::J3dDecodedVertex> triangles,
                          std::uint32_t sets) {
    if (triangles.empty() || sets == 0) {
        return {};
    }
    const std::uint32_t reported = std::min(sets, sb::native_render::kJ3dTextureCoordinateSets);
    std::string text = " uv";
    for (std::uint32_t set = 0; set < reported; ++set) {
        float lowestU = std::numeric_limits<float>::infinity();
        float highestU = -std::numeric_limits<float>::infinity();
        float lowestV = lowestU;
        float highestV = highestU;
        for (const sb::native_render::J3dDecodedVertex& vertex : triangles) {
            lowestU = std::min(lowestU, vertex.uv[set][0]);
            highestU = std::max(highestU, vertex.uv[set][0]);
            lowestV = std::min(lowestV, vertex.uv[set][1]);
            highestV = std::max(highestV, vertex.uv[set][1]);
        }
        std::array<char, 128> body{};
        const int size = std::snprintf(body.data(), body.size(), " %u:u=%g..%g v=%g..%g", set,
                                       lowestU, highestU, lowestV, highestV);
        if (size <= 0) {
            return " uv: could not be described";
        }
        text.append(body.data(), static_cast<std::size_t>(size));
    }
    return text;
}

// The colour the draw resolves to across its vertices, through the shipping transform.
//
// Every material family composes its authored colours, lighting and vertex colours into the two
// values the vertex shader receives, and they are composed differently in each. Reading them back
// out of the draw as submitted is the only description that is true for all of them, and the only
// one that cannot drift from what is actually drawn.
std::string describe_resolved_color(const sb::native_render::ModelDraw& draw,
                                    std::span<const sb::native_render::MeshVertex> vertices) {
    if (vertices.empty()) {
        return {};
    }
    std::array<float, 4> lowest{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> highest{0.0F, 0.0F, 0.0F, 0.0F};
    sb::native_render::Color additive{};
    for (const sb::native_render::MeshVertex& vertex : vertices) {
        const sb::native_render::ClipVertex clip =
            sb::native_render::transform_vertex(draw, vertex);
        const std::array<float, 4> channels{clip.color.r, clip.color.g, clip.color.b, clip.color.a};
        for (std::size_t channel = 0; channel < 4U; ++channel) {
            lowest[channel] = std::min(lowest[channel], channels[channel]);
            highest[channel] = std::max(highest[channel], channels[channel]);
        }
        additive = clip.additiveColor;
    }
    std::array<char, 192> text{};
    const int written = std::snprintf(
        text.data(), text.size(), " colour r%g-%g g%g-%g b%g-%g a%g-%g additive %g,%g,%g,%g",
        lowest[0], highest[0], lowest[1], highest[1], lowest[2], highest[2], lowest[3], highest[3],
        additive.r, additive.g, additive.b, additive.a);
    if (written <= 0) {
        return " colour: could not be described";
    }
    return std::string(text.data(), static_cast<std::size_t>(written));
}

// Where the draw's geometry lands once it has been transformed, and whether anything could have
// rasterised it.
//
// A draw that resolves a sensible colour and covers no pixel at all is answered by nothing above:
// the colour describer runs the same transform and reads only what comes out of it as colour. This
// reads the position. Every count here is a separate way for a draw to disappear -- behind the eye,
// outside the clip volume on one axis, wound the way the pipeline is culling -- and a run prints
// all of them so that "the geometry vanished" is replaced by which one.
//
// The winding is measured in the same convention the pipeline rasterises with: the pass declares
// clockwise front faces, and a clockwise triangle in a y-down screen has a positive signed area.
std::string describe_clip_coverage(const sb::native_render::ModelDraw& draw,
                                   std::span<const sb::native_render::MeshVertex> vertices,
                                   const sb::native_render::ModelRasterPolicy& policy) {
    if (vertices.empty()) {
        return {};
    }
    std::vector<sb::native_render::Vec4> positions;
    positions.reserve(vertices.size());
    std::uint64_t behindEye = 0;
    float lowestX = std::numeric_limits<float>::max();
    float highestX = std::numeric_limits<float>::lowest();
    float lowestY = lowestX;
    float highestY = highestX;
    float lowestZ = lowestX;
    float highestZ = highestX;
    for (const sb::native_render::MeshVertex& vertex : vertices) {
        const sb::native_render::Vec4 position =
            sb::native_render::transform_vertex(draw, vertex).position;
        positions.push_back(position);
        if (!(position.w > 0.0F)) {
            behindEye += 1;
            continue;
        }
        lowestX = std::min(lowestX, position.x / position.w);
        highestX = std::max(highestX, position.x / position.w);
        lowestY = std::min(lowestY, position.y / position.w);
        highestY = std::max(highestY, position.y / position.w);
        lowestZ = std::min(lowestZ, position.z / position.w);
        highestZ = std::max(highestZ, position.z / position.w);
    }

    std::uint64_t clockwise = 0;
    std::uint64_t counterClockwise = 0;
    std::uint64_t degenerate = 0;
    std::uint64_t unprojectable = 0;
    for (std::size_t first = 0; first + 2 < positions.size(); first += 3) {
        const sb::native_render::Vec4& a = positions[first];
        const sb::native_render::Vec4& b = positions[first + 1];
        const sb::native_render::Vec4& c = positions[first + 2];
        if (!(a.w > 0.0F) || !(b.w > 0.0F) || !(c.w > 0.0F)) {
            unprojectable += 1;
            continue;
        }
        const float area = (((b.x / b.w) - (a.x / a.w)) * ((c.y / c.w) - (a.y / a.w))) -
                           (((c.x / c.w) - (a.x / a.w)) * ((b.y / b.w) - (a.y / a.w)));
        if (area > 0.0F) {
            clockwise += 1;
        } else if (area < 0.0F) {
            counterClockwise += 1;
        } else {
            degenerate += 1;
        }
    }

    const char* culled = culled_faces(policy.cull);

    std::array<char, 320> text{};
    const int written =
        behindEye == vertices.size()
            ? std::snprintf(text.data(), text.size(),
                            " clip: every one of %zu vertex(es) is behind the eye; culling %s",
                            vertices.size(), culled)
            : std::snprintf(text.data(), text.size(),
                            " clip: x%g..%g y%g..%g z%g..%g | %llu behind the eye | triangles "
                            "cw=%llu ccw=%llu degenerate=%llu unprojectable=%llu | culling %s",
                            lowestX, highestX, lowestY, highestY, lowestZ, highestZ,
                            static_cast<unsigned long long>(behindEye),
                            static_cast<unsigned long long>(clockwise),
                            static_cast<unsigned long long>(counterClockwise),
                            static_cast<unsigned long long>(degenerate),
                            static_cast<unsigned long long>(unprojectable), culled);
    if (written <= 0) {
        return " clip: could not be described";
    }
    return std::string(text.data(), static_cast<std::size_t>(written));
}

// The two matrices the draw carries, printed as they are rather than summarised.
//
// What the fog stage will do to this draw, printed whether or not it is switched on. A listing that
// says nothing about a draw whose fog is off cannot be told from one whose fog was never read, and
// a distant fragment taken to the fog colour outright is indistinguishable, in the finished frame,
// from one drawn in the wrong colour to begin with.
std::string describe_fog(const sb::native_render::ModelFog& fog) {
    if (fog.mode == sb::native_render::ModelFogMode::Disabled) {
        return ", fog off";
    }
    std::array<char, 160> text{};
    const int written =
        std::snprintf(text.data(), text.size(), ", fog linear %g..%g to rgba %g %g %g %g",
                      fog.start, fog.end, fog.color.r, fog.color.g, fog.color.b, fog.color.a);
    if (written <= 0) {
        return ", fog unprintable";
    }
    return {text.data(), static_cast<std::size_t>(written)};
}

// The clip description above says where the geometry ended up; this says which of the two
// transforms put it there. A model-view whose translation sits at the eye and a projection whose
// depth row is wrong produce the same unusable clip position, and no amount of describing the
// result separates them.
std::string describe_transforms(const sb::native_render::ModelDraw& draw,
                                const sb::native_render::Matrix3x4& view) {
    std::string text = " model-view[0] ";
    std::array<char, 64> number{};
    const auto append = [&](float value, const char* separator) {
        const int written = std::snprintf(number.data(), number.size(), "%g%s", value, separator);
        if (written > 0) {
            text.append(number.data(), static_cast<std::size_t>(written));
        }
    };
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            append(draw.pose.modelViews[0].value[(row * 4) + column], column == 3 ? "" : " ");
        }
        text.append(row == 2 ? "" : "; ");
    }
    text.append(" | projection ");
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            append(draw.projection.value[(row * 4) + column], column == 3 ? "" : " ");
        }
        text.append(row == 3 ? "" : "; ");
    }
    // And the camera the title is drawing with at this moment, which the model-view above is
    // supposed to have concatenated into it. A draw matrix built for an earlier pass's camera is a
    // perfectly well-formed matrix; the only thing that says it is the wrong one is this beside it.
    text.append(" | view ");
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            append(view.value[(row * 4) + column], column == 3 ? "" : " ");
        }
        text.append(row == 2 ? "" : "; ");
    }
    return text;
}

} // namespace

void print_draw_listing(const DrawListing& listing) {
    const sb::native_render::ClassifiedJ3dMaterial& classified = *listing.classified;
    const sb::title_adapter::GuestTexGenBlock& texGen = *listing.texGen;
    const sb::title_adapter::GuestShapePose& pose = *listing.pose;
    const sb::native_render::ModelDraw& draw = *listing.draw;
    const std::span<const sb::native_render::DecodedImageView> images = listing.images;
    const std::span<const sb::native_render::J3dDecodedVertex> triangles = listing.triangles;
    const std::span<const sb::native_render::MeshVertex> vertices = listing.vertices;
    const std::uint64_t frame = listing.frame;
    const std::uint64_t ordinal = listing.ordinal;
    const std::uint64_t mesh = listing.mesh;
    const sb::native_render::ModelRasterPolicy& policy =
        sb::native_render::raster_policy(classified.material);
    std::printf("gmse01_boot:   frame %llu draw %llu: %s %s/%s/depth-write=%d %zu vertex(es) from "
                "0x%08llx, %u texture(s)%s%s\n",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(ordinal),
                sb::native_render::j3d_material_family_name(classified.family),
                sb::native_render::model_blend_mode_name(policy.blend),
                sb::native_render::model_alpha_test_name(policy.alphaTest),
                static_cast<int>(policy.depthWrite), vertices.size(),
                static_cast<unsigned long long>(mesh),
                static_cast<unsigned>(classified.textureCount),
                describe_first_texture(classified, images).c_str(), describe_fog(draw.fog).c_str());
    std::printf(
        "gmse01_boot:    %s%s%s\n", describe_tex_gen(texGen).c_str(),
        describe_coordinate_range(triangles, texGen.recognised ? texGen.texGenCount : 0).c_str(),
        describe_resolved_color(draw, vertices).c_str());
    std::printf("gmse01_boot:   %s\n", describe_clip_coverage(draw, vertices, policy).c_str());
    std::printf("gmse01_boot:   %s pipeline=%s group=%s view=%u palette=0x%08x\n",
                describe_transforms(draw, pose.viewMatrix).c_str(),
                sb::title_adapter::guest_skinning_pipeline_name(pose.pipeline),
                sb::title_adapter::guest_matrix_group_kind_name(pose.kind), pose.viewNumber,
                pose.matrixPalette);
}

} // namespace sunbright::gcnport_boot

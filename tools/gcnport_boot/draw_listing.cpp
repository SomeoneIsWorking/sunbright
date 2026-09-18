// SPDX-License-Identifier: GPL-2.0-or-later
#include "draw_listing.h"

#include <sunbright/native_render/model.h>

#include <array>
#include <cstdio>
#include <limits>
#include <string>

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

} // namespace

void print_draw_listing(std::uint64_t frame, std::uint64_t ordinal,
                        const sb::native_render::ClassifiedJ3dMaterial& classified,
                        const sb::title_adapter::GuestTexGenBlock& texGen,
                        std::span<const sb::native_render::DecodedImageView> images,
                        std::span<const sb::native_render::J3dDecodedVertex> triangles,
                        const sb::native_render::ModelDraw& draw,
                        std::span<const sb::native_render::MeshVertex> vertices,
                        std::uint64_t mesh) {
    const sb::native_render::ModelRasterPolicy& policy =
        sb::native_render::raster_policy(classified.material);
    std::printf("gmse01_boot:   frame %llu draw %llu: %s %s/%s/depth-write=%d %zu vertex(es) from "
                "0x%08llx, %u texture(s)%s\n",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(ordinal),
                sb::native_render::j3d_material_family_name(classified.family),
                sb::native_render::model_blend_mode_name(policy.blend),
                sb::native_render::model_alpha_test_name(policy.alphaTest),
                static_cast<int>(policy.depthWrite), vertices.size(),
                static_cast<unsigned long long>(mesh),
                static_cast<unsigned>(classified.textureCount),
                describe_first_texture(classified, images).c_str());
    std::printf(
        "gmse01_boot:    %s%s%s\n", describe_tex_gen(texGen).c_str(),
        describe_coordinate_range(triangles, texGen.recognised ? texGen.texGenCount : 0).c_str(),
        describe_resolved_color(draw, vertices).c_str());
}

} // namespace sunbright::gcnport_boot

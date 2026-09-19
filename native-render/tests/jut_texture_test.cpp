// Drives the shared JUTTexture decode policy both runtimes reach.
//
// What is under test is the policy, not the pixel maths: which formats are owned, which samplers
// are legal, when a palette is required, and what happens when the bytes handed in are shorter
// than the extent claims. Those are the decisions a second copy of this would be free to make
// differently, so each one has a case that pins it -- including the refusals, because a decode
// that silently accepted a short span would read whatever followed it and produce an image that
// looks decoded.

#include <sunbright/native_render/jut_texture.h>

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

using sb::native_render::AddressMode;
using sb::native_render::decode_jut_texture;
using sb::native_render::DecodedTexture;
using sb::native_render::EncodedImageFormat;
using sb::native_render::FilterMode;
using sb::native_render::JutTextureError;
using sb::native_render::JutTextureFields;
using sb::native_render::JutTexturePlan;
using sb::native_render::MipFilter;
using sb::native_render::PaletteFormat;
using sb::native_render::plan_jut_texture;

// GX numbers a linear, unmipped minification filter 1 and clamp/repeat/mirror 0/1/2.
JutTextureFields rgb5a3(std::uint32_t width, std::uint32_t height) {
    return {.format = 5,
            .alphaEnabled = true,
            .width = width,
            .height = height,
            .wrapS = 1,
            .wrapT = 2,
            .minFilter = 1,
            .magFilter = 1};
}

void plans_what_a_layer_will_consume() {
    JutTexturePlan plan{};
    assert(plan_jut_texture(rgb5a3(64, 32), plan) == JutTextureError::None);
    assert(plan.format == EncodedImageFormat::Rgb5A3);
    assert(!plan.indexed);
    assert(plan.encodedBytes == 64U * 32U * 2U);
    assert(plan.paletteBytes == 0);
    assert(plan.decodedBytes == 64U * 32U * 4U);
    assert(plan.texture.width == 64 && plan.texture.height == 32);
    assert(plan.texture.addressU == AddressMode::Repeat);
    assert(plan.texture.addressV == AddressMode::Mirror);
    assert(plan.texture.minFilter == FilterMode::Linear);
    assert(plan.texture.magFilter == FilterMode::Linear);
    assert(plan.texture.mipFilter == MipFilter::None);
    assert(plan.texture.hasAlpha);
}

void plans_the_palette_an_indexed_layer_needs() {
    JutTextureFields fields = rgb5a3(8, 8);
    fields.format = 9; // Indexed8
    fields.hasPalette = true;
    fields.paletteFormat = 2;
    fields.paletteEntries = 256;

    JutTexturePlan plan{};
    assert(plan_jut_texture(fields, plan) == JutTextureError::None);
    assert(plan.indexed);
    assert(plan.paletteFormat == PaletteFormat::Rgb5A3);
    assert(plan.encodedBytes == 8U * 8U);
    assert(plan.paletteBytes == 512);

    fields.hasPalette = false;
    assert(plan_jut_texture(fields, plan) == JutTextureError::MissingPalette);
    fields.hasPalette = true;
    fields.paletteFormat = 7;
    assert(plan_jut_texture(fields, plan) == JutTextureError::UnsupportedPaletteFormat);
}

void refuses_what_it_cannot_sample() {
    JutTexturePlan plan{};
    JutTextureFields fields = rgb5a3(0, 8);
    assert(plan_jut_texture(fields, plan) == JutTextureError::EmptyExtent);

    fields = rgb5a3(8, 8);
    fields.wrapS = 9;
    assert(plan_jut_texture(fields, plan) == JutTextureError::UnsupportedSampler);

    fields = rgb5a3(8, 8);
    fields.minFilter = 5; // linear, with a linear mip filter
    assert(plan_jut_texture(fields, plan) == JutTextureError::MipmappedNotImplemented);

    fields = rgb5a3(8, 8);
    fields.format = 7;
    assert(plan_jut_texture(fields, plan) == JutTextureError::UnsupportedFormat);
}

void decodes_a_layer_and_gives_it_a_content_revision() {
    const JutTextureFields fields = rgb5a3(4, 4);
    JutTexturePlan plan{};
    assert(plan_jut_texture(fields, plan) == JutTextureError::None);

    std::vector<std::uint8_t> encoded(plan.encodedBytes, 0);
    // One opaque red texel in RGB5A3's opaque encoding, so the decode is checked and not just run.
    encoded[0] = 0xFC;
    encoded[1] = 0x00;

    DecodedTexture decoded{};
    assert(decode_jut_texture(fields, encoded, {}, decoded) == JutTextureError::None);
    assert(decoded.rgba8.size() == plan.decodedBytes);
    assert(decoded.rgba8[0] == 0xFF && decoded.rgba8[1] == 0x00 && decoded.rgba8[2] == 0x00 &&
           decoded.rgba8[3] == 0xFF);
    assert(decoded.texture.revision != 0);

    // Different bytes, different revision: a cache keyed by it must not reuse the earlier upload.
    DecodedTexture other{};
    encoded[0] = 0x80;
    assert(decode_jut_texture(fields, encoded, {}, other) == JutTextureError::None);
    assert(other.texture.revision != decoded.texture.revision);
}

void refuses_bytes_shorter_than_the_extent_claims() {
    const JutTextureFields fields = rgb5a3(8, 8);
    JutTexturePlan plan{};
    assert(plan_jut_texture(fields, plan) == JutTextureError::None);

    const std::vector<std::uint8_t> shortEncoded(plan.encodedBytes - 1, 0);
    DecodedTexture decoded{};
    assert(decode_jut_texture(fields, shortEncoded, {}, decoded) ==
           JutTextureError::EncodedBytesTooShort);
    assert(decoded.rgba8.empty());

    JutTextureFields indexed = fields;
    indexed.format = 9;
    indexed.hasPalette = true;
    indexed.paletteFormat = 2;
    indexed.paletteEntries = 16;
    const std::vector<std::uint8_t> pixels(8U * 8U, 0);
    const std::vector<std::uint8_t> shortPalette(31, 0);
    assert(decode_jut_texture(indexed, pixels, shortPalette, decoded) ==
           JutTextureError::PaletteBytesTooShort);
    assert(decoded.rgba8.empty());
}

} // namespace

int main() {
    plans_what_a_layer_will_consume();
    plans_the_palette_an_indexed_layer_needs();
    refuses_what_it_cannot_sample();
    decodes_a_layer_and_gives_it_a_content_revision();
    refuses_bytes_shorter_than_the_extent_claims();
    return 0;
}

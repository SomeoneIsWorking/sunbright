#include <sunbright/native_render/semantic_sink.h>

#include <sb_native_j2d.h>

#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/JDrama/JDRRect.hpp>
#include <JSystem/JUtility/JUTPalette.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

extern "C" void sb_native_solid_rectangle_submit(const void* rect, std::uint32_t rgba);

namespace {

int g_hostAllocationDepth = 0;

struct ReceivedImage {
    std::uint64_t resource = 0;
    std::uint64_t revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8{};
};

struct Receiver {
    std::size_t calls = 0;
    sb::native_render::PictureDraw draw{};
    sb::native_render::GlyphDraw glyph{};
    sb::native_render::SolidRectangleDraw solid{};
    std::array<ReceivedImage, 4> images{};
    std::size_t imageCount = 0;
};

bool receive(const sb::native_render::SemanticDraw& draw,
             std::span<const sb::native_render::DecodedImageView> images, void* context) {
    assert(g_hostAllocationDepth == 1);
    auto& receiver = *static_cast<Receiver*>(context);
    ++receiver.calls;
    if (const auto* picture = std::get_if<sb::native_render::PictureDraw>(&draw)) {
        receiver.draw = *picture;
    } else if (const auto* glyph = std::get_if<sb::native_render::GlyphDraw>(&draw)) {
        receiver.glyph = *glyph;
    } else {
        receiver.solid = std::get<sb::native_render::SolidRectangleDraw>(draw);
    }
    receiver.imageCount = images.size();
    for (std::size_t index = 0; index < images.size(); ++index) {
        const auto& source = images[index];
        receiver.images[index] = {source.resource,
                                  source.revision,
                                  source.width,
                                  source.height,
                                  {source.rgba8.begin(), source.rgba8.end()}};
    }
    return true;
}

void identity(Mtx matrix) {
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            matrix[row][column] = 0.0f;
    matrix[0][0] = 1.0f;
    matrix[1][1] = 1.0f;
    matrix[2][2] = 1.0f;
}

void initialize_texture(JUTTexture& texture, const ResTIMG& identity,
                        std::span<std::uint8_t> texels, std::uint32_t format, std::uint16_t width,
                        std::uint16_t height, bool alpha, JUTPalette* palette) {
    texture.mTexInfo = &identity;
    texture.mTexData = texels.data();
    texture.mEmbPalette = nullptr;
    texture.field_0x2c = palette;
    texture.mTlutName = 0;
    texture.mFormat = format;
    texture.mAlphaEnabled = alpha ? 1 : 0;
    texture.mWidth = width;
    texture.mHeight = height;
    texture.mWrapS = GX_CLAMP;
    texture.mWrapT = GX_CLAMP;
    texture.mMinFilter = GX_NEAR;
    texture.mMagFilter = GX_NEAR;
    texture.mMinLOD = 0;
    texture.mMaxLOD = 0;
    texture.mLODBias = 0;
    texture.mFlags = 0;
    texture.field_0x4c = nullptr;
    texture.unk50 = 0;
}

} // namespace

extern "C" void sb_host_alloc_push(void) {
    ++g_hostAllocationDepth;
}

extern "C" void sb_host_alloc_pop(void) {
    assert(g_hostAllocationDepth > 0);
    --g_hostAllocationDepth;
}

extern "C" [[noreturn]] void OSPanic(const char*, int, const char*, ...) {
    std::abort();
}

int main() {
    std::array<std::uint8_t, 32> indexedTexels{};
    std::array<std::uint8_t, 32> intensityTexels{};
    std::array<std::uint8_t, 32> paletteBytes{};
    indexedTexels[0] = 0xf0;
    intensityTexels[0] = 77;
    paletteBytes[30] = 64;
    paletteBytes[31] = 32;

    JUTPalette palette(GX_TLUT0, GX_TL_IA8, JUT_TRANSPARENCY_UNK1, 16, paletteBytes.data());
    ResTIMG identity0{};
    ResTIMG identity1{};
    JUTTexture texture0;
    JUTTexture texture1;
    initialize_texture(texture0, identity0, indexedTexels, GX_TF_C4, 8, 8, true, &palette);
    initialize_texture(texture1, identity1, intensityTexels, GX_TF_I8, 8, 4, true, nullptr);

    J2DPicture picture;
    picture.mBounds.set(0, 0, 8, 8);
    picture.mClipRect.set(1, 2, 7, 6);
    identity(picture.mGlobalMtx);
    picture.mColorAlpha = 128;
    picture.mTextureNum = 2;
    picture.mTextures[0] = &texture0;
    picture.mTextures[1] = &texture1;
    picture.mTextures[2] = nullptr;
    picture.mTextures[3] = nullptr;
    for (std::uint8_t& owned : picture.unkFD)
        owned = 0;
    picture.mPalette = nullptr;
    picture.mBinding = BIND15;
    picture.mMirror = MIRROR0;
    picture.mFlip = false;
    picture.mWrapmodeHor = J2DWrapmode_NONE;
    picture.mWrapmodeVer = J2DWrapmode_NONE;
    picture.mWhite.set(0xffffffff);
    picture.mBlack.set(0x00000000);
    for (auto& corner : picture.mCornerColor)
        corner.set(0xffffffff);
    picture.mBlendKonstColor.set(0x00000080);
    picture.mBlendKonstAlpha.set(0x00000040);

    Mtx parent{};
    identity(parent);
    Receiver receiver{};
    J2DOrthoGraph graph(10, 20, 320, 240);
    assert(!sb::native_render::has_semantic_sink());
    sb_native_picture_submit(&picture, &parent);
    assert(g_hostAllocationDepth == 0);
    assert(receiver.calls == 0);

    sb::native_render::SemanticSinkLease sinkLease;
    assert(sb::native_render::claim_semantic_sink({receive, &receiver}, sinkLease));
    sb_native_picture_context_push(&graph, 1);
    sb_native_picture_submit(&picture, &parent);
    assert(g_hostAllocationDepth == 0);
    assert(receiver.calls == 1);
    assert(receiver.draw.picture.material.textureCount == 2);
    assert((receiver.draw.canvas.viewport == sb::native_render::PixelRect{10, 20, 320, 240}));
    assert(receiver.draw.picture.clip.enabled);
    assert(receiver.draw.picture.clip.x == 1 && receiver.draw.picture.clip.y == 2);
    assert(receiver.draw.picture.clip.width == 6 && receiver.draw.picture.clip.height == 4);
    assert(receiver.imageCount == 2);
    assert(receiver.images[0].resource == reinterpret_cast<std::uintptr_t>(&identity0));
    assert(receiver.images[0].revision != 0);
    assert(receiver.images[0].width == 8 && receiver.images[0].height == 8);
    assert((std::array<std::uint8_t, 4>{receiver.images[0].rgba8[0], receiver.images[0].rgba8[1],
                                        receiver.images[0].rgba8[2], receiver.images[0].rgba8[3]} ==
            std::array<std::uint8_t, 4>{32, 32, 32, 64}));
    assert((std::array<std::uint8_t, 4>{receiver.images[1].rgba8[0], receiver.images[1].rgba8[1],
                                        receiver.images[1].rgba8[2], receiver.images[1].rgba8[3]} ==
            std::array<std::uint8_t, 4>{77, 77, 77, 77}));

    const std::uint64_t firstIndexedRevision = receiver.images[0].revision;
    const std::uint64_t firstIntensityRevision = receiver.images[1].revision;
    sb_native_picture_submit(&picture, &parent);
    assert(receiver.calls == 2);
    assert(receiver.images[0].revision == firstIndexedRevision);
    assert(receiver.images[1].revision == firstIntensityRevision);

    paletteBytes[31] = 33;
    sb_native_picture_submit(&picture, &parent);
    assert(receiver.calls == 3);
    assert(receiver.images[0].revision != firstIndexedRevision);
    assert(receiver.images[0].rgba8[0] == 33);
    assert(receiver.images[1].revision == firstIntensityRevision);
    assert(g_hostAllocationDepth == 0);

    // Immediate-mode J2DPicture::draw runs outside a J2DScreen scope. setup2D publishes its active
    // ortho graph, then the direct bridge consumes the position matrix the retained body built.
    sb_native_picture_context_pop();
    sb_native_picture_context_activate(&graph);
    Mtx directTransform{};
    identity(directTransform);
    directTransform[0][3] = 12.0f;
    directTransform[1][3] = 34.0f;
    sb_native_picture_submit_direct(&picture, &directTransform, 48, 20, 1, 0, 0);
    assert(receiver.calls == 4);
    assert(!receiver.draw.picture.clip.enabled);
    assert((receiver.draw.picture.positions ==
            std::array<sb::native_render::Vec2, 4>{
                sb::native_render::Vec2{12, 34}, sb::native_render::Vec2{60, 34},
                sb::native_render::Vec2{12, 54}, sb::native_render::Vec2{60, 54}}));
    assert((receiver.draw.picture.uv ==
            std::array<sb::native_render::Vec2, 4>{
                sb::native_render::Vec2{1, 0}, sb::native_render::Vec2{0, 0},
                sb::native_render::Vec2{1, 1}, sb::native_render::Vec2{0, 1}}));

    // Generic J2D filled-box control: drive the production-linked native-layout adapter with a
    // transform and four distinct colours. The coordinate values deliberately exceed signed 16-bit
    // range so this proves the retail GXPosition3s16 narrowing rather than merely matching floats.
    identity(graph.mPosMtx);
    graph.mPosMtx[0][3] = 5.0f;
    graph.mPosMtx[1][3] = -4.0f;
    graph.setColor(JUtility::TColor(0xff0000ff), JUtility::TColor(0x00ff00ff),
                   JUtility::TColor(0x0000ffff), JUtility::TColor(0xffffffff));
    JUTRect gradientFill(65537, -65534, 65546, 65556);
    sb_native_j2d_fill_box_submit(&graph, &gradientFill);
    assert(receiver.calls == 5);
    assert(receiver.imageCount == 0);
    assert(receiver.solid.rectangle.source ==
           sb::native_render::SolidRectangleSource::J2dGrafContextFillBox);
    assert(receiver.solid.rectangle.positions[0] == sb::native_render::Vec2(6.0f, -2.0f));
    assert(receiver.solid.rectangle.positions[3] == sb::native_render::Vec2(15.0f, 16.0f));
    assert(receiver.solid.rectangle.corner[2] == sb::native_render::color_from_rgba8(0x0000ffff));
    assert(receiver.solid.rectangle.corner[3] == sb::native_render::color_from_rgba8(0xffffffff));
    assert(receiver.solid.rectangle.clip.space ==
           sb::native_render::ClipCoordinateSpace::TargetPixels);
    assert(receiver.solid.rectangle.clip.x == 10 && receiver.solid.rectangle.clip.y == 19);

    JDrama::TRect fill(-107, 20, 747, 460);
    sb_native_solid_rectangle_submit(&fill, 0x10203080U);
    assert(receiver.calls == 6);
    assert(receiver.imageCount == 0);
    assert(receiver.solid.rectangle.positions[0] == sb::native_render::Vec2(-107.0f, 20.0f));
    assert(receiver.solid.rectangle.positions[3] == sb::native_render::Vec2(747.0f, 460.0f));
    const auto fillColor = receiver.solid.rectangle.corner[0];
    constexpr float kColorTolerance = 0.000001f;
    assert(std::abs(fillColor.r - 16.0f / 255.0f) < kColorTolerance);
    assert(std::abs(fillColor.g - 32.0f / 255.0f) < kColorTolerance);
    assert(std::abs(fillColor.b - 48.0f / 255.0f) < kColorTolerance);
    assert(std::abs(fillColor.a - 128.0f / 255.0f) < kColorTolerance);
    assert(g_hostAllocationDepth == 0);

    // Resource-font control: the decomp seam supplies the exact high-level glyph metrics and
    // selected encoded page; this adapter owns decode and semantic submission.
    sb_native_picture_context_push(&graph, 1);
    JUTRect textClip(2, 3, 8, 9);
    Mtx textTransform{};
    identity(textTransform);
    textTransform[0][3] = 5.0f;
    sb_native_text_context_push(&textClip, &textTransform);
    sb_native_font_remap(0x10203040, 0xa0b0c0d0);
    const SbNativeFontGlyph glyph{&picture,
                                  'A',
                                  1,
                                  10.0f,
                                  20.0f,
                                  8.0f,
                                  4.0f,
                                  8,
                                  4,
                                  3,
                                  1,
                                  1,
                                  6,
                                  8,
                                  0,
                                  0,
                                  0,
                                  intensityTexels.data(),
                                  8,
                                  4,
                                  GX_TF_I8,
                                  intensityTexels.size(),
                                  {0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff}};
    sb_native_font_glyph_submit(&glyph);
    assert(receiver.calls == 7);
    assert(receiver.glyph.glyph.code == 'A');
    assert(receiver.glyph.glyph.clip.enabled && receiver.glyph.glyph.clip.x == 2);
    assert(receiver.glyph.glyph.positions[0] == sb::native_render::Vec2(14.0f, 17.0f));
    assert(receiver.glyph.glyph.positions[3] == sb::native_render::Vec2(22.0f, 21.0f));
    assert(receiver.glyph.glyph.black == sb::native_render::color_from_rgba8(0x10203040));
    assert(receiver.glyph.glyph.white == sb::native_render::color_from_rgba8(0xa0b0c0d0));
    assert(receiver.imageCount == 1 && receiver.images[0].rgba8[0] == 77);
    assert(g_hostAllocationDepth == 0);
    sb_native_text_context_pop();
    sb_native_picture_context_pop();

    assert(sb::native_render::release_semantic_sink(sinkLease));
}

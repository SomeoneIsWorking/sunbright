#include <sunbright/native_render/picture_sink.h>

#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/JUtility/JUTPalette.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>

#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

extern "C" void sb_native_picture_submit(const void* picture, const void* parentMatrix);

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
    sb::native_render::PictureCommand command{};
    std::array<ReceivedImage, 4> images{};
    std::size_t imageCount = 0;
};

bool receive(const sb::native_render::PictureCommand& command,
             std::span<const sb::native_render::DecodedImageView> images, void* context) {
    assert(g_hostAllocationDepth == 1);
    auto& receiver = *static_cast<Receiver*>(context);
    ++receiver.calls;
    receiver.command = command;
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
    sb::native_render::set_picture_sink({});
    sb_native_picture_submit(&picture, &parent);
    assert(g_hostAllocationDepth == 0);
    assert(receiver.calls == 0);

    sb::native_render::set_picture_sink({receive, &receiver});
    sb_native_picture_submit(&picture, &parent);
    assert(g_hostAllocationDepth == 0);
    assert(receiver.calls == 1);
    assert(receiver.command.material.textureCount == 2);
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

    sb::native_render::set_picture_sink({});
}

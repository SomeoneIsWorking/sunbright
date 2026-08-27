#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/picture_sink.h>

#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <dolphin/os.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace {

sb::native_render::Color color(std::uint32_t rgba) noexcept {
    constexpr float kScale = 1.0f / 255.0f;
    return {static_cast<float>((rgba >> 24U) & 0xffU) * kScale,
            static_cast<float>((rgba >> 16U) & 0xffU) * kScale,
            static_cast<float>((rgba >> 8U) & 0xffU) * kScale,
            static_cast<float>(rgba & 0xffU) * kScale};
}

} // namespace

extern "C" void sb_native_picture_submit(const void* picturePointer, const void* matrixPointer) {
    if (!sb::native_render::has_picture_sink())
        return;
    const auto fail = [&](const char* reason) {
        OSPanic(__FILE__, __LINE__, "semantic J2DPicture capture failed: %s picture=%p matrix=%p",
                reason, picturePointer, matrixPointer);
    };
    if (picturePointer == nullptr || matrixPointer == nullptr) {
        fail("null input");
        return;
    }

    const auto& picture = *static_cast<const J2DPicture*>(picturePointer);
    const auto& parent = *static_cast<const Mtx*>(matrixPointer);
    if (picture.mTextureNum == 0 || picture.mTextureNum > 4 || picture.mTextures[0] == nullptr) {
        fail("invalid texture count or texture zero");
        return;
    }

    sb::native_render::PictureCommand command{};
    command.instance = reinterpret_cast<std::uintptr_t>(&picture);
    command.opacity = static_cast<float>(picture.mColorAlpha) / 255.0f;
    command.material.textureCount = picture.mTextureNum;
    command.material.black = color(static_cast<u32>(picture.mBlack));
    command.material.white = color(static_cast<u32>(picture.mWhite));
    for (std::size_t index = 0; index < command.corner.size(); ++index)
        command.corner[index] = color(static_cast<u32>(picture.mCornerColor[index]));

    for (std::size_t index = 0; index < picture.mTextureNum; ++index) {
        const JUTTexture* source = picture.mTextures[index];
        if (source == nullptr || source->mTexInfo == nullptr) {
            fail("missing texture metadata");
            return;
        }
        auto& texture = command.material.textures[index];
        texture.resource = reinterpret_cast<std::uintptr_t>(source->mTexInfo);
        texture.width = source->mWidth;
        texture.height = source->mHeight;
        if (!sb::native_render::decode_address_mode(source->mWrapS, texture.addressU) ||
            !sb::native_render::decode_address_mode(source->mWrapT, texture.addressV) ||
            !sb::native_render::decode_min_filter(source->mMinFilter, texture.minFilter,
                                                  texture.mipFilter) ||
            !sb::native_render::decode_mag_filter(source->mMagFilter, texture.magFilter)) {
            fail("unsupported sampler state");
            return;
        }
        texture.hasAlpha = source->mAlphaEnabled != 0;
        if (index != 0 &&
            (!sb::native_render::decode_blend_factor(static_cast<u32>(picture.mBlendKonstColor),
                                                     index, texture.colorMix) ||
             !sb::native_render::decode_blend_factor(static_cast<u32>(picture.mBlendKonstAlpha),
                                                     index, texture.alphaMix))) {
            fail("invalid blend-factor layer");
            return;
        }
    }

    sb::native_render::PictureLayout layout{};
    layout.width = picture.mBounds.getWidth();
    layout.height = picture.mBounds.getHeight();
    layout.textureWidth = command.material.textures[0].width;
    layout.textureHeight = command.material.textures[0].height;
    layout.binding = picture.mBinding;
    layout.mirror = picture.mMirror;
    layout.transpose = picture.mFlip;
    layout.horizontalWrap = picture.mWrapmodeHor;
    layout.verticalWrap = picture.mWrapmodeVer;
    std::copy_n(&parent[0][0], 12, layout.parentTransform.value.begin());
    std::copy_n(&picture.mGlobalMtx[0][0], 12, layout.globalTransform.value.begin());
    if (!sb::native_render::resolve_picture_layout(layout, command.positions, command.uv)) {
        fail("layout resolution refused the picture");
        return;
    }
    if (!sb::native_render::submit_picture(command))
        fail("sink rejected a validated command");
}

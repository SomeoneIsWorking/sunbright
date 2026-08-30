#include <sunbright/native_render/image.h>
#include <sunbright/native_render/image_decode.h>
#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/picture_context.h>
#include <sunbright/native_render/semantic_sink.h>

#include "host_allocation_scope.h"

#include <JSystem/J2D/J2DGrafContext.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/JUtility/JUTPalette.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <dolphin/os.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

constexpr std::size_t kContextScopeCapacity = 16;
sb::native_render::PictureContextStack g_pictureContexts{};
std::array<bool, kContextScopeCapacity> g_contextScopeHasValue{};
std::size_t g_contextScopeDepth = 0;

const sb::native_render::PictureContext* current_picture_context() noexcept {
    if (g_contextScopeDepth == 0 || !g_contextScopeHasValue[g_contextScopeDepth - 1])
        return nullptr;
    return g_pictureContexts.current();
}

} // namespace

extern "C" void sb_native_picture_context_push(const void* contextPointer, int clipEnabled) {
    if (g_contextScopeDepth == g_contextScopeHasValue.size()) {
        OSPanic(__FILE__, __LINE__, "semantic J2D context stack overflow");
        return;
    }

    bool pushed = false;
    if (!sb::native_render::has_semantic_sink()) {
        g_contextScopeHasValue[g_contextScopeDepth++] = false;
        return;
    }
    const auto* context = static_cast<const J2DGrafContext*>(contextPointer);
    if (context != nullptr && context->unk4 == 1) {
        const auto& graph = *static_cast<const J2DOrthoGraph*>(context);
        const JUTRect& logical = graph.getOrtho();
        const JUTRect& viewport = graph.mBounds;
        if (logical.getWidth() <= 0 || logical.getHeight() <= 0 || viewport.getWidth() <= 0 ||
            viewport.getHeight() <= 0) {
            OSPanic(__FILE__, __LINE__, "semantic J2D context has invalid extents");
            return;
        }
        const sb::native_render::PictureContext value{
            {.origin = {static_cast<float>(logical.x1), static_cast<float>(logical.y1)},
             .extent = {static_cast<float>(logical.getWidth()),
                        static_cast<float>(logical.getHeight())},
             .viewport = {viewport.x1, viewport.y1, static_cast<std::uint32_t>(viewport.getWidth()),
                          static_cast<std::uint32_t>(viewport.getHeight())}},
            clipEnabled != 0};
        pushed = g_pictureContexts.push(value);
        if (!pushed)
            OSPanic(__FILE__, __LINE__, "semantic J2D context rejected ortho graph");
    }
    g_contextScopeHasValue[g_contextScopeDepth++] = pushed;
}

extern "C" void sb_native_picture_context_pop(void) {
    if (g_contextScopeDepth == 0) {
        OSPanic(__FILE__, __LINE__, "semantic J2D context stack underflow");
        return;
    }
    const bool hadValue = g_contextScopeHasValue[--g_contextScopeDepth];
    if (hadValue && !g_pictureContexts.pop())
        OSPanic(__FILE__, __LINE__, "semantic J2D context value stack underflow");
}

extern "C" void sb_native_picture_submit(const void* picturePointer, const void* matrixPointer) {
    if (!sb::native_render::has_semantic_sink())
        return;
    const auto fail = [&](const char* reason) {
        OSPanic(__FILE__, __LINE__, "semantic J2DPicture capture failed: %s picture=%p matrix=%p",
                reason, picturePointer, matrixPointer);
    };
    if (picturePointer == nullptr || matrixPointer == nullptr) {
        fail("null input");
        return;
    }

    // This seam runs on the game thread, where ordinary new routes through JKR. Decoding owns host
    // vectors only for the duration of the atomic sink call, so route those allocations explicitly.
    const sb::HostAllocationScope hostAllocations;

    const auto& picture = *static_cast<const J2DPicture*>(picturePointer);
    const auto& parent = *static_cast<const Mtx*>(matrixPointer);
    const sb::native_render::PictureContext* context = current_picture_context();
    if (context == nullptr) {
        fail("missing or non-orthographic J2D screen context");
        return;
    }
    if (picture.mTextureNum == 0 || picture.mTextureNum > 4 || picture.mTextures[0] == nullptr) {
        fail("invalid texture count or texture zero");
        return;
    }

    sb::native_render::PictureCommand command{};
    command.instance = reinterpret_cast<std::uintptr_t>(&picture);
    command.opacity = static_cast<float>(picture.mColorAlpha) / 255.0f;
    command.material.textureCount = picture.mTextureNum;
    command.material.black = sb::native_render::color_from_rgba8(static_cast<u32>(picture.mBlack));
    command.material.white = sb::native_render::color_from_rgba8(static_cast<u32>(picture.mWhite));
    for (std::size_t index = 0; index < command.corner.size(); ++index)
        command.corner[index] =
            sb::native_render::color_from_rgba8(static_cast<u32>(picture.mCornerColor[index]));

    std::array<std::vector<std::uint8_t>, 4> decodedPixels{};
    std::array<sb::native_render::DecodedImageView, 4> images{};
    for (std::size_t index = 0; index < picture.mTextureNum; ++index) {
        const JUTTexture* source = picture.mTextures[index];
        if (source == nullptr || source->mTexInfo == nullptr || source->mTexData == nullptr ||
            source->mFormat > std::numeric_limits<std::uint8_t>::max()) {
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
        if (texture.mipFilter != sb::native_render::MipFilter::None) {
            fail("mipmapped semantic picture resource is not implemented");
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

        sb::native_render::EncodedImageFormat format{};
        std::size_t sourceBytes = 0;
        std::size_t outputBytes = 0;
        if (!sb::native_render::decode_image_format(static_cast<std::uint8_t>(source->mFormat),
                                                    format) ||
            !sb::native_render::encoded_image_data_size(source->mWidth, source->mHeight, format,
                                                        sourceBytes) ||
            !sb::native_render::decoded_image_data_size(source->mWidth, source->mHeight,
                                                        outputBytes)) {
            fail("unsupported texture encoding or extent");
            return;
        }

        sb::native_render::PaletteFormat paletteFormat = sb::native_render::PaletteFormat::Rgb5A3;
        std::uint32_t paletteEntries = 0;
        std::span<const std::uint8_t> palette{};
        if (format == sb::native_render::EncodedImageFormat::Indexed4 ||
            format == sb::native_render::EncodedImageFormat::Indexed8 ||
            format == sb::native_render::EncodedImageFormat::Indexed14) {
            const JUTPalette* activePalette = source->field_0x2c;
            if (activePalette == nullptr || activePalette->getColorTable() == nullptr ||
                !sb::native_render::decode_palette_format(activePalette->getFormat(),
                                                          paletteFormat)) {
                fail("missing or unsupported active palette");
                return;
            }
            paletteEntries = activePalette->getNumColors();
            palette = {reinterpret_cast<const std::uint8_t*>(activePalette->getColorTable()),
                       static_cast<std::size_t>(paletteEntries) * 2U};
        }

        const sb::native_render::EncodedImageView encoded{
            format,          source->mWidth,
            source->mHeight, {static_cast<const std::uint8_t*>(source->mTexData), sourceBytes},
            paletteFormat,   paletteEntries,
            palette,
        };
        decodedPixels[index].resize(outputBytes);
        const sb::native_render::ImageDecodeError decodeError =
            sb::native_render::decode_image_rgba8(encoded, decodedPixels[index]);
        if (decodeError != sb::native_render::ImageDecodeError::None ||
            !sb::native_render::image_content_revision(encoded, texture.revision)) {
            fail(sb::native_render::image_decode_error_name(decodeError));
            return;
        }
        images[index] = {texture.resource, texture.revision, texture.width, texture.height,
                         decodedPixels[index]};
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
    if (context->clipEnabled) {
        command.clip = {.enabled = true,
                        .x = picture.mClipRect.x1,
                        .y = picture.mClipRect.y1,
                        .width = static_cast<std::uint32_t>(picture.mClipRect.getWidth()),
                        .height = static_cast<std::uint32_t>(picture.mClipRect.getHeight())};
    }
    const sb::native_render::PictureDraw draw{context->canvas, command};
    if (!sb::native_render::submit_picture(draw, std::span(images).first(picture.mTextureNum)))
        fail("sink rejected a validated command");
}

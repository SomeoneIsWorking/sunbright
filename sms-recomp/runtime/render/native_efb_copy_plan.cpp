#include "native_efb_copy_plan.h"

#include <algorithm>

NativeEfbCopySource sbr_native_efb_copy_source(int x, int y, int width, int height, int targetWidth,
                                               int targetHeight) noexcept {
    if (width <= 0 || height <= 0 || targetWidth <= 0 || targetHeight <= 0)
        return {};

    const auto wideX = static_cast<long long>(x);
    const auto wideY = static_cast<long long>(y);
    const auto wideWidth = static_cast<long long>(width);
    const auto wideHeight = static_cast<long long>(height);
    const auto wideTargetWidth = static_cast<long long>(targetWidth);
    const auto wideTargetHeight = static_cast<long long>(targetHeight);
    const int left = static_cast<int>(std::clamp(wideX, 0LL, wideTargetWidth));
    const int top = static_cast<int>(std::clamp(wideY, 0LL, wideTargetHeight));
    const int right = static_cast<int>(std::clamp(wideX + wideWidth, 0LL, wideTargetWidth));
    const int bottom = static_cast<int>(std::clamp(wideY + wideHeight, 0LL, wideTargetHeight));
    if (right <= left || bottom <= top)
        return {};
    return {true, left, top, right - left, bottom - top};
}

NativeEfbCopyClear sbr_native_efb_copy_clear_from_bp(std::uint32_t copyExecute,
                                                     std::uint32_t clearAr, std::uint32_t clearGb,
                                                     std::uint32_t clearDepth, std::uint32_t zMode,
                                                     std::uint32_t colorMode) noexcept {
    constexpr float kColorScale = 1.0f / 255.0f;
    constexpr float kDepthScale = 1.0f / 16777215.0f;
    NativeEfbCopyClear clear{};
    clear.enabled = (copyExecute & (1u << 11)) != 0;
    clear.colorUpdate = (colorMode & (1u << 3)) != 0;
    clear.alphaUpdate = (colorMode & (1u << 4)) != 0;
    clear.depthUpdate = (zMode & (1u << 4)) != 0;
    clear.color[0] = static_cast<float>(clearAr & 0xFFu) * kColorScale;
    clear.color[1] = static_cast<float>((clearGb >> 8) & 0xFFu) * kColorScale;
    clear.color[2] = static_cast<float>(clearGb & 0xFFu) * kColorScale;
    clear.color[3] = static_cast<float>((clearAr >> 8) & 0xFFu) * kColorScale;
    clear.depth = static_cast<float>(clearDepth & 0xFFFFFFu) * kDepthScale;
    return clear;
}

NativeEfbCopyPlan sbr_native_efb_copy_plan(const NativeEfbCopyRequest& request, int targetWidth,
                                           int targetHeight) noexcept {
    NativeEfbCopyPlan plan{};
    plan.dest = request.dest;
    plan.source = sbr_native_efb_copy_source(request.sourceX, request.sourceY, request.sourceWidth,
                                             request.sourceHeight, targetWidth, targetHeight);
    plan.destWidth = request.destWidth;
    plan.destHeight = request.destHeight;
    plan.clear = request.clear;
    return plan;
}

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

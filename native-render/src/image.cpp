#include <sunbright/native_render/image.h>

#include <algorithm>
#include <limits>

namespace sb::native_render {

bool valid(const DecodedImageView& image) noexcept {
    if (image.resource == 0 || image.width == 0 || image.height == 0)
        return false;
    const std::uint64_t pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4U)
        return false;
    if (image.rgba8.size() != static_cast<std::size_t>(pixels * 4U))
        return false;
    std::uint32_t expectedWidth = image.width;
    std::uint32_t expectedHeight = image.height;
    for (const DecodedImageMipLevel& level : image.mipLevels) {
        expectedWidth = std::max(expectedWidth >> 1U, 1U);
        expectedHeight = std::max(expectedHeight >> 1U, 1U);
        const std::uint64_t levelPixels =
            static_cast<std::uint64_t>(expectedWidth) * expectedHeight;
        if (level.width != expectedWidth || level.height != expectedHeight ||
            levelPixels > std::numeric_limits<std::size_t>::max() / 4U ||
            level.rgba8.size() != static_cast<std::size_t>(levelPixels * 4U)) {
            return false;
        }
    }
    return true;
}

} // namespace sb::native_render

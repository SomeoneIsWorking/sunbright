#include "render_compare_metric.h"

#include <cstdio>
#include <vector>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

namespace {

std::vector<uint8_t> solid(int width, int height, uint8_t value) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 255);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = value;
        pixels[i + 1] = value;
        pixels[i + 2] = value;
    }
    return pixels;
}

std::vector<uint8_t> vertical_split(int width, int height, bool inverted) {
    std::vector<uint8_t> pixels = solid(width, height, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool bright = (x >= width / 2) != inverted;
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
            pixels[offset + 0] = bright ? 255 : 0;
            pixels[offset + 1] = bright ? 255 : 0;
            pixels[offset + 2] = bright ? 255 : 0;
        }
    }
    return pixels;
}

} // namespace

int main() {
    using sb::render_compare::evaluate_identity_control;
    using sb::render_compare::IdentityControlResult;
    using sb::render_compare::score_images;

    constexpr int kWidth = 16;
    constexpr int kHeight = 16;
    const auto flat = solid(kWidth, kHeight, 0);
    const auto flatScore = score_images(flat.data(), kWidth, kHeight, flat.data(), kWidth, kHeight);
    CHECK(!flatScore.edgeComparable);
    CHECK(!flatScore.lumaComparable);
    CHECK(evaluate_identity_control(flatScore) == IdentityControlResult::Deferred);

    const auto split = vertical_split(kWidth, kHeight, false);
    const auto identical =
        score_images(split.data(), kWidth, kHeight, split.data(), kWidth, kHeight);
    CHECK(identical.edgeComparable);
    CHECK(identical.lumaComparable);
    CHECK(identical.edgeIou > 99.999);
    CHECK(identical.lumaCorrelation > 0.999);
    CHECK(evaluate_identity_control(identical) == IdentityControlResult::Passed);

    // Known-different control: the same edge is present, but its black/white sides are inverted.
    // Edge IoU alone still reads 100, so the luma leg must visibly reject the identity claim.
    const auto inverted = vertical_split(kWidth, kHeight, true);
    const auto different =
        score_images(split.data(), kWidth, kHeight, inverted.data(), kWidth, kHeight);
    CHECK(different.edgeComparable);
    CHECK(different.lumaComparable);
    CHECK(different.edgeIou > 99.999);
    CHECK(different.lumaCorrelation < -0.999);
    CHECK(evaluate_identity_control(different) == IdentityControlResult::Failed);
    return 0;
}

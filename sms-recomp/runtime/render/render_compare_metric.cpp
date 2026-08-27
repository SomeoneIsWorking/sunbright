#include "render_compare_metric.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <vector>

namespace sb::render_compare {
namespace {

constexpr int kGridWidth = 320;
constexpr int kGridHeight = 224;
constexpr float kEdgeFraction = 0.15f;
constexpr double kIdentityTolerance = 0.001;

void to_grid_luma(const uint8_t* source, int sourceWidth, int sourceHeight,
                  std::vector<float>& output) {
    output.resize(kGridWidth * kGridHeight);
    for (int y = 0; y < kGridHeight; ++y) {
        const int sourceY = static_cast<int>(static_cast<int64_t>(y) * sourceHeight / kGridHeight);
        for (int x = 0; x < kGridWidth; ++x) {
            const int sourceX =
                static_cast<int>(static_cast<int64_t>(x) * sourceWidth / kGridWidth);
            const uint8_t* pixel =
                source + (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4;
            output[y * kGridWidth + x] =
                0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
        }
    }
}

void edge_mask(const std::vector<float>& luma, std::vector<uint8_t>& mask) {
    std::vector<float> magnitude(static_cast<size_t>(kGridWidth) * kGridHeight, 0.0f);
    for (int y = 1; y < kGridHeight - 1; ++y) {
        for (int x = 1; x < kGridWidth - 1; ++x) {
            const auto sample = [&](int dx, int dy) {
                return luma[(y + dy) * kGridWidth + (x + dx)];
            };
            const float gradientX = -sample(-1, -1) - 2 * sample(-1, 0) - sample(-1, 1) +
                                    sample(1, -1) + 2 * sample(1, 0) + sample(1, 1);
            const float gradientY = -sample(-1, -1) - 2 * sample(0, -1) - sample(1, -1) +
                                    sample(-1, 1) + 2 * sample(0, 1) + sample(1, 1);
            magnitude[y * kGridWidth + x] =
                std::sqrt(gradientX * gradientX + gradientY * gradientY);
        }
    }
    std::vector<float> sorted = magnitude;
    const size_t thresholdIndex =
        static_cast<size_t>((1.0f - kEdgeFraction) * static_cast<float>(sorted.size()));
    std::nth_element(sorted.begin(), sorted.begin() + thresholdIndex, sorted.end());
    const float threshold = std::max(sorted[thresholdIndex], 1.0f);
    mask.assign(magnitude.size(), 0);
    for (size_t i = 0; i < magnitude.size(); ++i) {
        mask[i] = magnitude[i] >= threshold ? 1 : 0;
    }
}

double pearson(const std::vector<float>& first, const std::vector<float>& second,
               bool& comparable) {
    double firstMean = 0.0;
    double secondMean = 0.0;
    for (size_t i = 0; i < first.size(); ++i) {
        firstMean += first[i];
        secondMean += second[i];
    }
    firstMean /= static_cast<double>(first.size());
    secondMean /= static_cast<double>(second.size());

    double numerator = 0.0;
    double firstVariance = 0.0;
    double secondVariance = 0.0;
    for (size_t i = 0; i < first.size(); ++i) {
        const double firstCentered = first[i] - firstMean;
        const double secondCentered = second[i] - secondMean;
        numerator += firstCentered * secondCentered;
        firstVariance += firstCentered * firstCentered;
        secondVariance += secondCentered * secondCentered;
    }
    comparable = firstVariance > 0.0 && secondVariance > 0.0;
    if (!comparable) {
        return 0.0;
    }
    return numerator / std::sqrt(firstVariance * secondVariance);
}

} // namespace

MetricScore score_images(const uint8_t* firstRgba, int firstWidth, int firstHeight,
                         const uint8_t* secondRgba, int secondWidth, int secondHeight) {
    if (firstRgba == nullptr || secondRgba == nullptr || firstWidth <= 0 || firstHeight <= 0 ||
        secondWidth <= 0 || secondHeight <= 0) {
        std::abort();
    }

    std::vector<float> firstLuma;
    std::vector<float> secondLuma;
    to_grid_luma(firstRgba, firstWidth, firstHeight, firstLuma);
    to_grid_luma(secondRgba, secondWidth, secondHeight, secondLuma);

    std::vector<uint8_t> firstEdges;
    std::vector<uint8_t> secondEdges;
    edge_mask(firstLuma, firstEdges);
    edge_mask(secondLuma, secondEdges);
    long intersection = 0;
    long edgeUnion = 0;
    for (size_t i = 0; i < firstEdges.size(); ++i) {
        if (firstEdges[i] || secondEdges[i]) {
            ++edgeUnion;
        }
        if (firstEdges[i] && secondEdges[i]) {
            ++intersection;
        }
    }

    MetricScore score;
    score.edgeComparable = edgeUnion > 0;
    score.edgeIou = score.edgeComparable
                        ? 100.0 * static_cast<double>(intersection) / static_cast<double>(edgeUnion)
                        : 0.0;
    score.lumaCorrelation = pearson(firstLuma, secondLuma, score.lumaComparable);
    return score;
}

IdentityControlResult evaluate_identity_control(const MetricScore& score) noexcept {
    if (!score.edgeComparable || !score.lumaComparable) {
        return IdentityControlResult::Deferred;
    }
    const bool edgeIdentity = std::abs(score.edgeIou - 100.0) <= kIdentityTolerance;
    const bool lumaIdentity = std::abs(score.lumaCorrelation - 1.0) <= kIdentityTolerance;
    return edgeIdentity && lumaIdentity ? IdentityControlResult::Passed
                                        : IdentityControlResult::Failed;
}

} // namespace sb::render_compare

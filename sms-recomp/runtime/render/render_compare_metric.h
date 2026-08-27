#pragma once

#include <cstdint>

namespace sb::render_compare {

struct MetricScore {
    double edgeIou = 0.0;
    double lumaCorrelation = 0.0;
    bool edgeComparable = false;
    bool lumaComparable = false;
};

// Score two valid, tightly packed RGBA8 images after nearest-neighbour projection to the common
// comparison grid. Comparability is explicit: a flat image has neither an edge population nor
// luma variance and therefore cannot validate the metric by agreeing with itself.
MetricScore score_images(const uint8_t* firstRgba, int firstWidth, int firstHeight,
                         const uint8_t* secondRgba, int secondWidth, int secondHeight);

enum class IdentityControlResult { Deferred, Passed, Failed };

// An identity control is meaningful only when both metrics have a denominator. A measurable
// identical input must produce the identity values; anything else is a visible failure.
IdentityControlResult evaluate_identity_control(const MetricScore& score) noexcept;

} // namespace sb::render_compare

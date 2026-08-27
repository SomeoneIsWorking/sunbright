#include "render_compare_join.h"

#include <array>
#include <cstdio>
#include <vector>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

using namespace sb::render_compare;

namespace {

constexpr std::array<uint8_t, 4> kRed{255, 0, 0, 255};
constexpr std::array<uint8_t, 4> kGreen{0, 255, 0, 255};
constexpr std::array<uint8_t, 4> kBlue{0, 0, 255, 255};

int delayed_out_of_order_callbacks_take_only_their_exact_frame() {
    FrameJoin join{4};
    CHECK(join.reserve(100) == JoinStatus::Accepted);
    CHECK(join.reserve(101) == JoinStatus::Accepted);
    CHECK(join.submit_baseline(100, kRed.data(), 1, 1, 1, 2, 3) == JoinStatus::Accepted);
    CHECK(join.submit_variant(100, 8, "control:no-op", kGreen.data(), 1, 1) ==
          JoinStatus::Accepted);
    CHECK(join.submit_baseline(101, kBlue.data(), 1, 1, 4, 5, 6) == JoinStatus::Accepted);

    // Callback first: retain the exact oracle until the producer has finished adding variants.
    CHECK(join.submit_oracle(100, kBlue.data(), 1, 1) == JoinStatus::Accepted);
    JoinedFrame earlier;
    CHECK(join.take_ready(100, earlier) == JoinStatus::AwaitingPeer);

    // Producer first: sealing does not consume until this frame's oracle arrives.
    CHECK(join.seal(101) == JoinStatus::Accepted);
    JoinedFrame later;
    CHECK(join.take_ready(101, later) == JoinStatus::AwaitingPeer);
    CHECK(join.submit_oracle(101, kGreen.data(), 1, 1) == JoinStatus::Accepted);
    CHECK(join.take_ready(101, later) == JoinStatus::Accepted);
    CHECK(later.frameId == 101u);
    CHECK(later.baseline.image.rgba == std::vector<uint8_t>(kBlue.begin(), kBlue.end()));
    CHECK(later.variants.empty());
    CHECK(later.oracle.rgba == std::vector<uint8_t>(kGreen.begin(), kGreen.end()));

    CHECK(join.seal(100) == JoinStatus::Accepted);
    CHECK(join.take_ready(100, earlier) == JoinStatus::Accepted);
    CHECK(earlier.frameId == 100u);
    CHECK(earlier.baseline.image.rgba == std::vector<uint8_t>(kRed.begin(), kRed.end()));
    CHECK(earlier.variants.size() == 1u);
    CHECK(earlier.variants[0].image.rgba == std::vector<uint8_t>(kGreen.begin(), kGreen.end()));
    return 0;
}

int unknown_and_replay_emission_callbacks_cannot_consume_another_frame() {
    FrameJoin join{2};
    CHECK(join.reserve(77) == JoinStatus::Accepted);
    CHECK(join.submit_baseline(77, kRed.data(), 1, 1, 0, 0, 0) == JoinStatus::Accepted);

    JoinedFrame output;
    // A replay emission has its own ID 78 and merely names source 77 in AuroraFrameSinkInfo.
    // Looking up the emission ID must not fall back to (and consume) the source frame.
    CHECK(join.submit_oracle(78, kBlue.data(), 1, 1) == JoinStatus::UnknownFrame);
    CHECK(join.pending() == 1u);
    CHECK(join.seal(77) == JoinStatus::Accepted);
    CHECK(join.submit_oracle(77, kBlue.data(), 1, 1) == JoinStatus::Accepted);
    CHECK(join.take_ready(77, output) == JoinStatus::Accepted);
    return 0;
}

int invalid_duplicates_capacity_and_missing_baseline_fail_closed() {
    FrameJoin join{1};
    CHECK(join.reserve(0) == JoinStatus::InvalidFrameId);
    CHECK(join.reserve(1) == JoinStatus::Accepted);
    CHECK(join.reserve(1) == JoinStatus::DuplicateFrame);
    CHECK(join.reserve(2) == JoinStatus::CapacityExceeded);
    CHECK(join.submit_variant(2, 1, "unknown", kRed.data(), 1, 1) == JoinStatus::UnknownFrame);
    CHECK(join.submit_variant(1, 1, "no-baseline", kRed.data(), 1, 1) ==
          JoinStatus::MissingBaseline);

    JoinedFrame output;
    CHECK(join.seal(1) == JoinStatus::Accepted);
    CHECK(join.submit_oracle(1, kBlue.data(), 1, 1) == JoinStatus::Accepted);
    CHECK(join.take_ready(1, output) == JoinStatus::MissingBaseline);
    CHECK(join.pending() == 0u);

    CHECK(join.reserve(2) == JoinStatus::Accepted);
    CHECK(join.submit_baseline(2, kRed.data(), 1, 1, 0, 0, 0) == JoinStatus::Accepted);
    CHECK(join.submit_baseline(2, kBlue.data(), 1, 1, 0, 0, 0) == JoinStatus::DuplicateBaseline);
    CHECK(join.submit_variant(2, 5, "first", kGreen.data(), 1, 1) == JoinStatus::Accepted);
    CHECK(join.submit_variant(2, 5, "duplicate", kBlue.data(), 1, 1) ==
          JoinStatus::DuplicateVariant);
    CHECK(join.submit_oracle(2, kBlue.data(), 1, 1) == JoinStatus::Accepted);
    CHECK(join.submit_oracle(2, kGreen.data(), 1, 1) == JoinStatus::DuplicateOracle);
    CHECK(join.seal(2) == JoinStatus::Accepted);
    CHECK(join.seal(2) == JoinStatus::AlreadySealed);
    CHECK(join.submit_variant(2, 6, "late", kBlue.data(), 1, 1) == JoinStatus::FrameSealed);

    CHECK(join.take_ready(2, output) == JoinStatus::Accepted);
    CHECK(output.baseline.image.rgba == std::vector<uint8_t>(kRed.begin(), kRed.end()));
    CHECK(output.variants.size() == 1u);
    CHECK(output.variants[0].image.rgba == std::vector<uint8_t>(kGreen.begin(), kGreen.end()));
    CHECK(join.take_ready(2, output) == JoinStatus::UnknownFrame);
    return 0;
}

int attribution_control_withholds_then_allows_an_identical_no_op() {
    Baseline baseline;
    baseline.image.rgba.assign(kRed.begin(), kRed.end());
    baseline.image.width = 1;
    baseline.image.height = 1;
    Variant control{
        .id = 8,
        .name = "control:no-op",
        .image = {.rgba = std::vector<uint8_t>(kRed.begin(), kRed.end()), .width = 1, .height = 1}};

    AttributionControl gate{8};
    CHECK(!gate.table_allowed());
    gate.observe(baseline, control);
    CHECK(gate.table_allowed());
    return 0;
}

int attribution_control_permanently_suppresses_after_one_changed_byte() {
    Baseline baseline;
    baseline.image.rgba.assign(kRed.begin(), kRed.end());
    baseline.image.width = 1;
    baseline.image.height = 1;
    Variant control{
        .id = 8,
        .name = "control:no-op",
        .image = {.rgba = std::vector<uint8_t>(kRed.begin(), kRed.end()), .width = 1, .height = 1}};
    control.image.rgba[1] ^= 1;

    AttributionControl gate{8};
    gate.observe(baseline, control);
    CHECK(gate.state() == ControlState::Failed);
    CHECK(!gate.table_allowed());

    control.image.rgba.assign(kRed.begin(), kRed.end());
    gate.observe(baseline, control);
    CHECK(!gate.table_allowed());
    return 0;
}

} // namespace

int main() {
    if (delayed_out_of_order_callbacks_take_only_their_exact_frame() != 0)
        return 1;
    if (unknown_and_replay_emission_callbacks_cannot_consume_another_frame() != 0)
        return 1;
    if (invalid_duplicates_capacity_and_missing_baseline_fail_closed() != 0)
        return 1;
    if (attribution_control_withholds_then_allows_an_identical_no_op() != 0)
        return 1;
    return attribution_control_permanently_suppresses_after_one_changed_byte();
}

#include "render_compare_join.h"

#include <algorithm>

namespace sb::render_compare {

const char* join_status_name(JoinStatus status) noexcept {
    switch (status) {
    case JoinStatus::Accepted:
        return "accepted";
    case JoinStatus::InvalidFrameId:
        return "invalid-frame-id";
    case JoinStatus::InvalidImage:
        return "invalid-image";
    case JoinStatus::CapacityExceeded:
        return "capacity-exceeded";
    case JoinStatus::DuplicateFrame:
        return "duplicate-frame";
    case JoinStatus::UnknownFrame:
        return "unknown-frame";
    case JoinStatus::FrameSealed:
        return "frame-sealed";
    case JoinStatus::DuplicateBaseline:
        return "duplicate-baseline";
    case JoinStatus::DuplicateVariant:
        return "duplicate-variant";
    case JoinStatus::DuplicateOracle:
        return "duplicate-oracle";
    case JoinStatus::AlreadySealed:
        return "already-sealed";
    case JoinStatus::AwaitingPeer:
        return "awaiting-peer";
    case JoinStatus::MissingBaseline:
        return "missing-baseline";
    }
    return "unknown-status";
}

bool FrameJoin::valid_image(const uint8_t* rgba, int width, int height) noexcept {
    return rgba != nullptr && width > 0 && height > 0;
}

JoinStatus FrameJoin::reserve(uint64_t frameId) {
    if (frameId == 0) {
        return JoinStatus::InvalidFrameId;
    }
    std::scoped_lock lock{mutex_};
    if (pending_.contains(frameId)) {
        return JoinStatus::DuplicateFrame;
    }
    if (pending_.size() >= capacity_) {
        return JoinStatus::CapacityExceeded;
    }
    pending_.try_emplace(frameId);
    return JoinStatus::Accepted;
}

JoinStatus FrameJoin::submit_baseline(uint64_t frameId, const uint8_t* rgba, int width, int height,
                                      uint8_t clearR, uint8_t clearG, uint8_t clearB) {
    if (frameId == 0) {
        return JoinStatus::InvalidFrameId;
    }
    if (!valid_image(rgba, width, height)) {
        return JoinStatus::InvalidImage;
    }
    std::scoped_lock lock{mutex_};
    const auto found = pending_.find(frameId);
    if (found == pending_.end()) {
        return JoinStatus::UnknownFrame;
    }
    PendingFrame& frame = found->second;
    if (frame.sealed) {
        return JoinStatus::FrameSealed;
    }
    if (frame.hasBaseline) {
        return JoinStatus::DuplicateBaseline;
    }
    frame.baseline.image.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    frame.baseline.image.width = width;
    frame.baseline.image.height = height;
    frame.baseline.clear[0] = clearR;
    frame.baseline.clear[1] = clearG;
    frame.baseline.clear[2] = clearB;
    frame.hasBaseline = true;
    return JoinStatus::Accepted;
}

JoinStatus FrameJoin::submit_variant(uint64_t frameId, int id, const char* name,
                                     const uint8_t* rgba, int width, int height) {
    if (frameId == 0) {
        return JoinStatus::InvalidFrameId;
    }
    if (!valid_image(rgba, width, height)) {
        return JoinStatus::InvalidImage;
    }
    std::scoped_lock lock{mutex_};
    const auto found = pending_.find(frameId);
    if (found == pending_.end()) {
        return JoinStatus::UnknownFrame;
    }
    PendingFrame& frame = found->second;
    if (frame.sealed) {
        return JoinStatus::FrameSealed;
    }
    if (!frame.hasBaseline) {
        return JoinStatus::MissingBaseline;
    }
    if (std::ranges::any_of(frame.variants,
                            [id](const Variant& variant) { return variant.id == id; })) {
        return JoinStatus::DuplicateVariant;
    }
    Variant variant;
    variant.id = id;
    variant.name = name != nullptr ? name : "?";
    variant.image.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    variant.image.width = width;
    variant.image.height = height;
    frame.variants.push_back(std::move(variant));
    return JoinStatus::Accepted;
}

JoinStatus FrameJoin::submit_oracle(uint64_t frameId, const uint8_t* rgba, int width, int height) {
    if (frameId == 0) {
        return JoinStatus::InvalidFrameId;
    }
    if (!valid_image(rgba, width, height)) {
        return JoinStatus::InvalidImage;
    }
    std::scoped_lock lock{mutex_};
    const auto found = pending_.find(frameId);
    if (found == pending_.end()) {
        return JoinStatus::UnknownFrame;
    }
    PendingFrame& frame = found->second;
    if (frame.hasOracle) {
        return JoinStatus::DuplicateOracle;
    }
    frame.oracle.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    frame.oracle.width = width;
    frame.oracle.height = height;
    frame.hasOracle = true;
    return JoinStatus::Accepted;
}

JoinStatus FrameJoin::seal(uint64_t frameId) {
    if (frameId == 0) {
        return JoinStatus::InvalidFrameId;
    }
    std::scoped_lock lock{mutex_};
    const auto found = pending_.find(frameId);
    if (found == pending_.end()) {
        return JoinStatus::UnknownFrame;
    }
    if (found->second.sealed) {
        return JoinStatus::AlreadySealed;
    }
    found->second.sealed = true;
    return JoinStatus::Accepted;
}

JoinStatus FrameJoin::take_ready(uint64_t frameId, JoinedFrame& output) {
    if (frameId == 0) {
        return JoinStatus::InvalidFrameId;
    }
    std::scoped_lock lock{mutex_};
    const auto found = pending_.find(frameId);
    if (found == pending_.end()) {
        return JoinStatus::UnknownFrame;
    }
    if (!found->second.sealed || !found->second.hasOracle) {
        return JoinStatus::AwaitingPeer;
    }
    PendingFrame frame = std::move(found->second);
    pending_.erase(found);
    if (!frame.hasBaseline) {
        return JoinStatus::MissingBaseline;
    }
    output.frameId = frameId;
    output.baseline = std::move(frame.baseline);
    output.variants = std::move(frame.variants);
    output.oracle = std::move(frame.oracle);
    return JoinStatus::Accepted;
}

size_t FrameJoin::pending() const {
    std::scoped_lock lock{mutex_};
    return pending_.size();
}

void AttributionControl::observe(const Baseline& baseline, const Variant& variant) {
    if (variant.id != variantId_ || state_ == ControlState::Failed) {
        return;
    }
    const bool identical = baseline.image.width == variant.image.width &&
                           baseline.image.height == variant.image.height &&
                           baseline.image.rgba == variant.image.rgba;
    state_ = identical ? ControlState::Passed : ControlState::Failed;
}

} // namespace sb::render_compare

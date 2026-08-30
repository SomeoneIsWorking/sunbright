#include <sunbright/native_render/semantic_frame_bridge.h>

namespace sb::native_render {

SemanticFrameBridge::~SemanticFrameBridge() {
    (void)deactivate();
}

bool SemanticFrameBridge::activate(SemanticFrameBridgeConfig config) noexcept {
    if (collector_.has_value())
        return fail("already active");
    if (config.targetWidth == 0 || config.targetHeight == 0)
        return fail("invalid target extent");
    if (has_semantic_sink())
        return fail("semantic sink already owned");

    collector_.emplace(config.limits);
    if (collector_->error() != SemanticFrameError::None) {
        const bool result = fail_collector(collector_->error());
        collector_.reset();
        return result;
    }
    config_ = config;
    lease_ = {};
    collecting_ = false;
    hasSealedFrame_ = false;
    error_ = "none";
    return true;
}

bool SemanticFrameBridge::deactivate() noexcept {
    if (collecting_ && !release_semantic_sink(lease_))
        return fail("semantic sink ownership lost");
    collector_.reset();
    sealedFrame_ = {};
    config_ = {};
    lease_ = {};
    collecting_ = false;
    hasSealedFrame_ = false;
    error_ = "none";
    return true;
}

bool SemanticFrameBridge::begin() noexcept {
    if (!collector_.has_value())
        return true;
    if (collecting_)
        return fail("frame already collecting");
    if (has_semantic_sink())
        return fail("semantic sink already owned");
    if (!collector_->begin(config_.targetWidth, config_.targetHeight, config_.clear))
        return fail_collector(collector_->error());
    if (!claim_semantic_sink(collector_->sink(), lease_)) {
        collector_->reset();
        return fail("semantic sink claim failed");
    }

    sealedFrame_ = {};
    hasSealedFrame_ = false;
    collecting_ = true;
    error_ = "none";
    return true;
}

bool SemanticFrameBridge::seal() noexcept {
    if (!collector_.has_value())
        return true;
    if (!collecting_)
        return fail("frame is not collecting");
    if (!owns_semantic_sink(lease_))
        return fail("semantic sink ownership lost");
    if (!release_semantic_sink(lease_))
        return fail("semantic sink release failed");

    lease_ = {};
    collecting_ = false;
    hasSealedFrame_ = false;
    if (!collector_->seal(sealedFrame_))
        return fail_collector(collector_->error());
    hasSealedFrame_ = true;
    ++sealedSequence_;
    error_ = "none";
    return true;
}

const SemanticFrame* SemanticFrameBridge::last_sealed_frame() const noexcept {
    return hasSealedFrame_ ? &sealedFrame_ : nullptr;
}

const char* SemanticFrameBridge::last_error() const noexcept {
    return error_;
}

bool SemanticFrameBridge::active() const noexcept {
    return collector_.has_value();
}

std::uint64_t SemanticFrameBridge::sealed_sequence() const noexcept {
    return sealedSequence_;
}

bool SemanticFrameBridge::fail(const char* error) noexcept {
    error_ = error;
    return false;
}

bool SemanticFrameBridge::fail_collector(SemanticFrameError error) noexcept {
    error_ = semantic_frame_error_name(error);
    return false;
}

SemanticFrameBridge& semantic_frame_bridge() noexcept {
    static SemanticFrameBridge bridge;
    return bridge;
}

} // namespace sb::native_render

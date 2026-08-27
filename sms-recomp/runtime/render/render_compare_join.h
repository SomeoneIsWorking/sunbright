#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sb::render_compare {

enum class JoinStatus {
    Accepted,
    InvalidFrameId,
    InvalidImage,
    CapacityExceeded,
    DuplicateFrame,
    UnknownFrame,
    FrameSealed,
    DuplicateBaseline,
    DuplicateVariant,
    DuplicateOracle,
    AlreadySealed,
    AwaitingPeer,
    MissingBaseline,
};

const char* join_status_name(JoinStatus status) noexcept;

struct Image {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

struct Baseline {
    Image image;
    uint8_t clear[3]{};
};

struct Variant {
    int id = 0;
    std::string name;
    Image image;
};

struct JoinedFrame {
    uint64_t frameId = 0;
    Baseline baseline;
    std::vector<Variant> variants;
    Image oracle;
};

// Bounded, exact-key rendezvous between native CPU readbacks and Aurora's delayed asynchronous
// frame sink. Nothing is paired by arrival order, and capacity pressure fails instead of evicting
// an older frame that may still be in flight.
class FrameJoin {
  public:
    explicit FrameJoin(size_t capacity) : capacity_(capacity) {}

    JoinStatus reserve(uint64_t frameId);
    JoinStatus submit_baseline(uint64_t frameId, const uint8_t* rgba, int width, int height,
                               uint8_t clearR, uint8_t clearG, uint8_t clearB);
    JoinStatus submit_variant(uint64_t frameId, int id, const char* name, const uint8_t* rgba,
                              int width, int height);
    JoinStatus submit_oracle(uint64_t frameId, const uint8_t* rgba, int width, int height);
    JoinStatus seal(uint64_t frameId);
    JoinStatus take_ready(uint64_t frameId, JoinedFrame& output);

    [[nodiscard]] size_t pending() const;

  private:
    struct PendingFrame {
        bool hasBaseline = false;
        bool hasOracle = false;
        bool sealed = false;
        Baseline baseline;
        std::vector<Variant> variants;
        Image oracle;
    };

    static bool valid_image(const uint8_t* rgba, int width, int height) noexcept;

    const size_t capacity_;
    mutable std::mutex mutex_;
    std::map<uint64_t, PendingFrame> pending_;
};

enum class ControlState { Waiting, Passed, Failed };

// The attribution table is publishable only after the named no-op variant has reproduced its
// exact baseline bytes. A later mismatch permanently revokes the table for the run.
class AttributionControl {
  public:
    explicit AttributionControl(int variantId) : variantId_(variantId) {}

    void observe(const Baseline& baseline, const Variant& variant);
    [[nodiscard]] ControlState state() const noexcept { return state_; }
    [[nodiscard]] bool table_allowed() const noexcept { return state_ == ControlState::Passed; }

  private:
    int variantId_;
    ControlState state_ = ControlState::Waiting;
};

} // namespace sb::render_compare

#pragma once

#include <sunbright/native_render/frame.h>
#include <sunbright/native_render/picture_sink.h>

#include <cstdint>
#include <optional>

namespace sb::native_render {

struct SemanticFrameBridgeConfig {
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    Color clear{0.0f, 0.0f, 0.0f, 1.0f};
    PictureFrameLimits limits{};
};

// Owns the process's one frame-scoped picture sink. Runtime frame seams call begin/seal even while
// inactive; host composition activates the bridge before the first begin when a semantic presenter
// is selected. Sealed storage remains valid until the next begin or deactivate.
class SemanticFrameBridge {
  public:
    ~SemanticFrameBridge();

    [[nodiscard]] bool activate(SemanticFrameBridgeConfig config) noexcept;
    [[nodiscard]] bool deactivate() noexcept;
    [[nodiscard]] bool begin() noexcept;
    [[nodiscard]] bool seal() noexcept;

    [[nodiscard]] const PictureFrame* last_sealed_frame() const noexcept;
    [[nodiscard]] const char* last_error() const noexcept;
    [[nodiscard]] bool active() const noexcept;

  private:
    [[nodiscard]] bool fail(const char* error) noexcept;
    [[nodiscard]] bool fail_collector(PictureFrameError error) noexcept;

    std::optional<PictureFrameCollector> collector_{};
    PictureFrame sealedFrame_{};
    SemanticFrameBridgeConfig config_{};
    PictureSinkLease lease_{};
    const char* error_ = "none";
    bool collecting_ = false;
    bool hasSealedFrame_ = false;
};

// Each runtime is a distinct process, so this is its one semantic-frame owner.
[[nodiscard]] SemanticFrameBridge& semantic_frame_bridge() noexcept;

} // namespace sb::native_render

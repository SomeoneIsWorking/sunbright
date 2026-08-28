#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/picture_sink.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sb::native_render {

// A sealed game-semantic frame. All spans remain valid until the collector that produced the frame
// begins or resets. This boundary contains no GX state, FIFO packets, guest pointers, or SDL types.
struct PictureFrame {
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    std::span<const PictureDraw> draws{};
    std::span<const DecodedImageView> images{};
    Color clear{};
};

struct PictureFrameLimits {
    std::size_t commands = 16'384;
    std::size_t images = 4'096;
    std::size_t decodedImageBytes = 256U * 1024U * 1024U;
};

enum class PictureFrameError : std::uint8_t {
    None,
    InvalidLimits,
    InvalidFrame,
    AlreadyCollecting,
    NotCollecting,
    InvalidSubmission,
    CommandLimit,
    ImageLimit,
    ImageByteLimit,
    ConflictingImage,
    AllocationFailure,
};

[[nodiscard]] const char* picture_frame_error_name(PictureFrameError error) noexcept;

// Owns the complete immutable input of one PC-native J2D picture frame. Runtime adapters may pass
// transient spans: append() copies every new decoded image before accepting its command. Images are
// coalesced only when resource, revision, dimensions, and content all agree.
class PictureFrameCollector {
  public:
    explicit PictureFrameCollector(PictureFrameLimits limits = {});

    PictureFrameCollector(const PictureFrameCollector&) = delete;
    PictureFrameCollector& operator=(const PictureFrameCollector&) = delete;

    [[nodiscard]] bool begin(std::uint32_t targetWidth, std::uint32_t targetHeight, Color clear);
    [[nodiscard]] PictureSink sink() noexcept;
    [[nodiscard]] bool append(const PictureDraw& draw, std::span<const DecodedImageView> images);
    [[nodiscard]] bool seal(PictureFrame& frame);
    void reset() noexcept;

    [[nodiscard]] PictureFrameError error() const noexcept;
    [[nodiscard]] std::size_t decoded_image_bytes() const noexcept;

  private:
    struct StoredImage {
        std::uint64_t resource = 0;
        std::uint64_t revision = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba8{};
    };

    enum class State : std::uint8_t { Idle, Collecting, Sealed };

    static bool receive(const PictureDraw& draw, std::span<const DecodedImageView> images,
                        void* context);
    [[nodiscard]] bool fail(PictureFrameError error) noexcept;

    PictureFrameLimits limits_{};
    std::uint32_t targetWidth_ = 0;
    std::uint32_t targetHeight_ = 0;
    Color clear_{};
    std::vector<PictureDraw> draws_{};
    std::vector<StoredImage> images_{};
    std::vector<DecodedImageView> imageViews_{};
    std::size_t decodedImageBytes_ = 0;
    PictureFrameError error_ = PictureFrameError::None;
    State state_ = State::Idle;
};

} // namespace sb::native_render

#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/model.h>
#include <sunbright/native_render/semantic_draw.h>
#include <sunbright/native_render/semantic_sink.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sb::native_render {

// A sealed game-semantic frame. All spans remain valid until the collector that produced the frame
// begins or resets. This boundary contains no GX state, FIFO packets, guest pointers, or SDL types.
struct SemanticFrame {
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    std::span<const SemanticDraw> draws{};
    std::span<const ModelDraw> models{};
    std::span<const MeshResourceView> meshes{};
    std::span<const DecodedImageView> images{};
    Color clear{};
};

struct SemanticFrameLimits {
    std::size_t commands = 16'384;
    std::size_t images = 4'096;
    std::size_t decodedImageBytes = 256U * 1024U * 1024U;
    std::size_t meshes = 4'096;
    std::size_t meshVertices = 8U * 1024U * 1024U;
};

enum class SemanticFrameError : std::uint8_t {
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
    MeshLimit,
    MeshVertexLimit,
    ConflictingMesh,
    AllocationFailure,
};

[[nodiscard]] const char* semantic_frame_error_name(SemanticFrameError error) noexcept;

// Owns the complete immutable input of one PC-native semantic frame. Runtime adapters may pass
// transient spans: image and mesh storage is copied before its command is accepted. Resources are
// coalesced only when identity, revision, dimensions/count, and content all agree.
class SemanticFrameCollector {
  public:
    explicit SemanticFrameCollector(SemanticFrameLimits limits = {});

    SemanticFrameCollector(const SemanticFrameCollector&) = delete;
    SemanticFrameCollector& operator=(const SemanticFrameCollector&) = delete;

    [[nodiscard]] bool begin(std::uint32_t targetWidth, std::uint32_t targetHeight, Color clear);
    [[nodiscard]] SemanticSink sink() noexcept;
    [[nodiscard]] bool append(const SemanticDraw& draw, std::span<const DecodedImageView> images);
    [[nodiscard]] bool append_picture(const PictureDraw& draw,
                                      std::span<const DecodedImageView> images);
    [[nodiscard]] bool append_glyph(const GlyphDraw& draw,
                                    std::span<const DecodedImageView> images);
    [[nodiscard]] bool append_solid_rectangle(const SolidRectangleDraw& draw);
    [[nodiscard]] bool append_model(const ModelDraw& draw, const MeshResourceView& mesh,
                                    std::span<const DecodedImageView> images = {});
    [[nodiscard]] bool seal(SemanticFrame& frame);
    void reset() noexcept;

    [[nodiscard]] SemanticFrameError error() const noexcept;
    [[nodiscard]] std::size_t decoded_image_bytes() const noexcept;

  private:
    struct StoredImage {
        std::uint64_t resource = 0;
        std::uint64_t revision = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba8{};
    };

    struct StoredMesh {
        std::uint64_t resource = 0;
        std::uint64_t revision = 0;
        std::vector<MeshVertex> vertices{};
    };

    enum class State : std::uint8_t { Idle, Collecting, Sealed };

    static bool receive(const SemanticDraw& draw, std::span<const DecodedImageView> images,
                        void* context);
    static bool receive_model(const ModelDraw& draw, const MeshResourceView& mesh,
                              std::span<const DecodedImageView> images, void* context);
    [[nodiscard]] bool append_textured(const SemanticDraw& draw, const Canvas& canvas,
                                       const PictureCommand& picture,
                                       std::span<const DecodedImageView> images);
    [[nodiscard]] bool fail(SemanticFrameError error) noexcept;

    SemanticFrameLimits limits_{};
    std::uint32_t targetWidth_ = 0;
    std::uint32_t targetHeight_ = 0;
    Color clear_{};
    std::vector<SemanticDraw> draws_{};
    std::vector<ModelDraw> models_{};
    std::vector<StoredMesh> meshes_{};
    std::vector<MeshResourceView> meshViews_{};
    std::vector<StoredImage> images_{};
    std::vector<DecodedImageView> imageViews_{};
    std::size_t decodedImageBytes_ = 0;
    std::size_t meshVertices_ = 0;
    SemanticFrameError error_ = SemanticFrameError::None;
    State state_ = State::Idle;
};

} // namespace sb::native_render

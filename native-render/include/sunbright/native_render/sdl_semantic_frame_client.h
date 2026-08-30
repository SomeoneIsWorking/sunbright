#pragma once

#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/sdl_gpu_frame_target.h>
#include <sunbright/native_render/semantic_2d_pass.h>
#include <sunbright/native_render/semantic_frame_bridge.h>

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sb::native_render {

enum class SemanticFrameAuditSetting : std::uint8_t { Disabled, Enabled, Invalid };

[[nodiscard]] SemanticFrameAuditSetting parse_semantic_frame_audit(const char* value) noexcept;

enum class SemanticReadbackMode : std::uint8_t { None, UntilNonClear, EveryFrame };

struct SdlSemanticFrameClientConfig {
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    SemanticReadbackMode readback = SemanticReadbackMode::UntilNonClear;
};

struct SdlSemanticFrameStats {
    std::uint64_t submittedFrames = 0;
    std::uint64_t completedFrames = 0;
    std::uint64_t nonEmptyFrames = 0;
    std::uint64_t mixedOperationFrames = 0;
    std::uint64_t submittedOperations = 0;
    std::uint64_t submittedPictures = 0;
    std::uint64_t submittedGlyphs = 0;
    std::uint64_t submittedSolidRectangles = 0;
    std::uint64_t submittedImages = 0;
    std::uint64_t sampledFrames = 0;
    std::uint64_t firstNonClearFrame = 0;
    std::uint64_t lastSampleHash = 0;
    std::size_t lastSampleNonClearPixels = 0;
    std::size_t firstNonClearPixels = 0;
};

// Offscreen SDL3 consumer for the process's sealed semantic 2D frame. It borrows the one
// process platform, owns only its target/pass/readback resources, and never claims or presents a
// window. Runtime composition must initialize it before the first bridge begin, consume once after
// each seal, and shut it down before the platform and Aurora.
class SdlSemanticFrameClient {
  public:
    SdlSemanticFrameClient() = default;
    ~SdlSemanticFrameClient();

    SdlSemanticFrameClient(const SdlSemanticFrameClient&) = delete;
    SdlSemanticFrameClient& operator=(const SdlSemanticFrameClient&) = delete;

    [[nodiscard]] bool initialize(SdlGpuPlatform& platform, SemanticFrameBridge& bridge,
                                  const SdlSemanticFrameClientConfig& config, std::string& error);
    [[nodiscard]] bool encode_last_sealed(std::string& error);
    // Stop accepting game commands before other runtime-owned frame/UI resources unwind. GPU
    // resources remain alive until shutdown(), which waits for the device before releasing them.
    [[nodiscard]] bool stop_collection(std::string& error) noexcept;
    // A bounded diagnostic run is evidence only if it completed every submission and the live
    // readback observed semantic output rather than the controlled black clear.
    [[nodiscard]] bool validate_audit(std::string& error) const noexcept;
    [[nodiscard]] bool shutdown(std::string& error) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const SdlSemanticFrameStats& stats() const noexcept;

  private:
    [[nodiscard]] bool should_read_back(const SemanticFrame& frame) const noexcept;
    [[nodiscard]] bool append_readback(SDL_GPUCommandBuffer* commandBuffer,
                                       const SemanticFrame& frame, std::string& error);
    [[nodiscard]] bool measure_readback(const SemanticFrame& frame, std::string& error) noexcept;
    void release_resources() noexcept;

    SdlGpuPlatform* platform_ = nullptr;
    SemanticFrameBridge* bridge_ = nullptr;
    SdlGpuFrameTarget target_{};
    std::unique_ptr<Semantic2dPass> pass_{};
    SDL_GPUTransferBuffer* readback_ = nullptr;
    SdlSemanticFrameClientConfig config_{};
    SdlSemanticFrameStats stats_{};
    std::uint64_t consumedSequence_ = 0;
    bool active_ = false;
};

[[nodiscard]] SdlSemanticFrameClient& sdl_semantic_frame_client() noexcept;

} // namespace sb::native_render

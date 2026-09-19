#pragma once

#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/sdl_gpu_frame_target.h>
#include <sunbright/native_render/semantic_2d_pass.h>
#include <sunbright/native_render/semantic_3d_pass.h>
#include <sunbright/native_render/semantic_frame_bridge.h>

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace sb::native_render {

// Which frames come back off the GPU. A readback is a full-target download and a stall, so a run
// that wants one image pays for it once rather than on every frame before it: `NamedFrame` is what
// separates "which frame do I want" from "how many frames must I pay for".
enum class SemanticReadbackMode : std::uint8_t { None, UntilNonClear, NamedFrame, EveryFrame };

// One sampled frame's pixels, offered to a consumer that wants to keep the image rather than only
// the measurement taken from it. RGBA8, tightly packed, top row first, already in the target's sRGB
// encoding. `rgba8` is the mapped readback buffer and is valid only for the duration of the call.
struct SemanticFrameSample {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Which frame this is, in the same numbering `SdlSemanticFrameStats::firstNonClearFrame` uses.
    std::uint64_t frameIndex = 0;
    std::size_t nonClearPixels = 0;
    std::span<const std::uint8_t> rgba8{};
};

// Answers false to refuse the sample, which fails the encode rather than losing it quietly: a
// consumer that could not write the image it asked for has not observed the frame.
using SemanticSampleObserver = bool (*)(const SemanticFrameSample& sample, void* context,
                                        std::string& error);

struct SdlSemanticFrameClientConfig {
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    SemanticReadbackMode readback = SemanticReadbackMode::UntilNonClear;
    // With `NamedFrame`, the one frame to read back, numbered as `SemanticFrameSample::frameIndex`
    // is -- so the first frame is 1. Required by that mode and ignored by the others; zero with
    // `NamedFrame` is refused rather than read as "never", which would be a run that asked for an
    // image and could not have taken one.
    std::uint64_t readbackFrame = 0;
    // Null measures each sample and discards its pixels, which is what an audit run wants.
    SemanticSampleObserver onSample = nullptr;
    void* onSampleContext = nullptr;
    // A non-null window selects the deliberately incomplete visible semantic preview. Null keeps
    // the semantic target offscreen for audit while Aurora presents its retained GX reference.
    SDL_Window* presentationWindow = nullptr;
};

struct SdlSemanticFrameStats {
    std::uint64_t submittedFrames = 0;
    std::uint64_t completedFrames = 0;
    std::uint64_t nonEmptyFrames = 0;
    std::uint64_t mixedOperationFrames = 0;
    std::uint64_t submittedOperations = 0;
    std::uint64_t submittedPictures = 0;
    std::uint64_t submittedJ2dWindowPictures = 0;
    std::uint64_t submittedGlyphs = 0;
    std::uint64_t submittedSolidRectangles = 0;
    std::uint64_t submittedJ2dFillBoxes = 0;
    std::uint64_t submittedJ2dWindowContents = 0;
    std::uint64_t submittedImages = 0;
    std::uint64_t submittedModels = 0;
    std::uint64_t submittedMeshes = 0;
    std::uint64_t submittedMeshVertices = 0;
    std::uint64_t sampledFrames = 0;
    std::uint64_t presentedFrames = 0;
    std::uint64_t windowUnavailableFrames = 0;
    std::uint64_t firstNonClearFrame = 0;
    std::uint64_t lastSampleHash = 0;
    std::size_t lastSampleNonClearPixels = 0;
    std::size_t firstNonClearPixels = 0;
};

// SDL3 consumer for the process's sealed semantic frame. It borrows the one process platform and
// owns its target/pass/readback resources. Audit mode stays offscreen; explicit preview mode
// attaches the platform's sole presenter and shows this incomplete target. Runtime composition
// must initialize it before the first bridge begin, consume once after each seal, and shut it down
// before the platform and Aurora.
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
    [[nodiscard]] bool validate_output(std::string& error) const noexcept;
    [[nodiscard]] bool shutdown(std::string& error) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const SdlSemanticFrameStats& stats() const noexcept;

  private:
    [[nodiscard]] bool cancel_encode(SDL_GPUCommandBuffer* commandBuffer,
                                     std::string& error) noexcept;
    [[nodiscard]] bool should_read_back(const SemanticFrame& frame) const noexcept;
    [[nodiscard]] bool append_readback(SDL_GPUCommandBuffer* commandBuffer,
                                       const SemanticFrame& frame, std::string& error);
    [[nodiscard]] bool measure_readback(const SemanticFrame& frame, std::string& error) noexcept;
    void release_resources() noexcept;

    SdlGpuPlatform* platform_ = nullptr;
    SemanticFrameBridge* bridge_ = nullptr;
    SdlGpuFrameTarget target_{};
    std::unique_ptr<Semantic3dPass> pass3d_{};
    std::unique_ptr<Semantic2dPass> pass_{};
    SDL_GPUTransferBuffer* readback_ = nullptr;
    SdlSemanticFrameClientConfig config_{};
    SdlSemanticFrameStats stats_{};
    std::uint64_t consumedSequence_ = 0;
    bool active_ = false;
};

[[nodiscard]] SdlSemanticFrameClient& sdl_semantic_frame_client() noexcept;

} // namespace sb::native_render

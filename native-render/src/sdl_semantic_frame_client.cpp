#include <sunbright/native_render/sdl_semantic_frame_client.h>

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>

namespace sb::native_render {
namespace {

constexpr auto kFenceTimeout = std::chrono::seconds(5);

std::uint64_t hash_bytes(const std::uint8_t* bytes, std::size_t size) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool byte_count(std::uint32_t width, std::uint32_t height, std::size_t& result) noexcept {
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * 4U;
    if (bytes == 0 || bytes > std::numeric_limits<Uint32>::max())
        return false;
    result = static_cast<std::size_t>(bytes);
    return true;
}

} // namespace

SdlSemanticFrameClient::~SdlSemanticFrameClient() {
    if (active_)
        std::terminate();
}

bool SdlSemanticFrameClient::initialize(SdlGpuPlatform& platform, SemanticFrameBridge& bridge,
                                        const SdlSemanticFrameClientConfig& config,
                                        std::string& error) {
    error.clear();
    if (active_) {
        error = "semantic SDL frame client is already active";
        return false;
    }
    std::size_t readbackBytes = 0;
    if (!platform.ready() || !byte_count(config.width, config.height, readbackBytes)) {
        error = "semantic SDL frame client has an invalid platform or extent";
        return false;
    }
    if (config.presentationWindow != nullptr && (SDL_GetWindowFlags(config.presentationWindow) &
                                                 (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0) {
        error = "native 2D preview requires a visible, non-minimized SDL window";
        return false;
    }

    platform_ = &platform;
    bridge_ = &bridge;
    config_ = config;
    stats_ = {};
    consumedSequence_ = bridge.sealed_sequence();
    const SdlGpuFrameTargetDesc targetDesc{
        .width = config.width, .height = config.height, .hasDepth = true};
    if (!target_.initialize(platform, targetDesc, error)) {
        release_resources();
        return false;
    }
    pass3d_ = std::make_unique<Semantic3dPass>(platform.device());
    pass_ = std::make_unique<Semantic2dPass>(platform.device());
    if (!pass3d_->initialize(error) || !pass_->initialize(error)) {
        release_resources();
        return false;
    }
    if (config.readback != SemanticReadbackMode::None) {
        const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                   static_cast<Uint32>(readbackBytes), 0};
        readback_ = SDL_CreateGPUTransferBuffer(platform.device(), &info);
        if (readback_ == nullptr) {
            error = std::string("semantic readback allocation failed: ") + SDL_GetError();
            release_resources();
            return false;
        }
    }
    if (config.presentationWindow != nullptr &&
        !platform.attach_presenter(config.presentationWindow, {}, error)) {
        release_resources();
        return false;
    }
    // The live readback instrument has one invariant clear value, independently controlled by the
    // empty-frame GPU test. Keeping it fixed prevents a caller-supplied clear from being mistaken
    // for semantic picture output.
    if (!bridge.activate({.targetWidth = config.width,
                          .targetHeight = config.height,
                          .clear = {0.0f, 0.0f, 0.0f, 1.0f}})) {
        error = std::string("semantic frame bridge activation failed: ") + bridge.last_error();
        release_resources();
        return false;
    }
    active_ = true;
    return true;
}

bool SdlSemanticFrameClient::encode_last_sealed(std::string& error) {
    error.clear();
    if (!ready()) {
        error = "semantic SDL frame client is not active";
        return false;
    }
    const SemanticFrame* frame = bridge_->last_sealed_frame();
    const std::uint64_t sequence = bridge_->sealed_sequence();
    if (frame == nullptr || sequence == consumedSequence_) {
        error = frame == nullptr ? "semantic frame bridge has no sealed frame"
                                 : "semantic sealed frame was already consumed";
        return false;
    }
    if (frame->targetWidth != config_.width || frame->targetHeight != config_.height) {
        error = "semantic sealed-frame extent differs from its GPU target";
        return false;
    }

    SDL_GPUCommandBuffer* commandBuffer = platform_->acquire_command_buffer(error);
    if (commandBuffer == nullptr)
        return false;
    const Semantic3dPassTarget pass3dTarget{commandBuffer, target_.color(),
                                            target_.desc().colorFormat, target_.depth(),
                                            target_.desc().depthFormat};
    if (!pass3d_->encode(*frame, pass3dTarget, error)) {
        std::string completionError;
        (void)pass3d_->complete_encode(false, completionError);
        (void)platform_->cancel_command_buffer(commandBuffer, completionError);
        return false;
    }
    const Semantic2dPassTarget passTarget{commandBuffer, target_.color(),
                                          target_.desc().colorFormat, SDL_GPU_LOADOP_LOAD,
                                          SDL_GPU_STOREOP_STORE};
    if (!pass_->encode(*frame, passTarget, error)) {
        (void)cancel_encode(commandBuffer, error);
        return false;
    }

    const bool sample = should_read_back(*frame);
    if (sample && !append_readback(commandBuffer, *frame, error)) {
        (void)cancel_encode(commandBuffer, error);
        return false;
    }

    PresentResult presentResult = PresentResult::WindowUnavailable;
    const bool presentationRequested = config_.presentationWindow != nullptr;
    if (presentationRequested) {
        presentResult = platform_->encode_present(commandBuffer, target_.color(), config_.width,
                                                  config_.height, error);
        if (presentResult == PresentResult::Failed) {
            (void)cancel_encode(commandBuffer, error);
            return false;
        }
    }

    SDL_GPUFence* fence = platform_->submit_and_acquire_fence(commandBuffer, error);
    if (fence == nullptr) {
        std::string completionError;
        if (!pass3d_->complete_encode(false, completionError))
            error += "; semantic 3D pass rollback failed: " + completionError;
        if (!pass_->complete_encode(false, completionError))
            error += "; semantic pass rollback failed: " + completionError;
        return false;
    }
    std::string completionError;
    const bool completed3d = pass3d_->complete_encode(true, completionError);
    std::string completion2dError;
    const bool completed2d = pass_->complete_encode(true, completion2dError);
    if (!completed3d || !completed2d) {
        platform_->release_fence(fence);
        error = "submitted semantic resource commit failed: " +
                (completed3d ? completion2dError : completionError);
        return false;
    }

    ++stats_.submittedFrames;
    stats_.submittedOperations += frame->draws.size() + frame->models.size();
    bool hasPictures = false;
    bool hasGlyphs = false;
    bool hasSolidRectangles = false;
    for (const SemanticDraw& draw : frame->draws) {
        if (std::holds_alternative<PictureDraw>(draw)) {
            ++stats_.submittedPictures;
            if (std::get<PictureDraw>(draw).picture.source == PictureSource::J2dWindow)
                ++stats_.submittedJ2dWindowPictures;
            hasPictures = true;
        } else if (std::holds_alternative<GlyphDraw>(draw)) {
            ++stats_.submittedGlyphs;
            hasGlyphs = true;
        } else {
            ++stats_.submittedSolidRectangles;
            if (std::get<SolidRectangleDraw>(draw).rectangle.source ==
                SolidRectangleSource::J2dGrafContextFillBox) {
                ++stats_.submittedJ2dFillBoxes;
            } else if (std::get<SolidRectangleDraw>(draw).rectangle.source ==
                       SolidRectangleSource::J2dWindowContents) {
                ++stats_.submittedJ2dWindowContents;
            }
            hasSolidRectangles = true;
        }
    }
    if (static_cast<unsigned>(hasPictures) + static_cast<unsigned>(hasGlyphs) +
            static_cast<unsigned>(hasSolidRectangles) >
        1U) {
        ++stats_.mixedOperationFrames;
    }
    stats_.submittedImages += frame->images.size();
    stats_.submittedModels += frame->models.size();
    stats_.submittedMeshes += frame->meshes.size();
    for (const MeshResourceView& mesh : frame->meshes)
        stats_.submittedMeshVertices += mesh.vertices.size();
    if (!frame->draws.empty() || !frame->models.empty())
        ++stats_.nonEmptyFrames;
    const auto deadline = std::chrono::steady_clock::now() + kFenceTimeout;
    while (!platform_->fence_signaled(fence)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            platform_->release_fence(fence);
            error = "semantic SDL frame fence made no progress for five seconds";
            return false;
        }
        SDL_Delay(1);
    }
    platform_->release_fence(fence);
    ++stats_.completedFrames;
    if (presentationRequested) {
        if (presentResult == PresentResult::Presented)
            ++stats_.presentedFrames;
        else
            ++stats_.windowUnavailableFrames;
    }
    consumedSequence_ = sequence;
    if (sample && !measure_readback(*frame, error))
        return false;
    return true;
}

bool SdlSemanticFrameClient::stop_collection(std::string& error) noexcept {
    error.clear();
    if (!active_ || bridge_ == nullptr || !bridge_->active())
        return true;
    if (!bridge_->deactivate()) {
        error = std::string("semantic frame bridge deactivation failed: ") + bridge_->last_error();
        return false;
    }
    return true;
}

bool SdlSemanticFrameClient::validate_output(std::string& error) const noexcept {
    error.clear();
    if (!active_)
        return true;
    if (stats_.submittedFrames == 0) {
        error = "semantic output submitted no frames";
        return false;
    }
    if (stats_.completedFrames != stats_.submittedFrames) {
        error = "semantic output did not complete every submitted frame";
        return false;
    }
    if (config_.readback != SemanticReadbackMode::None &&
        (stats_.sampledFrames == 0 || stats_.firstNonClearFrame == 0)) {
        error = "semantic output never observed pixels distinct from the controlled clear";
        return false;
    }
    if (config_.presentationWindow != nullptr && stats_.presentedFrames == 0) {
        error = "native 2D preview never presented a frame to the application window";
        return false;
    }
    return true;
}

bool SdlSemanticFrameClient::shutdown(std::string& error) noexcept {
    error.clear();
    if (!active_) {
        release_resources();
        return true;
    }
    if (!stop_collection(error))
        return false;
    if (!platform_->wait_idle(error))
        return false;
    active_ = false;
    release_resources();
    return true;
}

bool SdlSemanticFrameClient::ready() const noexcept {
    return active_ && platform_ != nullptr && bridge_ != nullptr && target_.ready() &&
           pass3d_ != nullptr && pass_ != nullptr &&
           (config_.presentationWindow == nullptr || platform_->presenter_ready());
}

const SdlSemanticFrameStats& SdlSemanticFrameClient::stats() const noexcept {
    return stats_;
}

bool SdlSemanticFrameClient::cancel_encode(SDL_GPUCommandBuffer* commandBuffer,
                                           std::string& error) noexcept {
    std::string cancellationError;
    const bool cancelled = platform_->cancel_command_buffer(commandBuffer, cancellationError);
    std::string completionError;
    const bool completed = pass_->complete_encode(false, completionError);
    std::string completion3dError;
    const bool completed3d = pass3d_->complete_encode(false, completion3dError);
    if (!cancelled)
        error += "; " + cancellationError;
    if (!completed)
        error += "; semantic pass rollback failed: " + completionError;
    if (!completed3d)
        error += "; semantic 3D pass rollback failed: " + completion3dError;
    return cancelled && completed && completed3d;
}

bool SdlSemanticFrameClient::should_read_back(const SemanticFrame& frame) const noexcept {
    if (readback_ == nullptr)
        return false;
    if (config_.readback == SemanticReadbackMode::EveryFrame)
        return true;
    return config_.readback == SemanticReadbackMode::UntilNonClear &&
           (!frame.draws.empty() || !frame.models.empty()) && stats_.firstNonClearFrame == 0;
}

bool SdlSemanticFrameClient::append_readback(SDL_GPUCommandBuffer* commandBuffer,
                                             const SemanticFrame& frame, std::string& error) {
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commandBuffer);
    if (copy == nullptr) {
        error = std::string("semantic readback copy-pass creation failed: ") + SDL_GetError();
        return false;
    }
    const SDL_GPUTextureRegion source{target_.color(),    0, 0, 0, 0, 0, frame.targetWidth,
                                      frame.targetHeight, 1};
    const SDL_GPUTextureTransferInfo destination{readback_, 0, frame.targetWidth,
                                                 frame.targetHeight};
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    return true;
}

bool SdlSemanticFrameClient::measure_readback(const SemanticFrame& frame,
                                              std::string& error) noexcept {
    auto* pixels = static_cast<const std::uint8_t*>(
        SDL_MapGPUTransferBuffer(platform_->device(), readback_, false));
    if (pixels == nullptr) {
        error = std::string("semantic readback map failed: ") + SDL_GetError();
        return false;
    }
    const std::size_t pixelCount = static_cast<std::size_t>(frame.targetWidth) * frame.targetHeight;
    std::size_t nonClear = 0;
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const std::size_t offset = pixel * 4U;
        if (pixels[offset] != 0 || pixels[offset + 1] != 0 || pixels[offset + 2] != 0 ||
            pixels[offset + 3] != 255) {
            ++nonClear;
        }
    }
    stats_.lastSampleHash = hash_bytes(pixels, pixelCount * 4U);
    stats_.lastSampleNonClearPixels = nonClear;
    ++stats_.sampledFrames;
    if (nonClear != 0 && stats_.firstNonClearFrame == 0) {
        stats_.firstNonClearFrame = stats_.submittedFrames;
        stats_.firstNonClearPixels = nonClear;
    }
    SDL_UnmapGPUTransferBuffer(platform_->device(), readback_);
    return true;
}

void SdlSemanticFrameClient::release_resources() noexcept {
    if (readback_ != nullptr && platform_ != nullptr && platform_->device() != nullptr)
        SDL_ReleaseGPUTransferBuffer(platform_->device(), readback_);
    readback_ = nullptr;
    pass_.reset();
    pass3d_.reset();
    target_.shutdown();
    platform_ = nullptr;
    bridge_ = nullptr;
    config_ = {};
    consumedSequence_ = 0;
}

SdlSemanticFrameClient& sdl_semantic_frame_client() noexcept {
    static SdlSemanticFrameClient client;
    return client;
}

} // namespace sb::native_render

// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_frame_renderer.h"

#include "frame_image_file.h"

#include <cstdio>
#include <string>

#include <sunbright/native_render/sdl_gpu_platform.h>
#include <sunbright/native_render/sdl_semantic_frame_client.h>
#include <sunbright/native_render/semantic_frame_bridge.h>

#include <SDL3/SDL.h>

namespace sunbright::gcnport_boot {
namespace {

void record(std::string& first, const std::string& error) {
    if (first.empty() && !error.empty()) {
        first = error;
    }
}

} // namespace

GuestFrameRenderer::~GuestFrameRenderer() {
    if (started_) {
        std::string error;
        static_cast<void>(finish(error));
    }
}

bool GuestFrameRenderer::observe_sample(const sb::native_render::SemanticFrameSample& sample,
                                        void* context, std::string& error) {
    auto& renderer = *static_cast<GuestFrameRenderer*>(context);
    if (renderer.imagePath_.empty() || renderer.imageWritten_) {
        return true;
    }
    // With no frame named, the readback mode has already decided this: it samples nothing after the
    // first non-clear frame. With one named, every frame is sampled and this is the choice.
    if (renderer.imageFrameWanted_ != 0 && sample.frameIndex != renderer.imageFrameWanted_) {
        return true;
    }
    if (!write_ppm(renderer.imagePath_, sample.width, sample.height, sample.rgba8, error)) {
        return false;
    }
    renderer.imageWritten_ = true;
    renderer.imageFrame_ = sample.frameIndex;
    return true;
}

bool GuestFrameRenderer::start(const std::string& imagePath, std::uint64_t imageFrame,
                               std::string& error) {
    if (started_) {
        error = "the frame renderer was already started";
        return false;
    }
    // Each step names itself in the failure. Three different things can refuse here -- SDL's video
    // subsystem, the GPU device and the client -- and each can do it with an empty message, which
    // would print as a renderer that failed for no reason at all.
    const auto refuse = [&error](const char* step, const char* detail) {
        error = std::string(step) + ": " +
                ((detail != nullptr && detail[0] != '\0') ? detail : "no detail given");
        return false;
    };
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return refuse("SDL video did not initialize", SDL_GetError());
    }
    auto& platform = sb::native_render::sdl_gpu_platform();
    std::string detail;
    if (!platform.initialize_device({}, detail)) {
        const bool failed = refuse("the SDL GPU device did not open", detail.c_str());
        SDL_Quit();
        return failed;
    }
    // The client owns the bridge's activation, including the fixed black clear its readback
    // measures against, so this does not activate the bridge itself -- doing so is what an
    // "already active" refusal from inside `initialize` means.
    auto& client = sb::native_render::sdl_semantic_frame_client();
    auto& bridge = sb::native_render::semantic_frame_bridge();
    if (!client.initialize(
            platform, bridge,
            {.width = FRAMEBUFFER_WIDTH,
             .height = FRAMEBUFFER_HEIGHT,
             .readback = imageFrame != 0 ? sb::native_render::SemanticReadbackMode::EveryFrame
                                         : sb::native_render::SemanticReadbackMode::UntilNonClear,
             .onSample = observe_sample,
             .onSampleContext = this},
            detail)) {
        const bool failed = refuse("the semantic frame client did not initialize", detail.c_str());
        static_cast<void>(platform.shutdown(detail));
        SDL_Quit();
        return failed;
    }
    if (budget_ != nullptr) {
        budget_->begin_frame();
    }
    if (!bridge.begin()) {
        const bool failed = refuse("the first frame did not open", bridge.last_error());
        static_cast<void>(client.shutdown(detail));
        static_cast<void>(platform.shutdown(detail));
        SDL_Quit();
        return failed;
    }
    imagePath_ = imagePath;
    imageFrameWanted_ = imageFrame;
    started_ = true;
    collecting_ = true;
    return true;
}

void GuestFrameRenderer::seal_frame() {
    if (!collecting_) {
        return;
    }
    seams_ += 1;
    auto& bridge = sb::native_render::semantic_frame_bridge();
    if (!bridge.seal()) {
        sealFailures_ += 1;
        record(firstError_, bridge.last_error());
        return;
    }
    std::string error;
    if (!sb::native_render::sdl_semantic_frame_client().encode_last_sealed(error)) {
        encodeFailures_ += 1;
        record(firstError_, error);
    }
    if (budget_ != nullptr) {
        budget_->begin_frame();
    }
    if (!bridge.begin()) {
        beginFailures_ += 1;
        record(firstError_, bridge.last_error());
        // Nothing further can be collected once a frame cannot be opened, and carrying on would
        // submit into no frame at all. Stopping here keeps the counts above meaningful.
        collecting_ = false;
    }
}

bool GuestFrameRenderer::finish(std::string& error) {
    if (!started_) {
        return true;
    }
    started_ = false;
    collecting_ = false;
    // The client deactivates the bridge as part of its own shutdown, and the platform's device
    // outlives both, so this unwinds in exactly that order.
    bool ok = sb::native_render::sdl_semantic_frame_client().shutdown(error);
    std::string platformError;
    if (!sb::native_render::sdl_gpu_platform().shutdown(platformError)) {
        if (error.empty()) {
            error = platformError;
        }
        ok = false;
    }
    SDL_Quit();
    return ok;
}

void GuestFrameRenderer::report() const {
    const auto& stats = sb::native_render::sdl_semantic_frame_client().stats();
    std::printf("gmse01_boot: guest frame renderer: %llu frame seam(s), %llu submitted, %llu "
                "completed, %llu non-empty\n",
                static_cast<unsigned long long>(seams_),
                static_cast<unsigned long long>(stats.submittedFrames),
                static_cast<unsigned long long>(stats.completedFrames),
                static_cast<unsigned long long>(stats.nonEmptyFrames));
    std::printf("gmse01_boot:   %llu model(s), %llu mesh(es), %llu vertex(es), %llu image(s) "
                "reached the passes\n",
                static_cast<unsigned long long>(stats.submittedModels),
                static_cast<unsigned long long>(stats.submittedMeshes),
                static_cast<unsigned long long>(stats.submittedMeshVertices),
                static_cast<unsigned long long>(stats.submittedImages));
    // The clear is black, so a frame whose readback is entirely clear drew nothing that survived.
    // Reporting the first frame that was not, and how much of it was not, is what separates a
    // renderer that ran from one that only submitted.
    std::printf("gmse01_boot:   %llu frame(s) sampled; first non-clear frame %llu with %zu "
                "non-clear pixel(s); last sample hash %llu\n",
                static_cast<unsigned long long>(stats.sampledFrames),
                static_cast<unsigned long long>(stats.firstNonClearFrame),
                stats.firstNonClearPixels, static_cast<unsigned long long>(stats.lastSampleHash));
    // The client's own verdict, in its words. A run that submitted thousands of models and read
    // back nothing but the clear is the failure this diagnostic exists to catch, and it prints
    // identically to a successful one unless the negative is asked for and reported.
    std::string error;
    if (sb::native_render::sdl_semantic_frame_client().validate_output(error)) {
        std::printf("gmse01_boot:   the client validated its own output\n");
    } else {
        std::printf("gmse01_boot:   REFUSES: the client did not validate its output: %s\n",
                    error.c_str());
    }
    if (!imagePath_.empty()) {
        // An image that was asked for and not written is a failure, and prints as one. A run that
        // reached no non-clear frame writes nothing, which is the same silence as a writer that
        // never fired -- so the two are told apart here rather than left to be inferred.
        if (imageWritten_) {
            std::printf("gmse01_boot:   wrote frame %llu to %s\n",
                        static_cast<unsigned long long>(imageFrame_), imagePath_.c_str());
        } else if (imageFrameWanted_ != 0) {
            std::printf("gmse01_boot:   REFUSES: no frame was written to %s; the run sealed %llu "
                        "frame(s) and never reached frame %llu\n",
                        imagePath_.c_str(), static_cast<unsigned long long>(seams_),
                        static_cast<unsigned long long>(imageFrameWanted_));
        } else {
            std::printf("gmse01_boot:   REFUSES: no frame was written to %s; nothing distinct from "
                        "the clear was ever sampled\n",
                        imagePath_.c_str());
        }
    }
    std::printf("gmse01_boot:   %llu seal failure(s), %llu encode failure(s), %llu begin "
                "failure(s); first error: %s\n",
                static_cast<unsigned long long>(sealFailures_),
                static_cast<unsigned long long>(encodeFailures_),
                static_cast<unsigned long long>(beginFailures_),
                firstError_.empty() ? "none" : firstError_.c_str());
}

} // namespace sunbright::gcnport_boot

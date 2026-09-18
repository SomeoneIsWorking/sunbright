// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

// Rasterising GMSE01's own draws, rather than counting them.
//
// The model probe composes the title's shapes into `native_render::ModelDraw`s and submits them to
// whatever sink the process holds. Until now that sink counted. This owner installs the shipping
// one instead -- the process frame bridge feeding `SdlSemanticFrameClient` on an offscreen device
// -- so the draws reach the same passes, pipelines and shaders the product uses.
//
// It is a separate owner from the boot run because it owns a device, a bridge lease and a frame
// lifetime, none of which the run understands. The run tells it when the title reached a frame
// seam; everything else is here.

namespace sunbright::gcnport_boot {

class GuestFrameRenderer {
  public:
    GuestFrameRenderer() = default;
    ~GuestFrameRenderer();

    GuestFrameRenderer(const GuestFrameRenderer&) = delete;
    GuestFrameRenderer& operator=(const GuestFrameRenderer&) = delete;

    // GMSE01's external framebuffer is 640x448; rendering its draws at another size would change
    // what the title's own projection covers, so the default is its size rather than a round one.
    static constexpr std::uint32_t FRAMEBUFFER_WIDTH = 640;
    static constexpr std::uint32_t FRAMEBUFFER_HEIGHT = 448;

    [[nodiscard]] bool start(std::string& error);

    // One guest frame seam: seal what the title submitted, encode it, and open the next frame. A
    // failure here is recorded rather than thrown, because the run must still reach its reported
    // boundary -- a renderer that stopped working halfway is a result, and an aborted run is not.
    void seal_frame();

    [[nodiscard]] bool finish(std::string& error);
    void report() const;

    [[nodiscard]] bool started() const noexcept { return started_; }

  private:
    bool started_ = false;
    bool collecting_ = false;
    std::uint64_t seams_ = 0;
    std::uint64_t sealFailures_ = 0;
    std::uint64_t encodeFailures_ = 0;
    std::uint64_t beginFailures_ = 0;
    std::string firstError_;
};

// The hook the run installs at the title's frame seam. It seals through the renderer and hands the
// body straight back, so the title's own frame pacing is unchanged.
struct FrameSeamHook {
    GuestFrameRenderer* renderer = nullptr;
    std::uint32_t address = 0;
    std::uint64_t entries = 0;

    gcnport::HookResult operator()(gcnport::GuestContext&) {
        entries += 1;
        renderer->seal_frame();
        return gcnport::HookResult::call_original_once();
    }
};

} // namespace sunbright::gcnport_boot

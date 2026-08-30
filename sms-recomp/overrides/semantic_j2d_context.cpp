#include "semantic_j2d_context.h"

#include "guest_byte_reader.h"
#include "j2d_picture_adapter.h"
#include "overrides.h"

#include "../runtime/sb_assert.h"

#include <sunbright/native_render/semantic_sink.h>

extern "C" void func_802cfda8(CPUState&); // J2DScreen::draw
extern "C" void func_802eb6bc(CPUState&); // J2DGrafContext::setup2D

namespace sb::recomp {
namespace {

native_render::PictureContextStack g_contexts{};
native_render::PictureContext g_activeContext{};
bool g_hasActiveContext = false;

class ScreenContextScope {
  public:
    explicit ScreenContextScope(const CPUState& cpu) {
        if (!native_render::has_semantic_sink())
            return;
        native_render::PictureContext context{};
        const J2DContextCaptureResult result =
            capture_j2d_context(live_guest_byte_reader(), cpu.gpr[3], cpu.gpr[6], context);
        SB_ASSERT(result == J2DContextCaptureResult::Success,
                  "semantic J2DScreen context capture failed: screen=%08x graf=%08x result=%u",
                  cpu.gpr[3], cpu.gpr[6], static_cast<unsigned>(result));
        active_ = g_contexts.push(context);
        SB_ASSERT(active_, "semantic J2DScreen context stack overflow");
    }

    ~ScreenContextScope() {
        if (active_)
            SB_ASSERT(g_contexts.pop(), "semantic J2DScreen context stack underflow");
    }

    ScreenContextScope(const ScreenContextScope&) = delete;
    ScreenContextScope& operator=(const ScreenContextScope&) = delete;

  private:
    bool active_ = false;
};

} // namespace

void run_semantic_j2d_screen_draw(CPUState& cpu) {
    ScreenContextScope semanticScope(cpu);
    func_802cfda8(cpu);
}

void run_semantic_j2d_setup(CPUState& cpu) {
    if (native_render::has_semantic_sink()) {
        native_render::PictureContext context{};
        const J2DContextCaptureResult result =
            capture_j2d_graph_context(live_guest_byte_reader(), cpu.gpr[3], context);
        if (result == J2DContextCaptureResult::Success) {
            g_activeContext = context;
            g_hasActiveContext = true;
        } else if (result == J2DContextCaptureResult::NonOrthographic) {
            g_hasActiveContext = false;
        } else {
            SB_ASSERT(false, "semantic active J2D context capture failed: graf=%08x", cpu.gpr[3]);
        }
    }
    func_802eb6bc(cpu);
}

const native_render::PictureContext* current_semantic_j2d_context() noexcept {
    if (const native_render::PictureContext* scoped = g_contexts.current(); scoped != nullptr)
        return scoped;
    return g_hasActiveContext ? &g_activeContext : nullptr;
}

} // namespace sb::recomp

namespace {
void override_semantic_j2d_screen_draw(CPUState& cpu) {
    sb::recomp::run_semantic_j2d_screen_draw(cpu);
}

void override_semantic_j2d_setup(CPUState& cpu) {
    sb::recomp::run_semantic_j2d_setup(cpu);
}
} // namespace

SB_OVERRIDE(0x802cfda8u, override_semantic_j2d_screen_draw, "J2DScreen::draw",
            "publish renderer-neutral J2D canvas and clip context around retained body")
SB_OVERRIDE(0x802eb6bcu, override_semantic_j2d_setup, "J2DGrafContext::setup2D",
            "publish the active renderer-neutral J2D canvas for immediate-mode draws")

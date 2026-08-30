#include "native_j2d_context.h"

#include <sunbright/native_render/semantic_sink.h>

#include <JSystem/J2D/J2DGrafContext.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <dolphin/os.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

constexpr std::size_t kContextScopeCapacity = 16;
sb::native_render::PictureContextStack g_pictureContexts{};
std::array<bool, kContextScopeCapacity> g_contextScopeHasValue{};
std::size_t g_contextScopeDepth = 0;
sb::native_render::PictureContext g_activePictureContext{};
bool g_hasActivePictureContext = false;

bool picture_context_from_graph(const J2DGrafContext* context, bool clipEnabled,
                                sb::native_render::PictureContext& value) noexcept {
    if (context == nullptr || context->unk4 != 1)
        return false;
    const auto& graph = *static_cast<const J2DOrthoGraph*>(context);
    const JUTRect& logical = graph.getOrtho();
    const JUTRect& viewport = graph.mBounds;
    if (logical.getWidth() <= 0 || logical.getHeight() <= 0 || viewport.getWidth() <= 0 ||
        viewport.getHeight() <= 0) {
        return false;
    }
    value = {{{static_cast<float>(logical.x1), static_cast<float>(logical.y1)},
              {static_cast<float>(logical.getWidth()), static_cast<float>(logical.getHeight())},
              {viewport.x1, viewport.y1, static_cast<std::uint32_t>(viewport.getWidth()),
               static_cast<std::uint32_t>(viewport.getHeight())}},
             sb::native_render::j2d_target_scissor(
                 context->mScissorBounds.x1, context->mScissorBounds.y1, context->mScissorBounds.x2,
                 context->mScissorBounds.y2),
             clipEnabled};
    return sb::native_render::valid(value.canvas);
}

} // namespace

namespace sb {

const native_render::PictureContext* current_native_j2d_context() noexcept {
    if (g_contextScopeDepth != 0) {
        if (!g_contextScopeHasValue[g_contextScopeDepth - 1])
            return nullptr;
        return g_pictureContexts.current();
    }
    return g_hasActivePictureContext ? &g_activePictureContext : nullptr;
}

} // namespace sb

extern "C" void sb_native_picture_context_push(const void* contextPointer, int clipEnabled) {
    if (g_contextScopeDepth == g_contextScopeHasValue.size()) {
        OSPanic(__FILE__, __LINE__, "semantic J2D context stack overflow");
        return;
    }

    bool pushed = false;
    if (!sb::native_render::has_semantic_sink()) {
        g_contextScopeHasValue[g_contextScopeDepth++] = false;
        return;
    }
    sb::native_render::PictureContext value{};
    if (picture_context_from_graph(static_cast<const J2DGrafContext*>(contextPointer),
                                   clipEnabled != 0, value)) {
        pushed = g_pictureContexts.push(value);
        if (!pushed)
            OSPanic(__FILE__, __LINE__, "semantic J2D context rejected ortho graph");
    }
    g_contextScopeHasValue[g_contextScopeDepth++] = pushed;
}

extern "C" void sb_native_picture_context_pop(void) {
    if (g_contextScopeDepth == 0) {
        OSPanic(__FILE__, __LINE__, "semantic J2D context stack underflow");
        return;
    }
    const bool hadValue = g_contextScopeHasValue[--g_contextScopeDepth];
    if (hadValue && !g_pictureContexts.pop())
        OSPanic(__FILE__, __LINE__, "semantic J2D context value stack underflow");
}

extern "C" void sb_native_picture_context_activate(const void* contextPointer) {
    if (!sb::native_render::has_semantic_sink()) {
        g_hasActivePictureContext = false;
        return;
    }
    sb::native_render::PictureContext value{};
    if (!picture_context_from_graph(static_cast<const J2DGrafContext*>(contextPointer), false,
                                    value)) {
        g_hasActivePictureContext = false;
        return;
    }
    g_activePictureContext = value;
    g_hasActivePictureContext = true;
}

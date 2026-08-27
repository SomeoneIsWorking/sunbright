#include "native_efb_copy_clear_draw.h"
#include "native_efb_copy_plan.h"
#include "native_render_pass.h"

#include <climits>
#include <cmath>
#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

void require_near(float actual, float expected) {
    require(std::fabs(actual - expected) < 1.0e-6f);
}

} // namespace

int main() {
    int passesBegun = 0;
    int vertexBindings = 0;
    const auto beginPass = [&] { return ++passesBegun; };
    const auto bindVertexBuffer = [&](int pass) {
        require(pass == passesBegun);
        ++vertexBindings;
    };
    require(sbr_native_begin_render_pass(beginPass, bindVertexBuffer, true) == 1);
    require(sbr_native_begin_render_pass(beginPass, bindVertexBuffer, true) == 2);
    require(passesBegun == 2 && vertexBindings == 2);
    // An empty frame still begins a pass for clear/present but has no draw and needs no binding.
    require(sbr_native_begin_render_pass(beginPass, bindVertexBuffer, false) == 3);
    require(vertexBindings == 2);

    NativeEfbCopySequence sequence;
    const auto firstEpoch = sequence.epoch();
    require(sequence.may_merge(firstEpoch));
    require(sequence.note_copy(1) == 1);
    // The synthetic clear is submitted at the incremented copy epoch. A pre-copy batch therefore
    // cannot merge across the barrier, while the clear itself belongs to the post-copy epoch.
    require(!sequence.may_merge(firstEpoch));
    require(sequence.may_merge(sequence.epoch()));

    const auto full = sbr_native_efb_copy_source(0, 0, 640, 448, 640, 448);
    require(full.valid && full.width == 640 && full.height == 448);

    const auto clipped = sbr_native_efb_copy_source(630, 440, 32, 32, 640, 448);
    require(clipped.valid && clipped.x == 630 && clipped.y == 440);
    require(clipped.width == 10 && clipped.height == 8);

    // Negative controls for the former std::clamp precondition violation: when x==targetWidth or
    // y==targetHeight, the old code passed low=1, high=0. These inputs must be rejected before any
    // GPU blit is encoded.
    require(!sbr_native_efb_copy_source(640, 0, 1, 1, 640, 448).valid);
    require(!sbr_native_efb_copy_source(0, 448, 1, 1, 640, 448).valid);
    require(!sbr_native_efb_copy_source(INT_MAX, 0, INT_MAX, 1, 640, 448).valid);
    require(!sbr_native_efb_copy_source(INT_MIN, 0, INT_MAX, 1, 640, 448).valid);

    // BP register layout follows GXSetCopyClear and the PE copy trigger: 0x4F is A:R, 0x50 is G:B,
    // 0x51 is 24-bit Z, BP 0x52 bit 11 enables the post-copy clear, and the update masks come from
    // ZMODE bit 4 / CMODE0 bits 3-4. These asymmetric bytes prove the channel order rather than
    // accepting a byte-swapped implementation.
    const auto decoded = sbr_native_efb_copy_clear_from_bp(1u << 11, 0x00001234u, 0x00005678u,
                                                           0x00800000u, 1u << 4, 1u << 3);
    require(decoded.enabled);
    require(decoded.colorUpdate);
    require(!decoded.alphaUpdate);
    require(decoded.depthUpdate);
    require_near(decoded.color[0], 0x34 / 255.0f);
    require_near(decoded.color[1], 0x56 / 255.0f);
    require_near(decoded.color[2], 0x78 / 255.0f);
    require_near(decoded.color[3], 0x12 / 255.0f);
    require_near(decoded.depth, 0x800000 / 16777215.0f);

    const auto clearDisabled = sbr_native_efb_copy_clear_from_bp(0, 0x0000FFFFu, 0x0000FFFFu, 0,
                                                                 1u << 4, (1u << 3) | (1u << 4));
    require(!clearDisabled.enabled);

    NativeEfbCopyRequest request{};
    request.dest = 0x80100000u;
    request.sourceX = 630;
    request.sourceY = 440;
    request.sourceWidth = 64;
    request.sourceHeight = 64;
    request.destWidth = 32;
    request.destHeight = 32;
    request.clear = decoded;
    const auto plan = sbr_native_efb_copy_plan(request, 640, 448);
    require(plan.has_copy());
    require(plan.has_clear());
    require(plan.source.x == 630 && plan.source.y == 440);
    require(plan.source.width == 10 && plan.source.height == 8);
    require(plan.destWidth == 32 && plan.destHeight == 32);
    NativeEfbCopyClearDraw draw{};
    require(sbr_native_efb_copy_clear_draw(plan, draw));
    require_near(draw.vertices[0].r, decoded.color[0]);
    require_near(draw.vertices[0].a, decoded.color[3]);
    require_near(draw.vertices[0].z, decoded.depth);
    require(draw.state.test == 1 && draw.state.func == 7 && draw.state.write == 1);
    require(draw.state.colorUpdate == 1 && draw.state.alphaUpdate == 0);
    require(draw.state.scissor[0] == 630 && draw.state.scissor[1] == 440);
    require(draw.state.scissor[2] == 10 && draw.state.scissor[3] == 8);
    require(draw.tev.numStages == 1);
    require(draw.tev.stage[0].cD == 10 && draw.tev.stage[0].aD == 5);

    const auto colorOnly = sbr_native_efb_copy_clear_from_bp(1u << 11, 0x00001234u, 0x00005678u,
                                                             0x00800000u, 0, 1u << 3);
    request.clear = colorOnly;
    const auto colorOnlyPlan = sbr_native_efb_copy_plan(request, 640, 448);
    NativeEfbCopyClearDraw colorOnlyDraw{};
    require(sbr_native_efb_copy_clear_draw(colorOnlyPlan, colorOnlyDraw));
    require(colorOnlyDraw.state.colorUpdate == 1);
    require(colorOnlyDraw.state.alphaUpdate == 0);
    require(colorOnlyDraw.state.test == 0 && colorOnlyDraw.state.write == 0);

    // Known-positive control for Aurora's former empty-as-full failure: Hx_Test5's eighth row
    // starts exactly at y=448. It is neither a copy nor a clear on a 640x448 target.
    request.sourceX = 0;
    request.sourceY = 448;
    const auto offscreen = sbr_native_efb_copy_plan(request, 640, 448);
    require(!offscreen.has_copy());
    require(!offscreen.has_clear());
    require(!sbr_native_efb_copy_clear_draw(offscreen, draw));

    request.sourceY = 0;
    request.clear = clearDisabled;
    const auto noClear = sbr_native_efb_copy_plan(request, 640, 448);
    require(noClear.has_copy());
    require(!noClear.has_clear());

    request.clear = decoded;
    request.clear.colorUpdate = false;
    request.clear.depthUpdate = false;
    const auto noWriteMask = sbr_native_efb_copy_plan(request, 640, 448);
    require(noWriteMask.has_copy());
    require(!noWriteMask.has_clear());
    return 0;
}

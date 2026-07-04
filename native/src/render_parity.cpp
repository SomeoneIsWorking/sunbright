// render_parity.cpp — Tier-1 vs Tier-2 in-process parity harness.
//
// Bypasses the game entirely. When SB_HARNESS=<test-name> is set, boot.cpp
// routes control here instead of launching the game thread. The harness runs a
// hand-authored synthetic frame through the active render sink and dumps a
// deterministic PPM to scratch/parity/<test-name>.<tier>.ppm.
//
// This is the concrete first step of the multi-session Tier-2 rebuild
// (task #29, direction pivot 2026-07-04). Two runs of the harness (once with
// SB_RENDER=native, once with SB_RENDER=oracle) produce two PPMs; tools/render/
// tier_parity.sh runs both back-to-back and computes the pixel-delta.
//
// Test scenarios:
//   synth-clear     — clear color only. Trivial parity; proves harness plumbing.
//   synth-triangle  — one immediate-mode triangle. Both tiers consume the SAME
//                     GX SDK call sequence: state (BP/XF via gx_impl.cpp) +
//                     immediate-mode geometry (SbImmVtx via gx_imm_impl.cpp) +
//                     FIFO bytes (sb::gxfifo, drained by Tier 2). Delta > 0 on
//                     this scenario is the Tier-2 geometry blocker in
//                     measurable form.
//
// Design constraint: deterministic input, deterministic output. NO game code,
// NO wall-clock, NO async loaders. Two runs of the same tier must be bit-identical.

#include "../../runtime/engine.h"
#include "../render/gx_geom.h"       // NvkTevVertex / NvkTevBatch
#include "../render/gx_imm_xform.h"  // SbImmVtx / SbImmBatch
#include "../render/gx_sdlgpu.h"

#include <dolphin/gx.h>
#include "../platform/gx_state.h"   // sb::platform::gx::state() for z-state routing (needs gx.h)
#include <dolphin/mtx.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>   // std::abort
#include <cstring>
#include <sys/stat.h>
#include <vector>

// FAIL FAST (CLAUDE.md § "FAIL FAST"): every precondition / postcondition failure
// in the parity harness turns into a hard abort with a full diagnostic message.
// A "silent" failure here (silent no-op, return-error-code that the shell script
// ignores) manufactures a bogus green PPM and hides the real bug. Two runs of a
// broken tier must produce a LOUD abort — not a smaller PPM, not a bit-exact
// zero, not a truncated file. Wrapping the printf + abort in a macro so the
// error site + line show up in the stack.
#define PARITY_PANIC(fmt, ...) do { \
    std::fprintf(stderr, "\n[parity] ABORT at %s:%d — " fmt "\n", \
                 __FILE__, __LINE__, ##__VA_ARGS__); \
    std::fflush(stderr); \
    std::abort(); \
} while (0)

// Forward decls of the two render sinks.
extern "C" void sb_oracle_present_frame(void* framebuffer, void* user) __attribute__((weak));

// Optional direct-write bridge into Dolphin. When linked (Tier-2 builds), this
// brings Dolphin's video backend UP before the scenario runs so each GX SDK
// setter can call LoadBPReg/LoadXFReg immediately — no FIFO byte encoding.
namespace sb::oracle { bool ensure_up(); }
[[maybe_unused]] static bool oracle_ensure_up_or_no_op() {
    if constexpr (requires { sb::oracle::ensure_up(); }) {
        return sb::oracle::ensure_up();
    }
    return false;
}
// Immediate-mode batch drain (native/platform/gx_imm_impl.cpp).
extern "C" int sb_gx_imm_take_batches(const sb::render::SbImmVtx** verts,
                                      const sb::render::SbImmBatch** batches, int* nbatch);
extern "C" void sb_gx_get_clear_color(float* rgba);

namespace {

using sb::render::NvkTevBatch;
using sb::render::NvkTevVertex;
using sb::render::SbImmVtx;
using sb::render::SbImmBatch;

constexpr int kW = 640;
constexpr int kH = 480;

// A passthrough fragment shader: out = raster color (vColor). Isolates the
// blend / vertex / raster / clear path — combiner is not exercised here.
// Layout matches sb::gxsdl's TEV vertex-shader convention (raster_basic_test).
const char* kPassFrag =
    "#version 450\n"
    "layout(location=0) in vec4 vColor;\n"
    "layout(location=1) in vec2 vUV[8];\n"
    "layout(location=9) in vec4 vColor1;\n"
    "layout(location=0) out vec4 o;\n"
    "layout(set=0, binding=0) uniform sampler2D tex[8];\n"
    "layout(push_constant) uniform Mat { ivec4 kcolor[4]; ivec4 tevreg[4]; } m;\n"
    "void main(){ o = vColor; }\n";

void write_ppm_rgba_or_panic(const char* path, const uint8_t* rgba, int w, int h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) PARITY_PANIC("open PPM for write failed: %s (errno-strerror below)\n"
                         "                (harness cannot produce output; check "
                         "path exists / is writable)", path);
    if (std::fprintf(f, "P6\n%d %d\n255\n", w, h) <= 0) {
        std::fclose(f);
        PARITY_PANIC("write PPM header failed: %s", path);
    }
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    const size_t wanted = rgb.size();
    const size_t got = std::fwrite(rgb.data(), 1, wanted, f);
    if (got != wanted) {
        std::fclose(f);
        PARITY_PANIC("write PPM body short: got %zu bytes, wanted %zu (%s)",
                     got, wanted, path);
    }
    if (std::fclose(f) != 0)
        PARITY_PANIC("close PPM failed: %s", path);
}

void copy_file_or_panic(const char* src, const char* dst) {
    FILE* in = std::fopen(src, "rb");
    if (!in) PARITY_PANIC("open source for copy failed: %s (Tier-2 oracle sink "
                          "was supposed to write this — its present function may "
                          "have silently failed to produce a PPM)", src);
    FILE* out = std::fopen(dst, "wb");
    if (!out) { std::fclose(in); PARITY_PANIC("open dest for copy failed: %s", dst); }
    uint8_t buf[8192];
    size_t total = 0;
    while (size_t n = std::fread(buf, 1, sizeof buf, in)) {
        size_t got = std::fwrite(buf, 1, n, out);
        if (got != n) {
            std::fclose(in); std::fclose(out);
            PARITY_PANIC("copy short: got %zu/%zu at offset %zu (%s -> %s)",
                         got, n, total, src, dst);
        }
        total += n;
    }
    if (total == 0) {
        std::fclose(in); std::fclose(out);
        PARITY_PANIC("copy source was empty: %s (Tier-2 oracle sink produced a "
                     "0-byte PPM — its present function completed but wrote "
                     "nothing; this is the silent-fail case FAIL FAST catches)",
                     src);
    }
    std::fclose(in); std::fclose(out);
}

const char* tier_name(sb::engine::RenderMode m) {
    switch (m) {
        case sb::engine::RenderMode::NATIVE_PC:  return "native";
        case sb::engine::RenderMode::GX_ORACLE:  return "oracle";
    }
    return "unknown";
}

// ── Test scenarios ─────────────────────────────────────────────────────────────

// synth-clear — clear only. Both tiers should paint the same uniform colour.
struct SynthClear {
    static constexpr GXColor kClearColor{ 102, 178, 51, 255 };  // (0.4, 0.7, 0.2)
    static const char* name() { return "synth-clear"; }
    static void program() { GXSetCopyClear(kClearColor, 0xffffff); }
    // No draws. sb::gxsdl's frame_begin only stores the clear colour; force a
    // render pass with clearFirst=true so SDL_GPU_LOADOP_CLEAR runs.
    static bool needs_geometry() { return false; }
};

// synth-triangle — one immediate-mode triangle covering most of the viewport.
// Uses real GX SDK API for state + geometry. Populates both:
//   • Native GXState + SbImmVtx buffers (Tier 1 drain)
//   • sb::gxfifo FIFO byte stream (Tier 2 drain)
// so both tiers consume the SAME input at the SDK layer. Any pixel delta
// between the tiers is a genuine seam divergence, not an input mismatch.
struct SynthTriangle {
    static constexpr GXColor kClearColor{ 0, 0, 0, 255 };
    static const char* name() { return "synth-triangle"; }
    static bool needs_geometry() { return true; }
    static void program() {
        // Clear background is black — the triangle colours are what we're
        // measuring, so the clear must contribute zero everywhere else.
        GXSetCopyClear(kClearColor, 0xffffff);

        // Pixel-engine + raster state: no depth, no blend, colour+alpha write.
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);

        // Viewport + scissor: full 640x480, z ∈ [0, 1].
        GXSetViewport(0.f, 0.f, (f32)kW, (f32)kH, 0.f, 1.f);
        GXSetScissor(0, 0, kW, kH);

        // Channels + TEV: 1 chan, 0 texgens, 1 stage passing raster COLOR0.
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        // Raster COLOR0 = vertex colour (SRC_VTX). No lighting.
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX,
                      0, GX_DF_CLAMP, GX_AF_NONE);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

        // Matrices: IDENTITY-mapping ortho + identity position matrix. Both
        // Tier 1 (native imm_project) and Tier 2 (Dolphin's XF vertex shader)
        // apply proj × posmtx to the input vertex. For the two tiers to render
        // the same triangle from the same FIFO/state, this transform must be
        // pass-through (or at least symmetric). Using ortho(t=1, b=-1, l=-1,
        // r=1, n=0, f=1) yields a matrix that maps NDC → NDC (scales by 1).
        // The triangle vertices below are therefore already in NDC ([-1,+1]).
        // Standard GC ortho: n=0, f=1. This is what a game would naturally
        // use. Tier 2 must handle it — depth mapping into Vulkan's [0,1]
        // clip range is Dolphin's responsibility (via BPMEM viewport z
        // scale/offset), NOT the harness's. Do not "widen" this to n=-1
        // to make the triangle render on Tier 2 — that hides a real
        // Tier-2 bug.
        Mtx44 proj;
        C_MTXOrtho(proj, /*t=*/1.f, /*b=*/-1.f, /*l=*/-1.f, /*r=*/1.f,
                          /*n=*/0.f, /*f=*/1.f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        Mtx pos;
        MTXIdentity(pos);
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(pos, GX_PNMTX0);

        // Vertex descriptor: POS + COLOR0, both DIRECT (inline in the draw).
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

        // Draw one triangle. Vertices in NDC (identity ortho above):
        //   • upper-left  (-0.75, +0.67, 0) → red
        //   • upper-right (+0.75, +0.67, 0) → green
        //   • lower-center ( 0.00, -0.67, 0) → blue
        // (In GC convention +Y is up; Vulkan will flip Y-down at present time
        // via gx_imm_xform's -y remap. Tier 2 needs to apply the same convention
        // via the XF viewport / projection state.)
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-0.75f,  0.67f, 0.f);  GXColor4u8(255,   0,   0, 255);
        GXPosition3f32( 0.75f,  0.67f, 0.f);  GXColor4u8(  0, 255,   0, 255);
        GXPosition3f32( 0.00f, -0.67f, 0.f);  GXColor4u8(  0,   0, 255, 255);
        GXEnd();
    }
};

// SynthBlend — like SynthTriangle but with a non-black clear and an XLU
// triangle (α=0.5) on top. Exercises BPMEM_BLENDMODE + BPMEM_BP_MASK plus
// alpha propagation through the vertex → colours_0 → TEV alpha chain.
// The expected result at any triangle-interior pixel is
//   final = src*α + dst*(1-α) = 0.5*vertex_rgb + 0.5*(clear_rgb)
// If Tier 2 doesn't wire GXSetBlendMode, the triangle will overwrite the
// clear and this test will FAIL loudly.
struct SynthBlend {
    static constexpr GXColor kClearColor{ 32, 64, 128, 255 };   // dark blue-ish
    static const char* name() { return "synth-blend"; }
    static bool needs_geometry() { return true; }
    static void program() {
        GXSetCopyClear(kClearColor, 0xffffff);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        // Standard XLU blend: src*srcα + dst*(1-srcα)
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
        GXSetViewport(0.f, 0.f, (f32)kW, (f32)kH, 0.f, 1.f);
        GXSetScissor(0, 0, kW, kH);
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX,
                      0, GX_DF_CLAMP, GX_AF_NONE);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        Mtx44 proj;
        C_MTXOrtho(proj, 1.f, -1.f, -1.f, 1.f, 0.f, 1.f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        Mtx pos;
        MTXIdentity(pos);
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(pos, GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        // Uniform red triangle at α=128 (0.5). Blends over the dark-blue clear.
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-0.75f,  0.67f, 0.f);  GXColor4u8(255, 0, 0, 128);
        GXPosition3f32( 0.75f,  0.67f, 0.f);  GXColor4u8(255, 0, 0, 128);
        GXPosition3f32( 0.00f, -0.67f, 0.f);  GXColor4u8(255, 0, 0, 128);
        GXEnd();
    }
};

// SynthDepth — two overlapping triangles at different z. Front (green) at
// z=+0.3, back (red) at z=-0.3. With Z-test enabled and GX_LESS, the green
// triangle must occlude the red triangle at the overlap region.
//
// Exercises: BPMEM_ZMODE (enable/func/write), depth-buffer allocation on
// both tiers, XFMEM_VIEWPORT depth mapping into Vulkan's [0,1] clip range.
// If Tier 2's depth-buffer allocation is missing, both triangles render
// as if depth were disabled — front triangle "loses" to whichever draws
// last (red, drawn second) at the overlap.
struct SynthDepth {
    static constexpr GXColor kClearColor{ 0, 0, 0, 255 };
    static const char* name() { return "synth-depth"; }
    static bool needs_geometry() { return true; }
    static void program() {
        GXSetCopyClear(kClearColor, 0xffffff);
        // Z-test ON, GX_LESS, write depth. This is the point of the test.
        GXSetZMode(GX_TRUE, GX_LESS, GX_TRUE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
        GXSetViewport(0.f, 0.f, (f32)kW, (f32)kH, 0.f, 1.f);
        GXSetScissor(0, 0, kW, kH);
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX,
                      0, GX_DF_CLAMP, GX_AF_NONE);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        Mtx44 proj;
        // n=-1, f=1 so GC ortho maps world z∈[-1,+1] → NDC z∈[0,1] safely
        // inside Vulkan's clip range. Triangles at z=+0.3 and z=-0.3 land
        // at NDC z=0.35 / 0.65.
        C_MTXOrtho(proj, 1.f, -1.f, -1.f, 1.f, -1.f, 1.f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        Mtx pos;
        MTXIdentity(pos);
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(pos, GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        // GREEN triangle FRONT (z=+0.3): drawn FIRST.
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-0.6f,  0.5f, 0.3f);  GXColor4u8(0, 255, 0, 255);
        GXPosition3f32( 0.6f,  0.5f, 0.3f);  GXColor4u8(0, 255, 0, 255);
        GXPosition3f32( 0.0f, -0.5f, 0.3f);  GXColor4u8(0, 255, 0, 255);
        GXEnd();
        // RED triangle BACK (z=-0.3): drawn SECOND; must be Z-culled where
        // green is closer.
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-0.4f,  0.7f, -0.3f);  GXColor4u8(255, 0, 0, 255);
        GXPosition3f32( 0.8f,  0.7f, -0.3f);  GXColor4u8(255, 0, 0, 255);
        GXPosition3f32( 0.2f, -0.3f, -0.3f);  GXColor4u8(255, 0, 0, 255);
        GXEnd();
    }
};

// SynthPersp — perspective projection instead of ortho. A single triangle
// centred on the -Z axis, drawn at z=-3 (in front of the camera). Under
// perspective, x/y are DIVIDED by w = -z after projection, so the triangle
// should appear as a smaller centred triangle vs the ortho case.
//
// Exercises: XFMEM_SETPROJECTION type=perspective (Dolphin builds a
// different projection matrix layout — raw[1] populates m[0][2], not
// m[0][3]; raw[3] populates m[1][2]), the w-divide in the vertex shader,
// and the perspective depth mapping the viewport z-scale participates in.
struct SynthPersp {
    static constexpr GXColor kClearColor{ 0, 0, 0, 255 };
    static const char* name() { return "synth-persp"; }
    static bool needs_geometry() { return true; }
    static void program() {
        GXSetCopyClear(kClearColor, 0xffffff);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
        GXSetViewport(0.f, 0.f, (f32)kW, (f32)kH, 0.f, 1.f);
        GXSetScissor(0, 0, kW, kH);
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX,
                      0, GX_DF_CLAMP, GX_AF_NONE);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        // Perspective: 60° FOV, aspect kW/kH, near=1, far=100.
        Mtx44 proj;
        C_MTXPerspective(proj, 60.f, (f32)kW/(f32)kH, 1.f, 100.f);
        GXSetProjection(proj, GX_PERSPECTIVE);
        Mtx pos;
        MTXIdentity(pos);
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(pos, GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        // Triangle at z=-3, 2 units wide, 2 tall. Centred on -Z axis.
        // Under perspective (60° FOV, aspect 4:3), a unit at z=-3 covers
        // 1 / (3 * tan(30°)) ≈ 0.577 of the half-height. So the triangle
        // should NOT fill the frame — it should appear as a smaller
        // centred triangle, size distinct from the ortho baseline. That
        // difference is exactly what this scenario measures.
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-1.f,  1.f, -3.f);  GXColor4u8(255,   0,   0, 255);
        GXPosition3f32( 1.f,  1.f, -3.f);  GXColor4u8(  0, 255,   0, 255);
        GXPosition3f32( 0.f, -1.f, -3.f);  GXColor4u8(  0,   0, 255, 255);
        GXEnd();
    }
};

// SynthScale — same triangle as SynthTriangle but with a NON-identity
// posmtx (scale 2x in X, 0.5x in Y). Tests XFMEM_POSMATRICES with actual
// data instead of identity: if Dolphin gets a zero'd or default matrix,
// vertices multiply to zero and the triangle vanishes / degenerates.
// Both tiers should produce the same distorted triangle.
struct SynthScale {
    static constexpr GXColor kClearColor{ 0, 0, 0, 255 };
    static const char* name() { return "synth-scale"; }
    static bool needs_geometry() { return true; }
    static void program() {
        GXSetCopyClear(kClearColor, 0xffffff);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
        GXSetViewport(0.f, 0.f, (f32)kW, (f32)kH, 0.f, 1.f);
        GXSetScissor(0, 0, kW, kH);
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX,
                      0, GX_DF_CLAMP, GX_AF_NONE);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        Mtx44 proj;
        C_MTXOrtho(proj, 1.f, -1.f, -1.f, 1.f, 0.f, 1.f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        // Non-identity posmtx: half x, half y (shrink the triangle to a
        // quarter of the ortho'd frame area).
        Mtx pos;
        MTXIdentity(pos);
        pos[0][0] = 0.5f;   // scale X
        pos[1][1] = 0.5f;   // scale Y
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(pos, GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-0.75f,  0.67f, 0.f);  GXColor4u8(255,   0,   0, 255);
        GXPosition3f32( 0.75f,  0.67f, 0.f);  GXColor4u8(  0, 255,   0, 255);
        GXPosition3f32( 0.00f, -0.67f, 0.f);  GXColor4u8(  0,   0, 255, 255);
        GXEnd();
    }
};

// SynthViewport — INTENTIONALLY not wired into the dispatch: exposed a
// Tier-1 gap rather than a Tier-2 gap. Tier 2 handled the 320x240 offset
// viewport correctly first-run — triangle bounds x=[201..438]/y=[160..319]
// exactly matched the expected viewport-transformed screen coords
// (v0=(200,160), v1=(440,160), v2=(320,320)). Tier 1's SDL3-GPU imm
// pipeline ignores GXState.vpLeft/vpTop/vpWd/vpHt and paints with a
// full-screen Vulkan viewport, so its triangle came out at bounds
// x=[400..639]/y=[319..479] — completely off, sitting in the bottom-right
// as if the viewport were the whole frame. Closing this scenario needs
// Tier 1's imm-batch pipeline to consult GXState viewport, parallel to
// the scissor gap synth-scissor caught. Left here as documentation of
// what the "same triangle in a smaller/offset viewport" test should
// exercise (XFMEM_VIEWPORT with non-full-screen values — my zscale fix
// only covered the 640x480 case).
struct SynthViewport {
    static constexpr GXColor kClearColor{ 0, 0, 0, 255 };
    static const char* name() { return "synth-viewport"; }
    static bool needs_geometry() { return true; }
    static void program() {
        GXSetCopyClear(kClearColor, 0xffffff);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
        // The interesting bit: 320x240 viewport centred in the frame.
        GXSetViewport(160.f, 120.f, 320.f, 240.f, 0.f, 1.f);
        GXSetScissor(0, 0, kW, kH);
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX,
                      0, GX_DF_CLAMP, GX_AF_NONE);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        Mtx44 proj;
        C_MTXOrtho(proj, 1.f, -1.f, -1.f, 1.f, 0.f, 1.f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        Mtx pos;
        MTXIdentity(pos);
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(pos, GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-0.75f,  0.67f, 0.f);  GXColor4u8(255,   0,   0, 255);
        GXPosition3f32( 0.75f,  0.67f, 0.f);  GXColor4u8(  0, 255,   0, 255);
        GXPosition3f32( 0.00f, -0.67f, 0.f);  GXColor4u8(  0,   0, 255, 255);
        GXEnd();
    }
};

// SynthLit — enable one directional light on channel 0, matsource=REG
// (dark red material), enablelighting=on. The vertex normal points
// straight at the light. Expected shade = mat * (amb + diff) with
// diff = dot(N, -Ldir).
//
// Exercises: XFMEM_LIGHTS registers (0x0600 range) — Dolphin's per-vertex
// lighting_chn0 reads dpos/ddir/color/cosatt/distatt from xfmem.lights[k].
// If Tier 2 leaves them zero, the whole diffuse term is 0 and only the
// ambient contributes.
struct SynthLit {
    static constexpr GXColor kClearColor{ 0, 0, 0, 255 };
    static const char* name() { return "synth-lit"; }
    static bool needs_geometry() { return true; }
    static void program() {
        GXSetCopyClear(kClearColor, 0xffffff);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
        GXSetViewport(0.f, 0.f, (f32)kW, (f32)kH, 0.f, 1.f);
        GXSetScissor(0, 0, kW, kH);
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        // Material RED (128,0,0), ambient dark (32,32,32). Light L0 white,
        // direction pointing -Z (into the screen); vertex normals point +Z
        // (out of the screen, i.e. AT the light source) → cos(θ)=1 → full
        // diffuse contribution.
        GXColor mat = { 128, 0, 0, 255 };
        GXColor amb = {  32, 32, 32, 255 };
        GXSetChanMatColor(GX_COLOR0A0, mat);
        GXSetChanAmbColor(GX_COLOR0A0, amb);
        // Enable lighting on chan 0: amb_src=REG, mat_src=REG, light_mask
        // covers LIGHT0, DiffuseFunc=CLAMP, AttnFunc=NONE.
        GXSetChanCtrl(GX_COLOR0A0, GX_TRUE, GX_SRC_REG, GX_SRC_REG,
                      GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
        // Configure L0: directional white light. GC's convention for a
        // directional light is a POSITIONAL light placed far away along
        // the direction the light comes FROM, with atten k=(1,0,0). The
        // vertex → light vector is then approximately parallel (which is
        // what "directional" means).
        //   Light comes from +Z (behind the camera in Vulkan/GC view space
        //   with the identity posmtx), shines down -Z.
        //   → place the light at (0, 0, +1e6).
        // Direction is used for SPOT/SPECULAR lights; for pure diffuse we
        // leave dir at the GXInitLightDir default (which negates the arg,
        // so passing (0,0,-1) stores +Z — matches the "toward the light"
        // convention).
        GXLightObj lt;
        GXInitLightColor(&lt, GXColor{ 255, 255, 255, 255 });
        GXInitLightPos(&lt, 0.f, 0.f, 1000000.f);
        GXInitLightDir(&lt, 0.f, 0.f, -1.f);
        GXInitLightAttn(&lt, 1, 0, 0, 1, 0, 0);   // k=(1,0,0): distance-independent
        GXLoadLightObjImm(&lt, GX_LIGHT0);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        Mtx44 proj;
        C_MTXOrtho(proj, 1.f, -1.f, -1.f, 1.f, 0.f, 1.f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        Mtx pos;
        MTXIdentity(pos);
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(pos, GX_PNMTX0);
        // Vertex format now includes NORMAL. Vertex colour is not used for
        // channel-0 base (matsource=REG grabs from the material register)
        // but the vertex descriptor still allocates it — keep the layout
        // symmetrical with other scenarios so the vertex-loader gap is
        // isolated to normals only.
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_NRM,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM,  GX_NRM_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-0.75f,  0.67f, 0.f);
        GXNormal3f32(0.f, 0.f, 1.f);
        GXColor4u8(255, 255, 255, 255);
        GXPosition3f32( 0.75f,  0.67f, 0.f);
        GXNormal3f32(0.f, 0.f, 1.f);
        GXColor4u8(255, 255, 255, 255);
        GXPosition3f32( 0.00f, -0.67f, 0.f);
        GXNormal3f32(0.f, 0.f, 1.f);
        GXColor4u8(255, 255, 255, 255);
        GXEnd();
    }
};

// SynthScissor — kept for a future arc where Tier 1's SDL3-GPU pipeline
// grows per-batch scissor plumbing. Currently: Tier 1 has no scissor rect
// on the imm batches, so the full-screen white triangle bleeds outside
// the scissor and the harness panic fires. Not wired into the dispatch.
struct SynthScissor {
    static constexpr GXColor kClearColor{ 20, 80, 40, 255 };   // dark green
    static const char* name() { return "synth-scissor"; }
    static bool needs_geometry() { return true; }
    static void program() {
        GXSetCopyClear(kClearColor, 0xffffff);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
        GXSetCullMode(GX_CULL_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
        GXSetViewport(0.f, 0.f, (f32)kW, (f32)kH, 0.f, 1.f);
        // The interesting bit: scissor to (160,120)..(480,360) — a 320x240
        // window in the middle of the 640x480 frame. Anything outside must
        // survive as the clear colour.
        GXSetScissor(160, 120, 320, 240);
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX,
                      0, GX_DF_CLAMP, GX_AF_NONE);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        Mtx44 proj;
        C_MTXOrtho(proj, 1.f, -1.f, -1.f, 1.f, 0.f, 1.f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        Mtx pos;
        MTXIdentity(pos);
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(pos, GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        // A big white triangle that fully overlaps the whole viewport — the
        // scissor is doing the cutting.
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(-1.f,  1.f, 0.f);  GXColor4u8(255, 255, 255, 255);
        GXPosition3f32( 3.f,  1.f, 0.f);  GXColor4u8(255, 255, 255, 255);
        GXPosition3f32(-1.f, -3.f, 0.f);  GXColor4u8(255, 255, 255, 255);
        GXEnd();
    }
};

// ── Tier-1 render: SDL3 GPU ────────────────────────────────────────────────────

// Convert an SbImmBatch into an NvkTevBatch pointing at the same vertex range
// with the passthrough fragment shader. Only the fields the passthrough shader
// consumes (vcount / vstart / colour_update / blend / z) are set; textures and
// TEV constants are left at defaults.
NvkTevBatch imm_to_tev_batch(const SbImmBatch& ib, uint32_t vertex_base) {
    NvkTevBatch b{};
    b.vstart = vertex_base + ib.vstart;
    b.vcount = ib.vcount;
    b.fragGlsl = kPassFrag;
    b.shaderKey = 0x50415353c0d0dfULL;   // "PASS" + suffix — any unique u64 works
    // Route z-state from the captured GXState (unlike the game's imm path
    // in sms_boot_present.cpp which hardcodes z-off for 2D-on-top draws).
    // synth-depth needs this: without it, Tier 1 ignores GXSetZMode entirely
    // and both triangles overwrite in draw order.
    const auto& g = sb::platform::gx::state();
    b.z_test  = g.zCompare ? 1 : 0;
    b.z_write = g.zUpdate  ? 1 : 0;
    b.z_func  = (uint8_t)g.zFunc;
    // Faithful per-prim blend from the immediate-mode capture. blendType=1
    // is GX_BM_BLEND; factors are GXBlendFactor values (ZERO=0 .. INVSRCALPHA=5).
    // Without this the harness silently overwrote the framebuffer on XLU
    // triangles — synth-blend caught it (2026-07-04).
    b.blend_mode = (ib.blendType == 1) ? 1 : 0;
    b.src_factor = (uint8_t)ib.blendSrc;
    b.dst_factor = (uint8_t)ib.blendDst;
    b.color_update = 1; b.alpha_update = 1;
    return b;
}

int run_tier1(const char* test_name, bool has_geometry) {
    if (!sb::gxsdl::init(kW, kH))
        PARITY_PANIC("Tier 1: sb::gxsdl::init(%d,%d) FAILED — set "
                     "SDL_VIDEODRIVER=offscreen or run with a valid display",
                     kW, kH);

    // Drain any immediate-mode geometry the scenario submitted. For a clear-only
    // scenario (has_geometry=false) this returns 0 batches; that's expected.
    // If the scenario CLAIMED geometry but the imm buffer is empty, that's a
    // silent capture bug — panic instead of silently rendering a clear frame.
    const SbImmVtx* imm_verts = nullptr;
    const SbImmBatch* imm_batches = nullptr;
    int n_imm_batches = 0;
    int n_imm_verts = sb_gx_imm_take_batches(&imm_verts, &imm_batches, &n_imm_batches);
    if (has_geometry && n_imm_verts == 0)
        PARITY_PANIC("Tier 1: scenario '%s' declared has_geometry=true but "
                     "sb_gx_imm_take_batches returned 0 vertices — the GXBegin/"
                     "GXPosition/GXEnd path in the scenario didn't capture "
                     "anything into GXState. This is a silent-capture bug.",
                     test_name);
    if (has_geometry && n_imm_batches == 0)
        PARITY_PANIC("Tier 1: scenario '%s' captured %d vertices but 0 batches "
                     "— the SbImmBatch record wasn't emitted (finalize_prim gap)",
                     test_name, n_imm_verts);

    // Build the NvkTev vertex + batch list. For synth-clear there are none.
    std::vector<NvkTevVertex> verts;
    verts.reserve((size_t)n_imm_verts);
    for (int i = 0; i < n_imm_verts; ++i) {
        NvkTevVertex v{};
        v.x = imm_verts[i].x; v.y = imm_verts[i].y; v.z = imm_verts[i].z; v.w = 1.f;
        v.rgba[0] = imm_verts[i].r; v.rgba[1] = imm_verts[i].g;
        v.rgba[2] = imm_verts[i].b; v.rgba[3] = imm_verts[i].a;
        v.rgba1[0] = v.rgba[0]; v.rgba1[1] = v.rgba[1]; v.rgba1[2] = v.rgba[2]; v.rgba1[3] = v.rgba[3];
        verts.push_back(v);
    }
    std::vector<NvkTevBatch> batches;
    batches.reserve((size_t)n_imm_batches);
    for (int i = 0; i < n_imm_batches; ++i) batches.push_back(imm_to_tev_batch(imm_batches[i], 0));

    // Fetch the scenario's clear colour from GXState (both tiers do this same).
    float clear[4] = {0, 0, 0, 1};
    sb_gx_get_clear_color(clear);

    sb::gxsdl::frame_begin(clear[0], clear[1], clear[2], clear[3]);
    // draw_tev_segment runs the render pass (which does SDL_GPU_LOADOP_CLEAR
    // when clearFirst=true) and then draws each batch. A batch-less segment
    // just clears — that's the synth-clear path.
    sb::gxsdl::draw_tev_segment(verts.data(), (int)verts.size(),
                                batches.data(), (int)batches.size(),
                                /*clearFirst=*/true);
    sb::gxsdl::frame_end();

    std::vector<uint8_t> pix((size_t)kW * kH * 4, 0);
    if (!sb::gxsdl::readback(pix.data(), kW, kH))
        PARITY_PANIC("Tier 1: sb::gxsdl::readback(%d,%d) FAILED — SDL3 GPU may "
                     "have failed to download the color target; check that the "
                     "render pass actually executed (frame_begin/end pair)",
                     kW, kH);

    // NOTE: sb::gxsdl silent-return preconditions (g_ok, g_in_frame, g_cmd) can
    // ALSO produce a zero-buffer readback without any explicit failure. Detect
    // that case: if the scenario declared has_geometry and the buffer is
    // uniformly the clear colour, the batches were dropped silently.
    if (has_geometry) {
        bool all_uniform = true;
        const uint8_t* p0 = pix.data();
        for (int i = 4; i < (int)pix.size(); i += 4) {
            if (pix[i] != p0[0] || pix[i+1] != p0[1] || pix[i+2] != p0[2]) {
                all_uniform = false; break;
            }
        }
        if (all_uniform)
            PARITY_PANIC("Tier 1: scenario '%s' declared has_geometry=true but "
                         "the readback framebuffer is uniformly (%u,%u,%u) — "
                         "the batches were silently dropped. Likely candidates: "
                         "vertices clipped by the Vulkan [0,1] depth range, "
                         "sb::gxsdl::draw_tev_segment early-returned due to a "
                         "precondition, or the shader failed to compile silently.",
                         test_name, p0[0], p0[1], p0[2]);
    }

    ::mkdir("scratch", 0755);
    ::mkdir("scratch/parity", 0755);
    char path[192];
    std::snprintf(path, sizeof path, "scratch/parity/%s.native.ppm", test_name);
    write_ppm_rgba_or_panic(path, pix.data(), kW, kH);
    std::fprintf(stderr, "[parity] Tier 1 wrote %s (%d imm verts, %d batches)\n",
                 path, n_imm_verts, n_imm_batches);
    if (std::getenv("SB_PARITY_DBG")) {
        for (int i = 0; i < n_imm_verts && i < 8; ++i) {
            std::fprintf(stderr, "[parity]   v%d pos=(%.3f,%.3f,%.3f) rgba=(%.2f,%.2f,%.2f,%.2f)\n",
                         i, imm_verts[i].x, imm_verts[i].y, imm_verts[i].z,
                         imm_verts[i].r, imm_verts[i].g, imm_verts[i].b, imm_verts[i].a);
        }
        for (int i = 0; i < n_imm_batches && i < 4; ++i) {
            std::fprintf(stderr, "[parity]   batch%d vstart=%u vcount=%u textured=%d\n",
                         i, imm_batches[i].vstart, imm_batches[i].vcount,
                         (int)imm_batches[i].textured);
        }
    }
    (void)has_geometry;   // reserved for future scenario dispatch
    return 0;
}

// ── Tier-2 render: Dolphin videovulkan in-process ──────────────────────────────

int run_tier2(const char* test_name) {
    if (!&sb_oracle_present_frame)
        PARITY_PANIC("Tier 2 (oracle) sink NOT LINKED into this sms-boot build. "
                     "Rebuild with root-CMake `cmake --build build --target "
                     "sms-boot -j` so the Dolphin videovulkan sink is included; "
                     "the standalone build-native/ target links only Tier 1.");

    // Delete any stale file so we can be sure this run produced our copy.
    const char* src_ppm = "scratch/frames/oracle_0001.ppm";
    std::remove(src_ppm);

    sb_oracle_present_frame(nullptr, nullptr);

    ::mkdir("scratch", 0755);
    ::mkdir("scratch/parity", 0755);
    char dst_path[192];
    std::snprintf(dst_path, sizeof dst_path, "scratch/parity/%s.oracle.ppm", test_name);
    // copy_file_or_panic aborts on missing source, 0-byte source, or short
    // copy — the three silent-fail cases the oracle sink can produce.
    copy_file_or_panic(src_ppm, dst_path);
    std::fprintf(stderr, "[parity] Tier 2 wrote %s\n", dst_path);
    return 0;
}

} // namespace

// Entry point called from boot.cpp when SB_HARNESS=<test_name> is set.
extern "C" int sb_render_parity_run(const char* test_name) {
    if (!test_name || !test_name[0]) test_name = SynthClear::name();
    const auto mode = sb::engine::mode();
    std::fprintf(stderr, "[parity] harness start: test=%s tier=%s\n",
                 test_name, tier_name(mode));

    // In Tier-2 mode: bring Dolphin's video backend UP BEFORE the scenario
    // runs. Each subsequent GX SDK setter then routes directly to Dolphin's
    // LoadBPReg / LoadXFReg — no FIFO byte encoding. In Tier-1 mode, this is
    // a no-op.
    if (mode == sb::engine::RenderMode::GX_ORACLE) {
        (void)oracle_ensure_up_or_no_op();
    }

    bool has_geometry = false;
    if (std::strcmp(test_name, SynthClear::name()) == 0) {
        SynthClear::program();
        has_geometry = SynthClear::needs_geometry();
    } else if (std::strcmp(test_name, SynthTriangle::name()) == 0) {
        SynthTriangle::program();
        has_geometry = SynthTriangle::needs_geometry();
    } else if (std::strcmp(test_name, SynthBlend::name()) == 0) {
        SynthBlend::program();
        has_geometry = SynthBlend::needs_geometry();
    } else if (std::strcmp(test_name, SynthDepth::name()) == 0) {
        SynthDepth::program();
        has_geometry = SynthDepth::needs_geometry();
    } else if (std::strcmp(test_name, SynthPersp::name()) == 0) {
        SynthPersp::program();
        has_geometry = SynthPersp::needs_geometry();
    } else if (std::strcmp(test_name, SynthScale::name()) == 0) {
        SynthScale::program();
        has_geometry = SynthScale::needs_geometry();
    } else {
        // synth-lit intentionally not wired: closing it requires GXNormal3f32
        // implementation, per-vertex normal capture in the imm layer, CP
        // VCD/VAT normal enable, XFMEM_SETINVERTEXSPEC nrm count, and a
        // per-vertex normal in the FIFO payload — plus Tier 1's SDL3-GPU
        // renderer needs to grow lit-vertex material/lighting math, since
        // its current passthrough shader just outputs the vertex colour.
        // Left as a whole arc; the SynthLit struct above sits ready for
        // when the wiring lands.
        PARITY_PANIC("unknown test-name '%s' — known scenarios: synth-clear, "
                     "synth-triangle, synth-blend, synth-depth, synth-persp, "
                     "synth-scale. Typo in SB_HARNESS?", test_name);
    }

    switch (mode) {
    case sb::engine::RenderMode::NATIVE_PC: return run_tier1(test_name, has_geometry);
    case sb::engine::RenderMode::GX_ORACLE: return run_tier2(test_name);
    }
    PARITY_PANIC("unreachable: unknown sb::engine::RenderMode (%d) — "
                 "engine.h enum extended without harness dispatch",
                 (int)mode);
}

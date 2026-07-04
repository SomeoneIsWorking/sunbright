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
#include <dolphin/mtx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

// Forward decls of the two render sinks.
extern "C" void sb_oracle_present_frame(void* framebuffer, void* user) __attribute__((weak));
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

bool write_ppm_rgba(const char* path, const uint8_t* rgba, int w, int h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

bool copy_file(const char* src, const char* dst) {
    FILE* in = std::fopen(src, "rb");
    if (!in) return false;
    FILE* out = std::fopen(dst, "wb");
    if (!out) { std::fclose(in); return false; }
    uint8_t buf[8192];
    while (size_t n = std::fread(buf, 1, sizeof buf, in)) std::fwrite(buf, 1, n, out);
    std::fclose(in); std::fclose(out);
    return true;
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
    b.z_test = 0; b.z_write = 0;
    // Ignore captured blend for the passthrough test; force straight overwrite.
    b.blend_mode = 0; b.src_factor = 1; b.dst_factor = 0;
    b.color_update = 1; b.alpha_update = 1;
    return b;
}

int run_tier1(const char* test_name, bool has_geometry) {
    if (!sb::gxsdl::init(kW, kH)) {
        std::fprintf(stderr, "[parity] Tier 1: sb::gxsdl::init(%d,%d) FAILED — "
                             "need SDL_VIDEODRIVER=offscreen or a valid display\n",
                     kW, kH);
        return 1;
    }
    // Drain any immediate-mode geometry the scenario submitted. For a clear-only
    // scenario (needs_geometry=false) this returns 0 batches; that's fine.
    const SbImmVtx* imm_verts = nullptr;
    const SbImmBatch* imm_batches = nullptr;
    int n_imm_batches = 0;
    int n_imm_verts = sb_gx_imm_take_batches(&imm_verts, &imm_batches, &n_imm_batches);

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
    if (!sb::gxsdl::readback(pix.data(), kW, kH)) {
        std::fprintf(stderr, "[parity] Tier 1: readback FAILED\n");
        return 1;
    }
    ::mkdir("scratch", 0755);
    ::mkdir("scratch/parity", 0755);
    char path[192];
    std::snprintf(path, sizeof path, "scratch/parity/%s.native.ppm", test_name);
    if (!write_ppm_rgba(path, pix.data(), kW, kH)) {
        std::fprintf(stderr, "[parity] Tier 1: PPM write FAILED (%s)\n", path);
        return 1;
    }
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
    if (!&sb_oracle_present_frame) {
        std::fprintf(stderr, "[parity] Tier 2 (oracle) sink NOT LINKED into this "
                             "sms-boot build. Rebuild with root-CMake so the "
                             "Dolphin videovulkan sink is included.\n");
        return 1;
    }
    // The oracle sink writes its PPM at frame 1 (native/render/oracle_present.cpp).
    // Delete any stale file first so we know our copy is fresh.
    const char* src_ppm = "scratch/frames/oracle_0001.ppm";
    std::remove(src_ppm);

    sb_oracle_present_frame(nullptr, nullptr);

    ::mkdir("scratch", 0755);
    ::mkdir("scratch/parity", 0755);
    char dst_path[192];
    std::snprintf(dst_path, sizeof dst_path, "scratch/parity/%s.oracle.ppm", test_name);
    if (!copy_file(src_ppm, dst_path)) {
        std::fprintf(stderr, "[parity] Tier 2: could not relocate %s -> %s "
                             "(oracle sink may have failed to write)\n",
                     src_ppm, dst_path);
        return 1;
    }
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

    bool has_geometry = false;
    if (std::strcmp(test_name, SynthClear::name()) == 0) {
        SynthClear::program();
        has_geometry = SynthClear::needs_geometry();
    } else if (std::strcmp(test_name, SynthTriangle::name()) == 0) {
        SynthTriangle::program();
        has_geometry = SynthTriangle::needs_geometry();
    } else {
        std::fprintf(stderr, "[parity] unknown test-name '%s' (known: "
                             "synth-clear, synth-triangle)\n", test_name);
        return 2;
    }

    switch (mode) {
    case sb::engine::RenderMode::NATIVE_PC: return run_tier1(test_name, has_geometry);
    case sb::engine::RenderMode::GX_ORACLE: return run_tier2(test_name);
    }
    return 3;
}

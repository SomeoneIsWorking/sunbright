// render_parity.cpp — Tier-1 vs Tier-2 in-process parity harness (scaffold).
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
// Test scenarios (add incrementally as Tier 2 grows):
//   synth-clear    — clear color only. Trivial parity; proves the harness works.
//   synth-triangle — one hand-authored immediate-mode triangle (not yet).
//   synth-tev      — TEV combiner variations (not yet).
//   … etc.
//
// Design constraint: deterministic input, deterministic output. NO game code,
// NO wall-clock, NO async loaders. The two runs must be bit-identical PER TIER.

#include "../../runtime/engine.h"
#include "../render/gx_sdlgpu.h"

#include <dolphin/gx.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

// Forward decls of the two render sinks.
extern "C" void sb_oracle_present_frame(void* framebuffer, void* user) __attribute__((weak));

namespace {

constexpr int kW = 640;
constexpr int kH = 480;

// Write a raw RGBA8 buffer as a P6 PPM (RGB only, alpha discarded).
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

// Copy file src to dst. Used to relocate the oracle sink's default PPM output
// to our deterministic harness path.
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

// synth-clear — clear only. Both tiers should paint the same uniform colour.
// This is the trivial-parity test: passing means "the harness plumbing works",
// not "Tier 2 is faithful". A non-trivial delta here would mean one of the
// sinks failed to honour GXSetCopyClear (which we already verified separately
// — this test formalises that check as a repeatable ctest-style artifact).
struct SynthClear {
    static constexpr GXColor kClearColor{ 102, 178, 51, 255 };  // (0.4, 0.7, 0.2)
    static const char* name() { return "synth-clear"; }
};

// Program the shared GXState both tiers read from. The GX-seam layer
// (native/platform/gx_impl.cpp) captures GXSetCopyClear into
// GXState.copyClearColor; both Tier 1 and Tier 2 read it via
// sb_gx_get_clear_color at present time.
void program_synth_clear() {
    GXSetCopyClear(SynthClear::kClearColor, 0xffffff);
}

// Tier-1 rendering path: SDL3 GPU, direct sb::gxsdl calls. No batches — just
// clear and present. Reads back to CPU and writes PPM.
int run_tier1(const char* test_name) {
    if (!sb::gxsdl::init(kW, kH)) {
        std::fprintf(stderr, "[parity] Tier 1: sb::gxsdl::init(%d,%d) FAILED — "
                             "need SDL_VIDEODRIVER=offscreen or a valid display\n",
                     kW, kH);
        return 1;
    }
    const float r = SynthClear::kClearColor.r / 255.f;
    const float g = SynthClear::kClearColor.g / 255.f;
    const float b = SynthClear::kClearColor.b / 255.f;
    const float a = SynthClear::kClearColor.a / 255.f;
    sb::gxsdl::frame_begin(r, g, b, a);
    // sb::gxsdl's frame_begin only STORES the clear colour; the actual EFB clear
    // happens inside draw_tev_segment as SDL_GPU_LOADOP_CLEAR. Issue an empty
    // segment with clearFirst=true so the render pass runs and the clear lands
    // even for a batch-free harness frame.
    sb::gxsdl::draw_tev_segment(nullptr, 0, nullptr, 0, /*clearFirst=*/true);
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
    std::fprintf(stderr, "[parity] Tier 1 wrote %s\n", path);
    return 0;
}

// Tier-2 rendering path: Dolphin videovulkan in-process. The oracle sink's
// present function reads GXState.copyClearColor itself (via
// sb_gx_get_clear_color) so we just call it. It internally dumps to
// scratch/frames/oracle_0001.ppm; we relocate to our deterministic path.
int run_tier2(const char* test_name) {
    if (!&sb_oracle_present_frame) {
        std::fprintf(stderr, "[parity] Tier 2 (oracle) sink NOT LINKED into this "
                             "sms-boot build. Rebuild with root-CMake "
                             "`cmake --build build --target sms-boot` so the "
                             "Dolphin videovulkan sink is included.\n");
        return 1;
    }
    // The oracle sink writes its PPM at frame 1 (see native/render/oracle_present.cpp).
    // Delete any stale file so we can be sure this run produced our copy.
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
// Returns 0 on success, non-zero on failure. Never falls through to the game.
extern "C" int sb_render_parity_run(const char* test_name) {
    if (!test_name || !test_name[0]) test_name = SynthClear::name();
    const auto mode = sb::engine::mode();
    std::fprintf(stderr, "[parity] harness start: test=%s tier=%s\n",
                 test_name, tier_name(mode));

    if (std::strcmp(test_name, SynthClear::name()) != 0) {
        std::fprintf(stderr, "[parity] unknown test-name '%s' (only 'synth-clear' "
                             "implemented so far)\n", test_name);
        return 2;
    }

    program_synth_clear();

    switch (mode) {
    case sb::engine::RenderMode::NATIVE_PC: return run_tier1(test_name);
    case sb::engine::RenderMode::GX_ORACLE: return run_tier2(test_name);
    }
    return 3;
}

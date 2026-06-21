// sms_boot_present.cpp — SLICE 1 of attaching the native renderer to sms-boot.
//
// The VI seam (native/platform/vi_impl.cpp) calls a present hook once per retrace. Here
// we install that hook: lazily bring up the headless Vulkan rasterizer (nvk), render the
// frame the engine asked for, and (env-gated) dump it to scratch/frames/ as a PPM so the
// boot's on-screen output is VERIFIABLE headlessly — the foundation later slices grow on
// (slice 2: immediate-mode 2D / fader; slice 3: live J3D scene capture).
//
// SLICE 1 content: the captured GXSetCopyClear colour rendered as a full-frame clear.
// That alone proves init -> capture -> render -> present -> dump end-to-end in the boot
// exe. We only do GPU work while dumping is enabled and under the frame cap, so a normal
// run isn't slowed by per-frame lavapipe rasterization.
#include "nvk.h"
#include "gx_imm_xform.h"   // SbImmVtx (== NvkVertex layout)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

using namespace sb::render;

extern "C" {
// vi_present.h (declared here to avoid pulling the platform include dir into sms-render)
void sb_vi_set_present_hook(void (*fn)(void*, void*), void* user);
// gx_impl.cpp bridge: captured copy-clear colour as floats (0..1).
void sb_gx_get_clear_color(float* rgba);
// gx_imm_impl.cpp bridge: the frame's captured immediate-mode triangle list (Vulkan
// NDC + RGBA). Returns the vertex count (multiple of 3); marks the buffer consumed.
int sb_gx_imm_take(const SbImmVtx** out);
}

// The capture vertex and the renderer vertex must be byte-identical so the present can
// hand the captured triangles straight to nvk with no per-vertex copy/conversion.
static_assert(sizeof(SbImmVtx) == sizeof(NvkVertex), "SbImmVtx/NvkVertex layout");

namespace {

Nvk g_nvk;
bool g_init_tried = false;
bool g_init_ok = false;
int g_frame = 0;
int g_max_dump = 0;   // 0 = dumping disabled
int g_start_dump = 0; // first VI-retrace frame to begin dumping (SB_FRAME_DUMP_START)

constexpr uint32_t kW = 640, kH = 480;

void write_ppm(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", g_nvk.width(), g_nvk.height());
    for (uint32_t y = 0; y < g_nvk.height(); ++y)
        for (uint32_t x = 0; x < g_nvk.width(); ++x) {
            const uint8_t* p = g_nvk.at(x, y);
            std::fputc(p[0], f); std::fputc(p[1], f); std::fputc(p[2], f);
        }
    std::fclose(f);
}

void present_hook(void* /*framebuffer*/, void* /*user*/) {
    // Only do GPU work while we're still dumping frames (keeps normal runs fast).
    if (!g_max_dump) return;
    // Skip retraces before the requested start frame (cheaply, no GPU work) so a
    // gameplay-era frame can be captured without rasterizing thousands of boot frames.
    if (g_frame < g_start_dump) { ++g_frame; return; }
    if (g_frame >= g_start_dump + g_max_dump) return;

    if (!g_init_tried) {
        g_init_tried = true;
        g_init_ok = g_nvk.init(kW, kH) || g_nvk.init(kW, kH, true);
        if (!g_init_ok)
            std::fprintf(stderr, "[present] no Vulkan device (slice 1 dump disabled)\n");
        else
            std::printf("[present] nvk up (%ux%u) — dumping first %d frames to scratch/frames/\n",
                        kW, kH, g_max_dump);
    }
    if (!g_init_ok) { g_max_dump = 0; return; }

    float c[4] = {0, 0, 0, 1};
    sb_gx_get_clear_color(c);

    // SLICE 2: clear to the captured GXSetCopyClear colour, then draw the frame's
    // captured immediate-mode 2D (the fader overlay / GC-logo / J2D HUD quads).
    const SbImmVtx* imm = nullptr;
    int nimm = sb_gx_imm_take(&imm);
    std::vector<NvkVertex> verts(nimm);
    if (nimm)
        std::memcpy(verts.data(), imm, (size_t)nimm * sizeof(NvkVertex));
    g_nvk.renderTriangles(verts, NvkClear{c[0], c[1], c[2], c[3]});

    char path[160];
    std::snprintf(path, sizeof path, "scratch/frames/boot_%04d.ppm", g_frame);
    write_ppm(path);
    if (g_frame == 0 || (g_frame % 30) == 0)
        std::printf("[present] frame %d clear=(%.2f,%.2f,%.2f,%.2f) imm_tris=%d -> %s\n",
                    g_frame, c[0], c[1], c[2], c[3], nimm / 3, path);
    ++g_frame;
}

} // namespace

// Called from boot.cpp after PlatformInit. SB_FRAME_DUMP=1 enables PPM dumping;
// SB_FRAME_DUMP_MAX overrides the frame cap (default 120).
extern "C" void sb_boot_present_install() {
    if (const char* e = std::getenv("SB_FRAME_DUMP")) {
        if (e[0] && e[0] != '0') {
            const char* m = std::getenv("SB_FRAME_DUMP_MAX");
            g_max_dump = m ? std::atoi(m) : 120;
            if (g_max_dump <= 0) g_max_dump = 120;
            if (const char* s = std::getenv("SB_FRAME_DUMP_START"))
                g_start_dump = std::atoi(s) > 0 ? std::atoi(s) : 0;
            ::mkdir("scratch", 0755);
            ::mkdir("scratch/frames", 0755);
        }
    }
    sb_vi_set_present_hook(present_hook, nullptr);
}

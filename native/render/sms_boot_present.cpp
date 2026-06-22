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
// SLICE 3a: the frame's captured live J3D scene triangles (Vulkan NDC + RGBA), same
// shape as sb_gx_imm_take. WEAK — when the capture TU (sms_boot_j3d_capture.cpp) isn't
// linked, this is null and the present skips the 3D concat. NvkVertex layout (==SbImmVtx).
int sb_boot_capture_j3d_take(const NvkVertex** out) __attribute__((weak));
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
int g_dumped = 0;     // frames actually dumped so far
bool g_on_scene = false;     // SB_FRAME_DUMP_ON_SCENE: dump from the first frame with 3D geometry
bool g_dump_started = false; // (on-scene mode) the scene-geometry trigger has fired

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
    if (!g_max_dump || g_dumped >= g_max_dump) return;

    // Drain the frame's captured live J3D scene + immediate-mode 2D EVERY frame (cheap —
    // just hands back the buffers and marks them consumed, so the capture is per-frame, not
    // accumulated). We only rasterize/dump frames that qualify (below).
    const NvkVertex* scene = nullptr;
    int nscene = (&sb_boot_capture_j3d_take) ? sb_boot_capture_j3d_take(&scene) : 0;
    const SbImmVtx* imm = nullptr;
    int nimm = sb_gx_imm_take(&imm);

    // Decide whether to dump this frame. SB_FRAME_DUMP_ON_SCENE waits for the first frame
    // with actual 3D geometry (the scene loads after the HUD, so a fixed start-frame catches
    // a black-but-HUD frame). Otherwise use the fixed start-frame window.
    bool dumpThis;
    if (g_on_scene) {
        if (!g_dump_started && nscene > 0) g_dump_started = true;
        dumpThis = g_dump_started;
    } else {
        if (g_frame < g_start_dump) { ++g_frame; return; }
        dumpThis = (g_frame < g_start_dump + g_max_dump);
    }
    ++g_frame;
    if (!dumpThis) return;

    if (!g_init_tried) {
        g_init_tried = true;
        g_init_ok = g_nvk.init(kW, kH) || g_nvk.init(kW, kH, true);
        if (!g_init_ok)
            std::fprintf(stderr, "[present] no Vulkan device (slice 1 dump disabled)\n");
        else
            std::printf("[present] nvk up (%ux%u) — dumping %d frames to scratch/frames/\n",
                        kW, kH, g_max_dump);
    }
    if (!g_init_ok) { g_max_dump = 0; return; }

    float c[4] = {0, 0, 0, 1};
    sb_gx_get_clear_color(c);

    // SLICE 3a: draw the J3D scene FIRST (so the 2D HUD composites over it), then the 2D.
    std::vector<NvkVertex> verts((size_t)nscene + nimm);
    if (nscene)
        std::memcpy(verts.data(), scene, (size_t)nscene * sizeof(NvkVertex));
    if (nimm)
        std::memcpy(verts.data() + nscene, imm, (size_t)nimm * sizeof(NvkVertex));
    g_nvk.renderTriangles(verts, NvkClear{c[0], c[1], c[2], c[3]});

    char path[160];
    std::snprintf(path, sizeof path, "scratch/frames/boot_%04d.ppm", g_frame);
    write_ppm(path);
    std::printf("[present] frame %d clear=(%.2f,%.2f,%.2f,%.2f) scene_tris=%d imm_tris=%d -> %s\n",
                g_frame, c[0], c[1], c[2], c[3], nscene / 3, nimm / 3, path);
    ++g_dumped;
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
            if (const char* o = std::getenv("SB_FRAME_DUMP_ON_SCENE"))
                g_on_scene = (o[0] && o[0] != '0');
            ::mkdir("scratch", 0755);
            ::mkdir("scratch/frames", 0755);
        }
    }
    sb_vi_set_present_hook(present_hook, nullptr);
}

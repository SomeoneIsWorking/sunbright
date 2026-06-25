// gx_raylib.cpp — see gx_raylib.h. P1 of the GX→raylib switch: headless GL context + offscreen
// render target + clear + readback. Immediate-mode geometry / matrices / textures / TEV shaders
// arrive in later phases (docs/gx_raylib_switch.md).
#include "gx_raylib.h"

#include "raylib.h"
#include "rlgl.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

// Host-allocation gate (JKRHeap.cpp): while raised on this thread, every plain `new`
// routes to host malloc instead of the non-thread-safe JKR heap. The present thread is a
// GAME thread, so a GL call here would otherwise drive Mesa's SYNCHRONOUS C++ allocations
// onto the JKR heap (filling the solid heap -> "SolidHeap OUT OF MEMORY"). Raising the
// gate around every raylib/rlgl entry point keeps all driver allocations on malloc.
// (Concurrent Mesa worker threads are handled by the game-thread default in JKRHeap.cpp.)
extern "C" void sb_host_alloc_push(void);
extern "C" void sb_host_alloc_pop(void);

namespace sb::gxray {

namespace {
bool  g_init_tried = false;
bool  g_ctx_ok     = false;
int   g_w = 0, g_h = 0;
RenderTexture2D g_rt{};        // offscreen target (EFB-sized)
bool  g_in_frame = false;

// RAII: route driver/library allocations on this thread to host malloc for the duration
// of a raylib/rlgl call (see the note above).
struct HostAllocScope { HostAllocScope() { sb_host_alloc_push(); } ~HostAllocScope() { sb_host_alloc_pop(); } };
}

bool enabled() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_RAYLIB"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v == 1;
}

bool init(int w, int h) {
    if (g_init_tried) return g_ctx_ok && g_w == w && g_h == h;
    g_init_tried = true;
    HostAllocScope _hs;   // keep driver init allocations off the JKR heap

    // Hidden window → offscreen GL context (no visible surface). Proven headless on DISPLAY=:0.
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    { const char* v = std::getenv("SB_RAYLIB_VERBOSE");
      SetTraceLogLevel(v && v[0] && v[0] != '0' ? LOG_ALL : LOG_WARNING); }
    InitWindow(w, h, "sms-boot (raylib)");
    if (!IsWindowReady()) {
        std::fprintf(stderr, "[gxray] no GL context (InitWindow failed) — falling back to nvk\n");
        g_ctx_ok = false;
        return false;
    }
    g_rt = LoadRenderTexture(w, h);
    g_w = w; g_h = h; g_ctx_ok = true;
    std::fprintf(stderr, "[gxray] raylib context up (%dx%d), GL=%s\n", w, h, rlGetVersion() == RL_OPENGL_33 ? "GL3.3" : "GL");
    return true;
}

void frame_begin(float r, float g, float b, float a) {
    if (!g_ctx_ok || g_in_frame) return;
    HostAllocScope _hs;
    BeginTextureMode(g_rt);
    unsigned char cr = (unsigned char)(r * 255.0f + 0.5f), cg = (unsigned char)(g * 255.0f + 0.5f);
    unsigned char cb = (unsigned char)(b * 255.0f + 0.5f), ca = (unsigned char)(a * 255.0f + 0.5f);
    ClearBackground((Color){ cr, cg, cb, ca });
    g_in_frame = true;
}

void frame_end() {
    if (!g_ctx_ok || !g_in_frame) return;
    HostAllocScope _hs;
    rlDrawRenderBatchActive();   // flush any pending rlgl immediate-mode geometry
    EndTextureMode();
    g_in_frame = false;
}

bool readback(uint8_t* rgba, int w, int h) {
    if (!g_ctx_ok || w != g_w || h != g_h || !rgba) return false;
    HostAllocScope _hs;
    // RenderTexture is bottom-left origin; flip to top-left to match the PPM/nvk convention.
    Image img = LoadImageFromTexture(g_rt.texture);   // RGBA8
    ImageFlipVertical(&img);
    if (img.data && img.width == w && img.height == h)
        std::memcpy(rgba, img.data, (size_t)w * h * 4);
    bool ok = img.data != nullptr;
    UnloadImage(img);
    return ok;
}

} // namespace sb::gxray

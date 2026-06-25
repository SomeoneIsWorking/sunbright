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
#include <unordered_map>

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

// Decoded RGBA8 textures uploaded to GL, cached by the source pixel pointer (the present
// layer keeps decoded texels stable for the frame; many batches share one texture).
std::unordered_map<const void*, unsigned> g_tex_cache;

// GL enum constants used directly (raylib pulls in the GL loader; avoid a hard GL header dep).
enum {
    GL_ZERO_ = 0x0000, GL_ONE_ = 0x0001,
    GL_SRC_COLOR_ = 0x0300, GL_ONE_MINUS_SRC_COLOR_ = 0x0301,
    GL_SRC_ALPHA_ = 0x0302, GL_ONE_MINUS_SRC_ALPHA_ = 0x0303,
    GL_DST_ALPHA_ = 0x0304, GL_ONE_MINUS_DST_ALPHA_ = 0x0305,
    GL_DST_COLOR_ = 0x0306, GL_ONE_MINUS_DST_COLOR_ = 0x0307,
    GL_FUNC_ADD_ = 0x8006,
};

// GX blend factor (GXBlendFactor) → GL factor. `isSrc` picks the SRCCLR/DSTCLR reading:
// GX_BL_SRCCLR=2 as a SOURCE factor means dst colour, as a DEST factor means src colour
// (mirrors nvk.cpp gx_blend_factor — keep the two in lockstep).
int gx_blend_factor(uint8_t f, bool isSrc) {
    switch (f) {
        case 0: return GL_ZERO_;
        case 1: return GL_ONE_;
        case 2: return isSrc ? GL_DST_COLOR_ : GL_SRC_COLOR_;
        case 3: return isSrc ? GL_ONE_MINUS_DST_COLOR_ : GL_ONE_MINUS_SRC_COLOR_;
        case 4: return GL_SRC_ALPHA_;
        case 5: return GL_ONE_MINUS_SRC_ALPHA_;
        case 6: return GL_DST_ALPHA_;
        case 7: return GL_ONE_MINUS_DST_ALPHA_;
        default: return isSrc ? GL_ONE_ : GL_ZERO_;
    }
}

// A clip-space vertex carrying the attributes rlgl immediate mode can replay (pos colour uv0).
struct ClipV { float x, y, z, w, r, g, b, a, u, v; };
inline ClipV clipv_of(const sb::render::NvkTevVertex& t) {
    return ClipV{ t.x, t.y, t.z, (t.w != 0.0f ? t.w : 1.0f),
                  t.rgba[0], t.rgba[1], t.rgba[2], t.rgba[3], t.uv[0][0], t.uv[0][1] };
}
inline ClipV clipv_lerp(const ClipV& A, const ClipV& B, float t) {
    return ClipV{ A.x+t*(B.x-A.x), A.y+t*(B.y-A.y), A.z+t*(B.z-A.z), A.w+t*(B.w-A.w),
                  A.r+t*(B.r-A.r), A.g+t*(B.g-A.g), A.b+t*(B.b-A.b), A.a+t*(B.a-A.a),
                  A.u+t*(B.u-A.u), A.v+t*(B.v-A.v) };
}
// Emit one clipped vertex (w>0 guaranteed): perspective divide → Vulkan NDC, then to GL-NDC
// (flip Y: Vulkan +Y down → GL +Y up; remap depth [0,1] → [-1,1]).
inline void emit_clipv(const ClipV& v) {
    float w = v.w;
    rlColor4ub((unsigned char)(v.r * 255.0f + 0.5f), (unsigned char)(v.g * 255.0f + 0.5f),
               (unsigned char)(v.b * 255.0f + 0.5f), (unsigned char)(v.a * 255.0f + 0.5f));
    rlTexCoord2f(v.u, v.v);
    rlVertex3f(v.x / w, -v.y / w, (v.z / w) * 2.0f - 1.0f);
}
// Replay one triangle, Sutherland-Hodgman clipped against the near/behind-camera plane in
// HOMOGENEOUS clip space (w >= eps). This is the catastrophic case: a CPU divide of a vertex
// with w<=0 (geometry surrounding the camera — the sky/backdrop dome) flips/explodes it into a
// full-screen smear. Clipping in clip space (interpolating attributes by the same t) removes it
// perspective-correctly; the GPU rasteriser handles the side/viewport clipping after divide.
inline void emit_tri_clipped(const sb::render::NvkTevVertex& a, const sb::render::NvkTevVertex& b,
                             const sb::render::NvkTevVertex& c) {
    ClipV in[3] = { clipv_of(a), clipv_of(b), clipv_of(c) };
    const float eps = 1e-4f;
    ClipV out[5]; int m = 0;
    for (int i = 0; i < 3; ++i) {
        const ClipV& A = in[i]; const ClipV& B = in[(i + 1) % 3];
        bool ina = A.w >= eps, inb = B.w >= eps;
        if (ina) out[m++] = A;
        if (ina != inb) out[m++] = clipv_lerp(A, B, (eps - A.w) / (B.w - A.w));
    }
    if (m < 3) return;
    for (int i = 1; i + 1 < m; ++i) { emit_clipv(out[0]); emit_clipv(out[i]); emit_clipv(out[i + 1]); }
}

// Upload (and cache) one RGBA8 texture, returning its GL id.
unsigned tex_for(const sb::render::Nvk::NvkTevBatch::Tex& t) {
    auto it = g_tex_cache.find(t.rgba);
    if (it != g_tex_cache.end()) return it->second;
    unsigned id = rlLoadTexture(t.rgba, (int)t.w, (int)t.h,
                                RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    if (id) {
        int filt = t.linear ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
        rlTextureParameters(id, RL_TEXTURE_MAG_FILTER, filt);
        rlTextureParameters(id, RL_TEXTURE_MIN_FILTER, filt);
    }
    g_tex_cache.emplace(t.rgba, id);
    return id;
}
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

void draw_tev(const sb::render::NvkTevVertex* verts, int nverts,
              const sb::render::Nvk::NvkTevBatch* batches, int nbatch) {
    if (!g_ctx_ok || !g_in_frame || !verts || !batches) return;
    HostAllocScope _hs;

    // The present layer's decoded texels are per-frame (their pointers are reused next frame),
    // so the GL upload cache only keys validly WITHIN one draw_tev. Drop last frame's uploads.
    for (auto& kv : g_tex_cache) if (kv.second) rlUnloadTexture(kv.second);
    g_tex_cache.clear();

    // We feed GL-NDC directly, so neutralise rlgl's matrices (BeginTextureMode leaves a
    // pixel-space ortho). gl_Position = identity * vec4(ndc,1).
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION); rlPushMatrix(); rlLoadIdentity();
    rlMatrixMode(RL_MODELVIEW);  rlPushMatrix(); rlLoadIdentity();

    // Bisection diagnostics (value-first; never eyeball): SB_RAYLIB_MAXBATCH=N draws only
    // the first N batches (find which batch washes the frame); SB_RAYLIB_NODEPTH=1 forces
    // pure painter's order (isolate a depth-sort bug from a blend/order bug).
    static const int maxbatch = [](){ const char* v = std::getenv("SB_RAYLIB_MAXBATCH"); return v && v[0] ? std::atoi(v) : -1; }();
    static const bool nodepth = [](){ const char* v = std::getenv("SB_RAYLIB_NODEPTH"); return v && v[0] && v[0] != '0'; }();

    for (int bi = 0; bi < nbatch; ++bi) {
        if (maxbatch >= 0 && bi >= maxbatch) break;
        const auto& b = batches[bi];
        if (b.vcount == 0) continue;

        // Bind the batch texture (1x1 white default → modulate = raster colour only).
        unsigned texid = (b.tex[0].rgba && b.tex[0].w && b.tex[0].h)
                             ? tex_for(b.tex[0]) : rlGetTextureIdDefault();
        rlSetTexture(texid);

        // Per-batch GL state. rlgl applies blend/depth at FLUSH time and does NOT track them
        // per draw-call, so each batch is flushed independently (below) with its own state.
        if (b.blend_mode == 1 /*GX_BM_BLEND*/) {
            rlSetBlendFactors(gx_blend_factor(b.src_factor, true),
                              gx_blend_factor(b.dst_factor, false), GL_FUNC_ADD_);
            rlSetBlendMode(RL_BLEND_CUSTOM);
        } else {
            rlSetBlendMode(RL_BLEND_ALPHA);   // opaque batches: alpha=1 → effectively replace
            rlDisableColorBlend();
        }
        if (b.z_test && !nodepth) rlEnableDepthTest(); else rlDisableDepthTest();

        rlBegin(RL_TRIANGLES);
        // Batches are triangle lists (3N verts). Clip each tri against the near plane in clip
        // space, then emit the divided GL-NDC verts.
        uint32_t end = b.vstart + b.vcount;
        if (end > (uint32_t)nverts) end = (uint32_t)nverts;
        for (uint32_t i = b.vstart; i + 3 <= end; i += 3) {
            emit_tri_clipped(verts[i], verts[i + 1], verts[i + 2]);
        }
        rlEnd();
        rlDrawRenderBatchActive();   // flush THIS batch with its own blend/depth state
        rlEnableColorBlend();
    }

    rlMatrixMode(RL_PROJECTION); rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);  rlPopMatrix();
    rlEnableDepthTest();
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

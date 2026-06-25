// gx_raylib.cpp — see gx_raylib.h. P1 of the GX→raylib switch: headless GL context + offscreen
// render target + clear + readback. Immediate-mode geometry / matrices / textures / TEV shaders
// arrive in later phases (docs/gx_raylib_switch.md).
#include "gx_raylib.h"

#include "raylib.h"
#include "rlgl.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstddef>
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

// GL type enum fed straight to rlSetVertexAttribute (raylib hides its GL loader from consumers).
enum { GL_FLOAT_ = 0x1406 };

// One interleaved GL vertex: a 4-COMPONENT clip-space position so TRUE homogeneous xyzw rides to
// the vertex shader and the GPU performs native near/side clipping + perspective-correct interp —
// exactly nvk's Vulkan contract. (P2's immediate-mode path could only carry a 3-component position,
// forcing a CPU-approximate near-clip that left a sky starburst + a black foreground disc.)
struct RlClipVtx { float pos[4]; float uv[2]; float col[4]; };   // 40 bytes; locations 0 / 1 / 3

// Custom GLSL-330 program. VS passes the true clip-space xyzw straight to gl_Position (no rlgl
// MVP at all); FS = texture0 * vertexColor — the GX_MODULATE default. Per-material TEV combiner
// shaders are P4 (R2). Attribute locations match rlgl's convention (pos=0, texcoord=1, color=3).
const char* kClipVS =
    "#version 330\n"
    "layout(location=0) in vec4 vertexPosition;\n"   // GL clip-space xyzw
    "layout(location=1) in vec2 vertexTexCoord;\n"
    "layout(location=3) in vec4 vertexColor;\n"
    "out vec2 fragTexCoord;\n"
    "out vec4 fragColor;\n"
    "void main(){ fragTexCoord = vertexTexCoord; fragColor = vertexColor; gl_Position = vertexPosition; }\n";
const char* kClipFS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform sampler2D texture0;\n"
    "void main(){ finalColor = texture(texture0, fragTexCoord) * fragColor; }\n";

unsigned g_prog = 0; int g_loc_tex0 = -1; bool g_prog_tried = false;
unsigned g_vao = 0, g_vbo = 0; size_t g_vbo_cap = 0;   // VBO capacity in bytes
std::vector<RlClipVtx> g_scratch;

// Build the clip-space program once. Returns false if compile/link failed.
bool ensure_program() {
    if (g_prog_tried) return g_prog != 0;
    g_prog_tried = true;
    g_prog = rlLoadShaderCode(kClipVS, kClipFS);
    if (g_prog) g_loc_tex0 = rlGetLocationUniform(g_prog, "texture0");
    else std::fprintf(stderr, "[gxray] clip-space shader failed to compile/link\n");
    return g_prog != 0;
}

// (Re)allocate the VAO+VBO to hold at least `bytes`, baking the 3 vertex attributes into the VAO.
// glVertexAttribPointer records into the bound VAO referencing the bound VBO, so this must run with
// both bound (rlLoadVertexBuffer leaves the new VBO bound to GL_ARRAY_BUFFER).
void ensure_vbo(size_t bytes) {
    if (g_vbo && g_vbo_cap >= bytes) return;
    if (g_vbo) { rlUnloadVertexBuffer(g_vbo); g_vbo = 0; }
    if (!g_vao) g_vao = rlLoadVertexArray();
    rlEnableVertexArray(g_vao);
    g_vbo = rlLoadVertexBuffer(nullptr, (int)bytes, true);   // dynamic, sized; data uploaded per frame
    g_vbo_cap = bytes;
    const int stride = (int)sizeof(RlClipVtx);
    rlSetVertexAttribute(0, 4, GL_FLOAT_, false, stride, (int)offsetof(RlClipVtx, pos));
    rlEnableVertexAttribute(0);
    rlSetVertexAttribute(1, 2, GL_FLOAT_, false, stride, (int)offsetof(RlClipVtx, uv));
    rlEnableVertexAttribute(1);
    rlSetVertexAttribute(3, 4, GL_FLOAT_, false, stride, (int)offsetof(RlClipVtx, col));
    rlEnableVertexAttribute(3);
    rlDisableVertexArray();
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
    if (!ensure_program()) return;

    // The present layer's decoded texels are per-frame (their pointers are reused next frame),
    // so the GL upload cache only keys validly WITHIN one draw_tev. Drop last frame's uploads.
    for (auto& kv : g_tex_cache) if (kv.second) rlUnloadTexture(kv.second);
    g_tex_cache.clear();

    // Take over GL state from rlgl: flush any pending immediate geometry first.
    rlDrawRenderBatchActive();

    // nvk rasterizes with VK_CULL_MODE_NONE; rlgl's rlglInit enables GL_CULL_FACE(GL_BACK). With
    // our Y-flip reversing winding, that culls the inward faces of the sky/backdrop sphere (camera
    // INSIDE it) → radial slivers + a black back hemisphere. Disable culling to match nvk exactly.
    rlDisableBackfaceCulling();

    // Convert the WHOLE captured array to interleaved GL clip-space once, then draw each batch by
    // (vstart,vcount). Vulkan clip-space (y-down, depth [0,1]) → GL clip-space (y-up, depth
    // [-1,1]): pos = (x, -y, 2z - w, w). The VS passes this through; the GPU divides by w and
    // clips against all 6 frustum planes (near plane vk_z>=0 ⇔ GL 2z-w>=-w; far vk_z<=w preserved).
    if (nverts < 0) nverts = 0;
    g_scratch.resize((size_t)nverts);
    for (int i = 0; i < nverts; ++i) {
        const auto& t = verts[i];
        float w = (t.w != 0.0f) ? t.w : 1.0f;
        RlClipVtx& o = g_scratch[(size_t)i];
        o.pos[0] = t.x; o.pos[1] = -t.y; o.pos[2] = 2.0f * t.z - w; o.pos[3] = w;
        o.uv[0] = t.uv[0][0]; o.uv[1] = t.uv[0][1];
        o.col[0] = t.rgba[0]; o.col[1] = t.rgba[1]; o.col[2] = t.rgba[2]; o.col[3] = t.rgba[3];
    }
    if (nverts > 0) {
        size_t bytes = (size_t)nverts * sizeof(RlClipVtx);
        ensure_vbo(bytes);
        rlEnableVertexBuffer(g_vbo);
        rlUpdateVertexBuffer(g_vbo, g_scratch.data(), (int)bytes, 0);
    }

    // Bisection diagnostics (value-first; never eyeball): SB_RAYLIB_MAXBATCH=N draws only
    // the first N batches (find which batch washes the frame); SB_RAYLIB_NODEPTH=1 forces
    // pure painter's order (isolate a depth-sort bug from a blend/order bug).
    static const int maxbatch = [](){ const char* v = std::getenv("SB_RAYLIB_MAXBATCH"); return v && v[0] ? std::atoi(v) : -1; }();
    static const bool nodepth = [](){ const char* v = std::getenv("SB_RAYLIB_NODEPTH"); return v && v[0] && v[0] != '0'; }();

    for (int bi = 0; bi < nbatch && nverts > 0; ++bi) {
        if (maxbatch >= 0 && bi >= maxbatch) break;
        const auto& b = batches[bi];
        if (b.vcount == 0 || b.vstart >= (uint32_t)nverts) continue;
        uint32_t count = b.vcount;
        if (b.vstart + count > (uint32_t)nverts) count = (uint32_t)nverts - b.vstart;

        unsigned texid = (b.tex[0].rgba && b.tex[0].w && b.tex[0].h)
                             ? tex_for(b.tex[0]) : rlGetTextureIdDefault();

        // Set blend/depth BEFORE binding our program+VAO: rlSetBlendMode may internally flush
        // rlgl's batch (which unbinds VAO + shader). rlEnable/DisableColorBlend + depth-test are
        // direct glEnable/glDisable. Each batch carries its own GX blend (mirrors nvk).
        if (b.blend_mode == 1 /*GX_BM_BLEND*/) {
            rlEnableColorBlend();
            rlSetBlendFactors(gx_blend_factor(b.src_factor, true),
                              gx_blend_factor(b.dst_factor, false), GL_FUNC_ADD_);
            rlSetBlendMode(RL_BLEND_CUSTOM);
        } else {
            rlSetBlendMode(RL_BLEND_ALPHA);   // opaque batches: alpha=1 → effectively replace
            rlDisableColorBlend();
        }
        if (b.z_test && !nodepth) rlEnableDepthTest(); else rlDisableDepthTest();

        // Bind our clip-space program + texture (unit 0) + VAO, then a raw indexed-free draw.
        rlEnableShader(g_prog);
        rlActiveTextureSlot(0); rlEnableTexture(texid);
        int unit = 0; if (g_loc_tex0 >= 0) rlSetUniform(g_loc_tex0, &unit, RL_SHADER_UNIFORM_INT, 1);
        rlEnableVertexArray(g_vao);
        rlDrawVertexArray((int)b.vstart, (int)count);
    }

    // Restore a clean rlgl state for frame_end's batch flush.
    rlDisableVertexArray();
    rlDisableShader();
    rlActiveTextureSlot(0); rlEnableTexture(0);
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlEnableColorBlend();
    rlEnableDepthTest();
    rlEnableBackfaceCulling();
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

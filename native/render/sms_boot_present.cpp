// sms_boot_present.cpp — the native present hook (the DISPLAY) for sms-boot.
//
// The VI seam (native/platform/vi_impl.cpp) calls this once per retrace. It brings up the
// headless Vulkan rasterizer (nvk), draws the frame the engine produced, and (env-gated)
// dumps it to scratch/frames/ as a PPM so the boot's on-screen output is VERIFIABLE headlessly.
//
// The frame is the live J3D scene, captured per-material by sms_boot_j3d_capture.cpp as
// NvkTevVertex batches (each = a generated TEV combiner shader + textures + push constants +
// depth/blend state), composited via nvk's multi-material TEV frame (renderTevFrame). The
// immediate-mode 2D (fader / HUD) is drained as flat-colour verts and appended as one extra
// passthrough (out = rasterColor) batch drawn last, depth-test off, over the 3D scene.
//
// We only do GPU work while dumping is enabled and under the frame cap, so a normal run isn't
// slowed by per-frame lavapipe rasterization.
#include "gx_geom.h"
#include "gx_sdlgpu.h"         // SDL3 GPU GX backend (the GX→SDL3-GPU switch, behind SB_SDLGPU)
#include "gx_imm_xform.h"      // SbImmVtx / SbImmBatch (immediate-mode capture)
#include "ngx_render_data.h"   // NgxTevState (for the 2D passthrough / modulate shaders)
#include "tev_shader.h"        // sb_tev_gen_fragment
#include "tex_decode.h"        // sb_tex_decode (J2D textured-pane decode)
#include "sb_parity_dump.h"    // sb_parity_emit (value-track parity sweep)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <utility>
#include <sys/stat.h>
#include <sys/types.h>

using namespace sb::render;

extern "C" {
void sb_vi_set_present_hook(void (*fn)(void*, void*), void* user);
void sb_gx_get_clear_color(float* rgba);
int  sb_gx_imm_take(const SbImmVtx** out);
int  sb_gx_imm_take_batches(const SbImmVtx** verts, const SbImmBatch** batches, int* nbatch);
// Live GX state for the value-track parity dump (defined in the GX seam / capture TU).
int  sb_gx_get_lights(float out[8][16]) __attribute__((weak));
void sb_gx_get_chan_amb(int slot, float rgb[3]) __attribute__((weak));
void sb_gx_get_chan_matcolor(int slot, float rgba[4]) __attribute__((weak));
void sb_gx_get_projection(int* type, float proj[6], float vp[6]) __attribute__((weak));
void sb_boot_dump_camera(int frame) __attribute__((weak));
// JKR host-allocation gate: route plain `new`/std::vector growth to host malloc (not the game's
// JKR/Solid heap) while raised. present_hook runs on the GAME thread, so its per-frame renderer
// scratch (verts/batches/tex_storage/present_pix + anything renderTevFrame allocates) would otherwise
// land on the active SolidHeap and exhaust it (the 4M-per-run "SolidHeap OUT OF MEMORY" flood in
// window mode, which renders every frame). Renderer scratch is not a game object — gate it to host.
void sb_host_alloc_push(void) __attribute__((weak));
void sb_host_alloc_pop(void)  __attribute__((weak));
}
namespace {
// RAII guard: raise the host-alloc gate for the lifetime of the per-frame render block.
struct HostAllocScope {
    HostAllocScope()  { if (sb_host_alloc_push) sb_host_alloc_push(); }
    ~HostAllocScope() { if (sb_host_alloc_pop)  sb_host_alloc_pop(); }
};
} // namespace
// The frame's captured live J3D scene: vertex list + per-material TEV batches. WEAK — null when
// the capture TU (sms_boot_j3d_capture.cpp) isn't linked, in which case the present draws only 2D.
int sb_boot_capture_tev_take(const NvkTevVertex** verts,
                             const NvkTevBatch** batches, int* nbatches) __attribute__((weak));

namespace {

bool g_init_tried = false;
bool g_init_ok = false;
int g_frame = 0;
int g_max_dump = 0;
int g_start_dump = 0;
int g_dumped = 0;
bool g_on_scene = false;
bool g_dump_started = false;
bool g_settle_dump = false;   // SB_FRAME_DUMP_SETTLE: start dumping once the camera has SETTLED
int g_request_dump = 0;  // event-triggered dump request (next N presented frames)

// The 2D-overlay passthrough TEV shader (out = rasterColor), generated once.
std::string g_pass_frag;
uint64_t    g_pass_key = 0;

// The 2D textured TEV shader (out = texmap0 × rasterColor, GX_MODULATE), generated once.
// J2D textured panes (window borders, picture digits/marks, glyphs) modulate the bound
// texture by the per-vertex corner colour, then alpha-blend over the scene.
std::string g_tex_frag;
uint64_t    g_tex_key = 0;

constexpr uint32_t kW = 640, kH = 480;

uint64_t fnv64(const char* s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

void ensure_pass_shader() {
    if (!g_pass_frag.empty()) return;
    NgxTevState st{};
    st.num_stages = 1;
    st.stage[0].color_env = (15u<<12)|(15u<<8)|(15u<<4)|10u | (1u<<19);  // cc out = RASC
    st.stage[0].alpha_env = (7u<<13)|(7u<<10)|(7u<<7)|(5u<<4) | (1u<<19); // ac out = RASA
    st.stage[0].texmap = 0xff; st.stage[0].texcoord = 0xff; st.stage[0].color_chan = 0; // COLOR0A0
    for (int i = 0; i < 4; ++i) st.swap_table[i] = 0x1B;                  // identity (NOT 0 → "rrrr")
    g_pass_frag = sb_tev_gen_fragment(st);
    g_pass_key  = fnv64(g_pass_frag.c_str());
}

void ensure_tex_shader() {
    if (!g_tex_frag.empty()) return;
    NgxTevState st{};
    st.num_stages = 1;
    // GX_MODULATE: cprev = lerp(ZERO, TEXC, RASC) = TEXC*RASC (color), TEXA*RASA (alpha).
    // color_env bits: a(>>12) b(>>8) c(>>4) d(>>0), clamp(bit19). a=ZERO(15) b=TEXC(8)
    // c=RASC(10) d=ZERO(15).
    st.stage[0].color_env = (15u<<12)|(8u<<8)|(10u<<4)|15u | (1u<<19);
    // alpha_env bits: a(>>13) b(>>10) c(>>7) d(>>4), clamp(bit19). a=ZERO(7) b=TEXA(4)
    // c=RASA(5) d=ZERO(7).
    st.stage[0].alpha_env = (7u<<13)|(4u<<10)|(5u<<7)|(7u<<4) | (1u<<19);
    st.stage[0].texmap = 0; st.stage[0].texcoord = 0; st.stage[0].color_chan = 0; // COLOR0
    for (int i = 0; i < 4; ++i) st.swap_table[i] = 0x1B;                          // identity
    g_tex_frag = sb_tev_gen_fragment(st);
    g_tex_key  = fnv64(g_tex_frag.c_str());
}

// Generate (and cache) the real GX combiner fragment for an immediate-mode batch's captured
// TEV state. Cached by the fragment-string hash so distinct combiners each compile once and
// the returned const char* outlives renderTevFrame (the map is static). Mirrors the J3D path
// (which keys batches by fnv64 of the generated fragment).
const char* imm_tev_fragment(const NgxTevState& st, uint64_t& keyOut) {
    static std::unordered_map<uint64_t, std::string> s_cache;
    std::string frag = sb_tev_gen_fragment(st);
    uint64_t key = fnv64(frag.c_str());
    auto it = s_cache.find(key);
    if (it == s_cache.end()) it = s_cache.emplace(key, std::move(frag)).first;
    keyOut = key;
    return it->second.c_str();
}


// Write a PPM from a tightly-packed RGBA buffer (top-left origin) — the SDL3 GPU backend's readback.
void write_ppm_buf(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; ++i) std::fwrite(rgba + i * 4, 1, 3, f);
    std::fclose(f);
}

// Exported accessor for the current present frame (anonymous-namespace g_frame is TU-local,
// but reachable from this same TU). Other TUs use it for settled-frame-gated diagnostics.
} // namespace
extern "C" int sb_present_frame() { return g_frame; }
extern "C" int sb_camera_view_settled();   // scene_drive.cpp — view matrix stationary
extern "C" int sb_boot_capture_last_clear_boundary();  // sms_boot_j3d_capture.cpp — EFB clear boundary
extern "C" int sb_boot_capture_texsrc_is_efb_dest(const void*);  // texmap addr == an EFB-copy dest?
extern "C" int sb_boot_capture_efb_copy_count();       // # of EFB→texture copies this frame
// The ordered EFB-copy list (the GC multi-EFB-target structure). Layout mirrors SbEfbCopyOut in
// sms_boot_j3d_capture.cpp. Used to segment the present render at the copy boundaries.
struct SbEfbCopy { int batch_index; const void* dest; int clear; int wd; int ht; };
extern "C" int sb_boot_capture_efb_copies(SbEfbCopy* out, int max);
namespace {

void present_hook(void* /*framebuffer*/, void* /*user*/) {
    // SB_PRESENT_TRACE: unconditional VI-present heartbeat — counts EVERY present_hook call
    // (i.e. every VIWaitForRetrace), independent of dumping, so we can measure whether the
    // VI/present rate collapses during a scene (the file-select state-0 stall: the capture
    // buffer accumulates 30k+ batches because present stops ticking → the eventual dump
    // render wedges the GPU). Interleave with [cardload] frame~ lines (both stderr) to see
    // how many presents happen per N perform frames.
    static const bool ptrace = [](){ const char* v = std::getenv("SB_PRESENT_TRACE"); return v && v[0] && v[0] != '0'; }();
    if (ptrace) {
        static long s_pc = 0; ++s_pc;
        if ((s_pc % 60) == 0) std::fprintf(stderr, "[present-beat] calls=%ld\n", s_pc);
    }
    // SB_BBOX_TRACE: map the captured scene's NDC bbox across frames (no GPU, no dump) to
    // see whether/when the camera brings geometry in-view.
    static const bool bbtrace = [](){ const char* v = std::getenv("SB_BBOX_TRACE"); return v && v[0] && v[0] != '0'; }();
    if (bbtrace) {
        const NvkTevVertex* sv = nullptr; const NvkTevBatch* sb = nullptr; int nb = 0;
        int n = (&sb_boot_capture_tev_take) ? sb_boot_capture_tev_take(&sv, &sb, &nb) : 0;
        static int s_bt = 0; static int s_f = 0; ++s_f;
        if (n > 0 && s_bt < 80 && (s_f % 8) == 0) {
            ++s_bt;
            float mnx=1e30f,mxx=-1e30f,mny=1e30f,mxy=-1e30f,mnz=1e30f,mxz=-1e30f;
            for (int i = 0; i < n; ++i) {
                if (sv[i].x<mnx)mnx=sv[i].x; if (sv[i].x>mxx)mxx=sv[i].x;
                if (sv[i].y<mny)mny=sv[i].y; if (sv[i].y>mxy)mxy=sv[i].y;
                if (sv[i].z<mnz)mnz=sv[i].z; if (sv[i].z>mxz)mxz=sv[i].z;
            }
            std::fprintf(stderr, "[bbtrace] f=%d nscene=%d nb=%d x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]\n",
                         s_f, n, nb, mnx,mxx,mny,mxy,mnz,mxz);
        }
        return;
    }
    // Drain the captured scene + 2D imm EVERY present (cheap; marks them consumed). This MUST run
    // before any early-return: the scene/imm capture buffers are append-only across the frame and
    // are only reset by being "taken" here. If we skip the drain on non-dumping frames the buffers
    // grow without bound (observed: 31304 batches / 30288 textures accumulated over ~1714 frames at
    // the file-select), which both leaks host memory and makes a later event-dump try to render
    // every accumulated frame at once (30k Vulkan texture submits → GPU wedge / watchdog kill).
    const NvkTevVertex* scene = nullptr;
    const NvkTevBatch* sbatches = nullptr;
    int nsbatch = 0;
    int nscene = (&sb_boot_capture_tev_take) ? sb_boot_capture_tev_take(&scene, &sbatches, &nsbatch) : 0;
    const SbImmVtx* imm = nullptr;
    const SbImmBatch* ibatches = nullptr;
    int nibatch = 0;
    int nimm = sb_gx_imm_take_batches(&imm, &ibatches, &nibatch);

    // SB_WINDOW=1: live-inspection mode — render + present EVERY frame to a window (run.sh -> sms-boot),
    // independent of the dump window. Artifacts (PPM/parity) still only write inside the dump window.
    static const bool windowMode = [](){ const char* v = std::getenv("SB_WINDOW"); return v && v[0] && v[0] != '0'; }();

    if (!windowMode && g_request_dump <= 0 && (!g_max_dump || g_dumped >= g_max_dump)) return;

    bool dumpThis;
    if (g_request_dump > 0) {
        // Event-triggered dump (e.g. TCardLoad reaching mState 0): capture the next N
        // presented frames regardless of the start/max window.
        dumpThis = true;
        --g_request_dump;
        ++g_frame;
    } else if (g_settle_dump) {
        // Dump only once the option camera has SETTLED (the file-select choice scene, ~present
        // frame 1690 — the number isn't known ahead of time, so we gate on the settle detector,
        // mirroring SB_BATCH_DBG=-1). This is the sms-boot companion to fileselect_oracle.sh's
        // settled Dolphin-GX capture: it lets the two be pixel-compared at the SAME settled state.
        if (!g_dump_started && nscene > 0 && sb_camera_view_settled()) g_dump_started = true;
        dumpThis = g_dump_started;
        ++g_frame;
    } else if (g_on_scene) {
        if (!g_dump_started && nscene > 0) g_dump_started = true;
        dumpThis = g_dump_started;
        ++g_frame;
    } else {
        dumpThis = (g_frame >= g_start_dump && g_frame < g_start_dump + g_max_dump);
        ++g_frame;
    }
    // In window mode we render every frame to keep g_cpu fresh (only artifact writes are gated on
    // dumpThis). BUT not until the first real SCENE has been captured: rendering during boot/loading
    // does a per-frame GPU fence wait on the cooperative scheduler's RUNNER thread, which starves the
    // async-load workers (JKRDecomp/JKRAram) that the runner then waits on → deadlock (watchdog hang).
    // Once gameplay is rendering, the loader workers are idle and the per-frame render is safe (this is
    // exactly when the dump path renders). STOPGAP until the worker threads are eliminated (async→sync,
    // memory sms-boot-threading-architecture) — then per-frame render is unconditionally safe.
    static bool g_scene_seen = false;
    if (nscene > 0) g_scene_seen = true;
    if (!dumpThis && (!windowMode || !g_scene_seen)) return;

    if (!g_init_tried) {
        g_init_tried = true;
        g_init_ok = sb::gxsdl::init((int)kW, (int)kH);   // SDL3 GPU is the GX seam's only renderer
        if (!g_init_ok)
            std::fprintf(stderr, "[present] SDL3 GPU init failed (dump disabled)\n");
        else
            std::printf("[present] SDL3 GPU up (%ux%u) — dumping %d frames to scratch/frames/\n",
                        kW, kH, g_max_dump);
    }
    if (!g_init_ok) { g_max_dump = 0; return; }
    ensure_pass_shader();
    ensure_tex_shader();

    // All per-frame renderer scratch below (verts/batches/tex_storage/present_pix + renderTevFrame
    // internals) must allocate on host malloc, NOT the game's JKR/Solid heap (we run on the game
    // thread). Held until present_hook returns; frees auto-route by tag regardless of gate state.
    HostAllocScope host_alloc_scope;

    float c[4] = {0, 0, 0, 1};
    sb_gx_get_clear_color(c);

    // Combined vertex list: scene verts first (batch vstarts are already 0-based into the scene
    // list), then the 2D imm verts appended after.
    std::vector<NvkTevVertex> verts((size_t)nscene + nimm);
    if (nscene) std::memcpy(verts.data(), scene, (size_t)nscene * sizeof(NvkTevVertex));
    for (int i = 0; i < nimm; ++i) {
        NvkTevVertex& tv = verts[(size_t)nscene + i];
        tv = NvkTevVertex{};
        tv.x = imm[i].x; tv.y = imm[i].y; tv.z = imm[i].z;
        tv.rgba[0] = imm[i].r; tv.rgba[1] = imm[i].g; tv.rgba[2] = imm[i].b; tv.rgba[3] = imm[i].a;
        tv.rgba1[0] = imm[i].r; tv.rgba1[1] = imm[i].g; tv.rgba1[2] = imm[i].b; tv.rgba1[3] = imm[i].a;
        tv.uv[0][0] = imm[i].u; tv.uv[0][1] = imm[i].v;   // texcoord0 for textured 2D panes
    }

    // SB_TEV_SOLID: force scene batches through the passthrough shader (out = rasterColor,
    // ignore textures/combiner) — isolates geometry+raster from the combiner/texture path.
    static const bool solid = [](){ const char* v = std::getenv("SB_TEV_SOLID"); return v && v[0] && v[0] != '0'; }();
    // SB_SKIP_KEY=hex: drop every scene batch whose shaderKey high-32 matches (bisect which batch
    // owns an artifact — e.g. eb5c8e74 = the file-select sea foam).
    static const unsigned skipKey = [](){ const char* v = std::getenv("SB_SKIP_KEY"); return v && v[0] ? (unsigned)std::strtoul(v,nullptr,16) : 0u; }();
    // SB_SKIP_BIDX="i,j,k": drop the scene batches at these unfiltered indices (bisect which b<N> owns
    // an artifact by exact index — e.g. the sky-checker batches b0,b1,b2). Up to 32 indices.
    static int s_skipN = 0; static int s_skipBidx[32];
    static const bool skipBidx = [](){
        const char* v = std::getenv("SB_SKIP_BIDX"); if (!v || !v[0]) return false;
        const char* p = v; while (*p && s_skipN < 32) { s_skipBidx[s_skipN++] = std::atoi(p);
            const char* c = std::strchr(p, ','); if (!c) break; p = c + 1; } return s_skipN > 0;
    }();
    // SB_OPAQUE_ONLY=1: draw only opaque scene batches (blend_mode 0) — isolate whether the
    // translucent/additive passes are blowing the scene to white (overbright diagnosis).
    static const bool opaqueOnly = [](){ const char* v = std::getenv("SB_OPAQUE_ONLY"); return v && v[0] && v[0] != '0'; }();
    static const bool texturedOnly = [](){ const char* v = std::getenv("SB_TEXTURED_ONLY"); return v && v[0] && v[0] != '0'; }();
    // SB_ABLATE_BM="src/dst[,src/dst...]": drop every BLENDED scene batch whose (src_factor,
    // dst_factor) matches one of the listed pairs. Lets the overbright harness systematically
    // disable each blend class (e.g. "4/1" additive SRCALPHA->ONE, "1/5" ONE->INVSRCALPHA) and
    // re-measure the native-vs-oracle brightness delta, so the tool ATTRIBUTES the overbright to a
    // specific layer instead of eyeballing batch dumps. Up to 8 pairs.
    static int s_ablN = 0; static uint8_t s_ablSrc[8], s_ablDst[8];
    static const bool ablBm = [](){
        const char* v = std::getenv("SB_ABLATE_BM"); if (!v || !v[0]) return false;
        const char* p = v;
        while (*p && s_ablN < 8) {
            int sf = std::atoi(p); const char* slash = std::strchr(p, '/'); if (!slash) break;
            int df = std::atoi(slash + 1);
            s_ablSrc[s_ablN] = (uint8_t)sf; s_ablDst[s_ablN] = (uint8_t)df; ++s_ablN;
            const char* comma = std::strchr(p, ','); if (!comma) break; p = comma + 1;
        }
        return s_ablN > 0;
    }();
    // SB_ABLATE_PHASE="N[,N...]": drop every scene batch captured from perform-list phase N
    // (1=unk40 pre-pass, 2=unk38, 3=unk3C, 4=mPerformListGX MAIN, 5=Silhouette, 6=GXPost). The
    // overbright harness uses this to attribute the double-composite to a specific perform list:
    // the EFB pre-passes render off-screen on GC, so compositing them into the visible framebuffer
    // double-draws the whole scene. Up to 8 phases.
    static int s_ablPhN = 0; static uint8_t s_ablPh[8];
    static const bool ablPhase = [](){
        const char* v = std::getenv("SB_ABLATE_PHASE"); if (!v || !v[0]) return false;
        const char* p = v;
        while (*p && s_ablPhN < 8) { s_ablPh[s_ablPhN++] = (uint8_t)std::atoi(p);
            const char* c = std::strchr(p, ','); if (!c) break; p = c + 1; }
        return s_ablPhN > 0;
    }();
    // SB_EFB_HONOR_CLEAR (default OFF — diagnostic): drop scene batches captured before the LAST
    // EFB clear copy. NOT a correct fix on its own: on GC the cleared pre-pass IS still visible
    // because its snapshot texture is drawn back by a later pass — dropping the pre-pass without
    // re-drawing its snapshot loses that content (measured 42.7→45.7 WORSE). The real fix is the
    // segmented snapshot+resample renderer (see the 2026-06-30 overbright journal). Kept as an A/B tool.
    static const bool honorClear = [](){ const char* v = std::getenv("SB_EFB_HONOR_CLEAR"); return v && v[0] && v[0]!='0'; }();
    const int clearBoundary = honorClear ? sb_boot_capture_last_clear_boundary() : -1;
    std::vector<NvkTevBatch> batches;
    batches.reserve((size_t)nsbatch + 1);
    for (int i = 0; i < nsbatch; ++i) {
        if (clearBoundary > 0 && i < clearBoundary) continue;   // pre-clear pre-pass → snapshotted+wiped
        if (skipBidx) { bool drop=false; for (int a=0;a<s_skipN;++a) if (i==s_skipBidx[a]){drop=true;break;} if (drop) continue; }
        NvkTevBatch b = sbatches[i];
        if (skipKey && (unsigned)(b.shaderKey >> 32) == skipKey) continue;
        if (ablPhase) { bool drop=false; for (int a=0;a<s_ablPhN;++a) if (b.phase==s_ablPh[a]){drop=true;break;} if (drop) continue; }
        if (opaqueOnly && b.blend_mode != 0) continue;
        if (ablBm && b.blend_mode != 0) {
            bool drop = false;
            for (int a = 0; a < s_ablN; ++a)
                if (b.src_factor == s_ablSrc[a] && b.dst_factor == s_ablDst[a]) { drop = true; break; }
            if (drop) continue;
        }
        // SB_TEXTURED_ONLY=1: draw only scene batches that have a bound texmap0 — isolate the
        // textured map surfaces from untextured (sky/gradient) batches (so TEVDBG=tex is meaningful).
        if (texturedOnly && !(b.tex[0].rgba && b.tex[0].w && b.tex[0].h)) continue;
        if (solid) { b.fragGlsl = g_pass_frag.c_str(); b.shaderKey = g_pass_key; b.blend_mode = 0;
                     b.z_test = 0; b.z_write = 0; }
        batches.push_back(b);
    }
    // Whether any diagnostic SCENE filter dropped/reordered scene batches — if so the EFB-copy
    // boundaries (recorded in unfiltered scene-batch index space) no longer map onto `batches`, so
    // the segmented present must NOT run (fall back to one pass). Default run: no filter → 1:1.
    const bool sceneFiltered = honorClear || ablPhase || opaqueOnly || ablBm ||
                               (skipKey != 0) || solid || texturedOnly || skipBidx;
    // Count of scene batches in `batches`; the 2D imm batches are appended after this index.
    const int nScenePushed = (int)batches.size();
    // 2D immediate batches: each is either a colour-only run (gradient, solid window fill →
    // passthrough shader) or a textured pane (window border, picture digit, glyph → decode
    // the bound GX texture once, MODULATE by vertex colour). Decoded textures are cached by
    // image ptr for this present (4 window-corner draws share one border texture). Storage
    // outlives renderTevFrame below (the batch's tex.rgba points into it).
    std::vector<std::vector<uint32_t>> tex_storage;
    std::unordered_map<const void*, int> tex_cache;   // image ptr → tex_storage index
    int n_tex_batches = 0;
    // SB_SKIP_IMM=1: drop ALL 2D immediate batches (isolate the 3D scene from the 2D overlay/
    // fader/wipe — diagnose a fullscreen cover hiding the scene). SB_SKIP_IMM_IDX=N: drop only
    // imm batch N (bisect which 2D quad covers the screen).
    static const bool skipImm = [](){ const char* v = std::getenv("SB_SKIP_IMM"); return v && v[0] && v[0] != '0'; }();
    static const int skipImmIdx = [](){ const char* v = std::getenv("SB_SKIP_IMM_IDX"); return v && v[0] ? std::atoi(v) : -1; }();
    for (int bi = 0; bi < nibatch; ++bi) {
        if (skipImm) break;
        if (bi == skipImmIdx) continue;
        const SbImmBatch& ib = ibatches[bi];
        // The TModelWaterManager water-volume / silhouette effect masks the framebuffer ALPHA with
        // J3D shape draws (SMS_DrawShape / SMS_DrawCube) that the immediate-mode path does NOT
        // capture, so the dst-alpha PLANE (which IS implemented + parity-tested, see
        // tests/parity/dst_alpha_test) has no live mask to read. Until that mask geometry is routed
        // to the alpha plane, drop the whole effect rather than render it wrong: the colour-OFF
        // setup/clear passes are invisible in GX anyway, and a dst-alpha-reading composite over an
        // empty mask paints fullscreen black (INV_DST_ALPHA→src). Ordinary 2D HUD/text/window draws
        // (no dst-alpha) are untouched. STOPGAP: route the water-volume mask shapes to the alpha
        // plane to render the actual tint — until then the effect is absent, not washed/black.
        const bool readsDstAlpha = (ib.blendType == 1) &&
            (ib.blendSrc == 6 || ib.blendSrc == 7 || ib.blendDst == 6 || ib.blendDst == 7);
        if (!ib.colorUpdate || readsDstAlpha) continue;
        NvkTevBatch b{};
        b.vstart = (uint32_t)nscene + ib.vstart;
        b.vcount = ib.vcount;
        std::memset(b.push.kcolor, 0xFF, sizeof b.push.kcolor);
        b.z_test = 0; b.z_write = 0;                 // 2D draws on top, no depth
        // GXSetColorUpdate / GXSetAlphaUpdate / GXSetDstAlpha — the destination-alpha plane the
        // TModelWaterManager water-volume / silhouette composites use: colour-OFF passes write
        // only the alpha mask (SMS_FillScreenAlpha forces it to 0), then a colour-ON pass
        // composites via DST_ALPHA. Honoring these renders the water tint where masked and stops
        // the colour-OFF fill passes from washing the scene (the Delfino white wash + black left).
        b.color_update = ib.colorUpdate ? 1 : 0;
        b.alpha_update = ib.alphaUpdate ? 1 : 0;
        b.dst_alpha_force = ib.dstAlphaForce ? 1 : 0;
        b.dst_alpha_val = ib.dstAlphaVal;
        // Faithful per-prim blend captured at GXBegin (GXSetBlendMode). Default to
        // GX_BM_BLEND SRCALPHA/INVSRCALPHA when the seam didn't capture (older paths / tests).
        // GX_BM_BLEND==1 -> blend on; factors are GXBlendFactor values (ZERO=0..INVSRCALPHA=5).
        // SB_NO_IMM_BLEND forces the old hardcode for A/B bisection.
        static const bool noimmblend = [](){ const char* v = std::getenv("SB_NO_IMM_BLEND"); return v && v[0] && v[0] != '0'; }();
        if (noimmblend || (ib.blendType == 0 && ib.blendSrc == 0 && ib.blendDst == 0)) {
            b.blend_mode = 1; b.src_factor = 4; b.dst_factor = 5;  // SRCALPHA / INVSRCALPHA
        } else {
            b.blend_mode = (ib.blendType == 1 /*GX_BM_BLEND*/) ? 1 : 0;
            b.src_factor = (uint8_t)ib.blendSrc;
            b.dst_factor = (uint8_t)ib.blendDst;
        }

        // Gate the bound texture before decoding (cf. sb_resolve_textures): a bad fmt,
        // absurd dims, or a paletted format with no loaded TLUT would send the tiled
        // decoder reading wild memory (a SEGV). An unrenderable texmap falls back to the
        // colour-only (passthrough) batch — not a silent nil, it's the GX white default.
        auto fmt_ok = [](int f){ return f==0||f==1||f==2||f==3||f==4||f==5||f==6||f==8||f==9||f==0xA||f==0xE; };
        auto paletted = [](int f){ return f==8||f==9||f==0xA; };
        bool decodable = ib.textured && ib.image && fmt_ok(ib.fmt) &&
                         ib.w > 0 && ib.h > 0 && ib.w <= 4096 && ib.h <= 4096 &&
                         (!paletted(ib.fmt) || ib.tlut);
        if (ib.image && sb_boot_capture_texsrc_is_efb_dest(ib.image)) {
            // EFB-copy snapshot consumer (the file-select soft-focus/bloom quad): bind the live
            // framebuffer snapshot keyed by ib.image (registered by the segmented present at the
            // copy boundary) instead of decoding stale guest RAM. This — with honoring the EFB clear
            // — is the overbright fix: the post-pass composites over the REAL scene, not flat white.
            b.tex[0].efb_src = ib.image;
            b.tex[0].w = (uint32_t)ib.w; b.tex[0].h = (uint32_t)ib.h;
            b.tex[0].linear = 1; b.tex[0].min_filter = 1; b.tex[0].wrap_s = 0; b.tex[0].wrap_t = 0;
            ++n_tex_batches;
            static const bool forceMod = [](){ const char* v = std::getenv("SB_IMM_MODULATE"); return v && v[0] && v[0] != '0'; }();
            if (!forceMod && ib.tev && ib.tev->num_stages > 0) {
                uint64_t key; b.fragGlsl = imm_tev_fragment(*ib.tev, key); b.shaderKey = key;
                for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) {
                    b.push.tevreg[c][k] = ib.tev->tev_color[c][k];
                    b.push.kcolor[c][k] = ib.tev->kcolor[c][k];
                }
            } else { b.fragGlsl = g_tex_frag.c_str(); b.shaderKey = g_tex_key; }
            batches.push_back(b);
            const char* d = std::getenv("SB_J3D_DBG");
            if (d && d[0] && d[0] != '0') { static long n=0; if (n++<8)
                std::fprintf(stderr, "[imm] EFB-SAMPLER imm batch %d image=%p %dx%d fmt=%d blend=%d/%d/%d "
                             "-> live snapshot bound\n", bi, ib.image, ib.w, ib.h, ib.fmt,
                             ib.blendType, ib.blendSrc, ib.blendDst); }
            continue;
        }
        if (ib.textured && !decodable) {
            static long s_rej = 0;
            if (s_rej < 20) { ++s_rej;
                std::fprintf(stderr, "[imm-tex] REJECT fmt=0x%x %dx%d img=%p tlut=%p — colour-only\n",
                             ib.fmt, ib.w, ib.h, ib.image, ib.tlut); }
        }
        if (decodable) {
            int idx;
            auto it = tex_cache.find(ib.image);
            if (it != tex_cache.end()) {
                idx = it->second;
            } else {
                // Decode at block-padded dims, then copy the logical w×h sub-rect into a
                // tightly-packed buffer (avoids the padding-column UV leak — the white-window
                // class fixed for the old ngx renderer; nvk uploads packed at logical dims).
                const int pw = sb_tex_pad_w(ib.w, ib.fmt), ph = sb_tex_pad_h(ib.h, ib.fmt);
                static const bool dbg = [](){ const char* v = std::getenv("SB_J3D_DBG"); return v && v[0] && v[0] != '0'; }();
                static long s_dbg = 0;
                if (dbg && s_dbg < 32) { ++s_dbg;
                    std::fprintf(stderr, "[imm-tex] decode fmt=0x%x %dx%d (pad %dx%d) img=%p tlut=%p linear=%d\n",
                                 ib.fmt, ib.w, ib.h, pw, ph, ib.image, ib.tlut, ib.linear); }
                std::vector<uint32_t> padded((size_t)pw * ph);
                sb_tex_decode(padded.data(), (const uint8_t*)ib.image, pw, ph, ib.fmt,
                              (const uint8_t*)ib.tlut, ib.tlutfmt);
                std::vector<uint32_t> packed((size_t)ib.w * ib.h);
                for (int y = 0; y < ib.h; ++y)
                    std::memcpy(&packed[(size_t)y * ib.w], &padded[(size_t)y * pw],
                                (size_t)ib.w * 4);
                idx = (int)tex_storage.size();
                tex_storage.push_back(std::move(packed));
                tex_cache.emplace(ib.image, idx);
            }
            b.tex[0].rgba = (const uint8_t*)tex_storage[idx].data();
            b.tex[0].w = ib.w; b.tex[0].h = ib.h; b.tex[0].linear = ib.linear;
            ++n_tex_batches;
            // Honor the REAL captured combiner (J2DPicture's intensity→black/white ramp +
            // corner-colour modulate) instead of the texture-modulate approximation, which
            // rendered the black cinematic letterbox bars (ramp output = black) as white.
            // SB_IMM_MODULATE forces the old hardcoded modulate for A/B bisection.
            static const bool forceMod = [](){ const char* v = std::getenv("SB_IMM_MODULATE"); return v && v[0] && v[0] != '0'; }();
            if (!forceMod && ib.tev && ib.tev->num_stages > 0) {
                uint64_t key; b.fragGlsl = imm_tev_fragment(*ib.tev, key); b.shaderKey = key;
                for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) {
                    b.push.tevreg[c][k] = ib.tev->tev_color[c][k];
                    b.push.kcolor[c][k] = ib.tev->kcolor[c][k];
                }
            } else {
                b.fragGlsl = g_tex_frag.c_str(); b.shaderKey = g_tex_key;
            }
        } else {
            b.fragGlsl = g_pass_frag.c_str(); b.shaderKey = g_pass_key;
        }
        batches.push_back(b);
    }

    // SB_IMM_DBG: per-immediate-batch screen coverage + mean colour + textured/blend state.
    // The 2D imm path is NOT filtered by SB_SKIP_KEY, so this localizes a HUD/fader/letterbox
    // quad (e.g. the pure-white bottom band that survives a scene-key skip).
    static const int immdbg = [](){ const char* v = std::getenv("SB_IMM_DBG"); return v && v[0] ? std::atoi(v) : 0; }();
    static bool s_immdone = false;
    if (immdbg && !s_immdone && nimm > 0 && (immdbg == 1 || g_frame >= immdbg)) {
        s_immdone = true;
        for (int bi = 0; bi < nibatch; ++bi) {
            const SbImmBatch& ib = ibatches[bi];
            float xmn=1e30f,xmx=-1e30f,ymn=1e30f,ymx=-1e30f; double r=0,g=0,bl=0,a=0; uint32_t n=0;
            for (uint32_t i = ib.vstart; i < ib.vstart + ib.vcount && (int)i < nimm; ++i) {
                const SbImmVtx& v = imm[i];
                if (v.x<xmn)xmn=v.x; if (v.x>xmx)xmx=v.x; if (v.y<ymn)ymn=v.y; if (v.y>ymx)ymx=v.y;
                r+=v.r; g+=v.g; bl+=v.b; a+=v.a; ++n;
            }
            if (!n) continue;
            std::fprintf(stderr, "[immdbg] ib%d vc=%u x[%.3f,%.3f] y[%.3f,%.3f] rgb=%.2f,%.2f,%.2f a=%.2f "
                         "tex=%d fmt=0x%x %dx%d blend=%d/%d/%d tev_ns=%d c0env=%08x C1=%d,%d,%d,%d\n",
                         bi, ib.vcount, xmn,xmx,ymn,ymx, r/n,g/n,bl/n,a/n,
                         (int)ib.textured, ib.fmt, ib.w, ib.h, ib.blendType, ib.blendSrc, ib.blendDst,
                         ib.tev ? ib.tev->num_stages : -1,
                         ib.tev ? ib.tev->stage[0].color_env : 0u,
                         ib.tev?ib.tev->tev_color[2][0]:0, ib.tev?ib.tev->tev_color[2][1]:0,
                         ib.tev?ib.tev->tev_color[2][2]:0, ib.tev?ib.tev->tev_color[2][3]:0);
            if (ib.vcount <= 24) for (uint32_t i = ib.vstart; i < ib.vstart + ib.vcount && (int)i < nimm; ++i) {
                const SbImmVtx& v = imm[i];
                std::fprintf(stderr, "        v%u (%.3f,%.3f,%.3f) uv(%.2f,%.2f) rgba=%.2f,%.2f,%.2f,%.2f\n",
                             i-ib.vstart, v.x,v.y,v.z, v.u,v.v, v.r,v.g,v.b,v.a);
            }
        }
    }

    static int s_bbox = 0;
    if (s_bbox < 2 && nscene > 0) {
        ++s_bbox;
        float mnx=1e30f,mxx=-1e30f,mny=1e30f,mxy=-1e30f,mnz=1e30f,mxz=-1e30f;
        for (int i = 0; i < nscene; ++i) {
            const NvkTevVertex& v = verts[i];
            if (v.x<mnx)mnx=v.x; if (v.x>mxx)mxx=v.x; if (v.y<mny)mny=v.y; if (v.y>mxy)mxy=v.y;
            if (v.z<mnz)mnz=v.z; if (v.z>mxz)mxz=v.z;
        }
        std::fprintf(stderr, "[bbox] scene NDC x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f] nbatch=%d b0(vs=%u vc=%u rgba=%.2f,%.2f,%.2f,%.2f)\n",
                     mnx,mxx,mny,mxy,mnz,mxz, nsbatch,
                     batches[0].vstart, batches[0].vcount,
                     verts[0].rgba[0],verts[0].rgba[1],verts[0].rgba[2],verts[0].rgba[3]);
    }
    // SB_BATCH_DBG: per-scene-batch NDC-z + screen-y span + depth/blend state, printed once.
    // Localizes which surfaces overlap in depth (the horizon z-fight class).
    static const int batchdbg = [](){ const char* v = std::getenv("SB_BATCH_DBG"); return v && v[0] ? std::atoi(v) : 0; }();
    static bool s_batchdone = false;
    // SB_BATCH_DBG=1 → first scene frame; SB_BATCH_DBG=N>1 → fire once at present frame >= N;
    // SB_BATCH_DBG=-1 → fire once the camera has SETTLED (the choice-state scene, ~frame 1690 —
    // the only way to catch the settled sea/foam batches, since the present-frame number at settle
    // isn't known ahead of time). The intro pan never "settles", so this can't fire early.
    const bool settle_trig = (batchdbg < 0 && sb_camera_view_settled());
    if (batchdbg && !s_batchdone && nscene > 0 && (batchdbg == 1 || (batchdbg > 1 && g_frame >= batchdbg) || settle_trig)) {
        s_batchdone = true;
        for (int bi = 0; bi < nsbatch; ++bi) {
            const NvkTevBatch& b = batches[bi];
            float zmn=1e30f,zmx=-1e30f,zsum=0; float ymn=1e30f,ymx=-1e30f,xmn=1e30f,xmx=-1e30f;
            double rr=0,gg=0,bb=0,aa=0,amn=1e30,amx=-1e30,r2=0,g2=0,b2=0; uint32_t cnt=0;
            for (uint32_t i = b.vstart; i < b.vstart + b.vcount && i < verts.size(); ++i) {
                const NvkTevVertex& v = verts[i];
                if (v.z<zmn)zmn=v.z; if (v.z>zmx)zmx=v.z; zsum+=v.z;
                if (v.y<ymn)ymn=v.y; if (v.y>ymx)ymx=v.y;
                if (v.x<xmn)xmn=v.x; if (v.x>xmx)xmx=v.x;
                rr+=v.rgba[0]; gg+=v.rgba[1]; bb+=v.rgba[2];
                aa+=v.rgba[3]; if(v.rgba[3]<amn)amn=v.rgba[3]; if(v.rgba[3]>amx)amx=v.rgba[3];
                r2+=v.rgba[0]*v.rgba[0]; g2+=v.rgba[1]*v.rgba[1]; b2+=v.rgba[2]*v.rgba[2]; ++cnt;
            }
            if (!cnt) continue;
            double mr=rr/cnt,mg=gg/cnt,mb=bb/cnt,ma=aa/cnt;
            double var=(r2/cnt-mr*mr)+(g2/cnt-mg*mg)+(b2/cnt-mb*mb);  // color variance (rainbow letters高い)
            // Bound-texture inventory: count non-null samplers + first tex dims (white-untextured triage).
            int ntex=0; char texinfo[96]="-"; for (int t=0;t<8;++t) if (b.tex[t].rgba) { if(!ntex) std::snprintf(texinfo,sizeof texinfo,"t%d=%ux%u minF=%u mag=%u wrap=%u/%u",t,b.tex[t].w,b.tex[t].h,b.tex[t].min_filter,b.tex[t].linear,b.tex[t].wrap_s,b.tex[t].wrap_t); ++ntex; }
            // mean UV span over the batch's first texcoord set (degenerate UV → untextured-looking).
            float umn=1e30f,umx=-1e30f,vmn=1e30f,vmx=-1e30f; for (uint32_t i=b.vstart;i<b.vstart+b.vcount&&i<verts.size();++i){const NvkTevVertex&v=verts[i]; float u=v.uv[0][0],w2=v.uv[0][1]; if(u<umn)umn=u; if(u>umx)umx=u; if(w2<vmn)vmn=w2; if(w2>vmx)vmx=w2;}
            std::fprintf(stderr, "[batchdbg] b%d ph%u vc=%u z[%.5f,%.5f]m%.5f ndcX[%.3f,%.3f] ndcY[%.3f,%.3f] "
                         "zt=%u zf=%u zw=%u bm=%u/%u/%u rgb=%.2f,%.2f,%.2f a=%.2f[%.2f,%.2f] cvar=%.3f ntex=%d %s uv[%.2f,%.2f;%.2f,%.2f] key=%llx\n",
                         bi, b.phase, b.vcount, zmn, zmx, zsum/cnt, xmn, xmx, ymn, ymx,
                         b.z_test, b.z_func, b.z_write, b.blend_mode, b.src_factor, b.dst_factor,
                         mr, mg, mb, ma, amn, amx, var, ntex, texinfo, umn,umx,vmn,vmx, (unsigned long long)b.shaderKey);
            // TEV value oracle (native side): the konst colours + S10 TEV colour registers fed to
            // the generated combiner, plus whether each bound texmap is an EFB-copy snapshot. This is
            // the native counterpart of the Dolphin-GX per-draw blend/TEV oracle (gx_capture.cpp
            // SUNBRIGHT_DBG_GXBLEND) — diff the two for the post-pass composite (b76 = the file-select
            // overbright). A near-white konst/tevreg fed to a SRCALPHA/SRCCLR blend over a dark texture
            // is the over-bright signature.
            // Decoded-texture mean RGBA per slot (the DECISIVE datum for the b76 wash: is the sea
            // reflection texture native decodes actually near-white?). Mean over the whole tex.
            auto texmean = [](const sb::render::NvkTevBatch::Tex& t, int out[4]){
                out[0]=out[1]=out[2]=out[3]=-1;
                if (!t.rgba || !t.w || !t.h) return;
                unsigned long long s0=0,s1=0,s2=0,s3=0; const uint8_t* p=t.rgba;
                size_t n=(size_t)t.w*t.h; for (size_t i=0;i<n;++i){ s0+=p[i*4];s1+=p[i*4+1];s2+=p[i*4+2];s3+=p[i*4+3]; }
                out[0]=(int)(s0/n); out[1]=(int)(s1/n); out[2]=(int)(s2/n); out[3]=(int)(s3/n);
            };
            int tm0[4], tm1[4]; texmean(b.tex[0], tm0); texmean(b.tex[1], tm1);
            if ((unsigned)(b.shaderKey >> 32) == 0xeb5c8e74u && b.fragGlsl)
                std::fprintf(stderr, "[b76-glsl]\n%s\n[/b76-glsl]\n", b.fragGlsl);
            std::fprintf(stderr, "[batchtev] b%d cU=%u aU=%u k0=%d,%d,%d,%d k1=%d,%d,%d,%d r0=%d,%d,%d,%d r1=%d,%d,%d,%d efb=%p/%p texmean0=%d,%d,%d,%d texmean1=%d,%d,%d,%d\n",
                         bi, b.color_update, b.alpha_update,
                         b.push.kcolor[0][0],b.push.kcolor[0][1],b.push.kcolor[0][2],b.push.kcolor[0][3],
                         b.push.kcolor[1][0],b.push.kcolor[1][1],b.push.kcolor[1][2],b.push.kcolor[1][3],
                         b.push.tevreg[0][0],b.push.tevreg[0][1],b.push.tevreg[0][2],b.push.tevreg[0][3],
                         b.push.tevreg[1][0],b.push.tevreg[1][1],b.push.tevreg[1][2],b.push.tevreg[1][3],
                         b.tex[0].efb_src, b.tex[1].efb_src,
                         tm0[0],tm0[1],tm0[2],tm0[3], tm1[0],tm1[1],tm1[2],tm1[3]);
            std::fprintf(stderr, "[batchbuf] b%d drawbuf=\"%s\"\n", bi, b.dbgName ? b.dbgName : "(none)");
            // Dump the generated TEV fragment shader for the composite batches (dst_factor SRCCLR/
            // INVSRCCLR — the over-bright class) so the combiner that produces `src` is inspectable.
            if (b.fragGlsl && (b.dst_factor == 2 || b.dst_factor == 3)) {
                char pf[128]; std::snprintf(pf, sizeof pf, "scratch/frames/bfrag_%02d.glsl", bi);
                if (FILE* f = fopen(pf, "w")) { fputs(b.fragGlsl, f); fclose(f); }
            }
            // Dump each small textured batch's tex0 (RGB ppm + alpha as grayscale ppm) so a
            // wash/ray layer's texture content + alpha falloff is visible, not guessed.
            for (int ti=0; ti<8; ++ti) {
                if (!(b.tex[ti].rgba && b.tex[ti].w>0 && b.tex[ti].h>0 && b.tex[ti].w<=256 && b.tex[ti].h<=256)) continue;
                uint32_t w=b.tex[ti].w,h=b.tex[ti].h; const uint8_t* px=b.tex[ti].rgba;
                char p0[128],p1[128];
                std::snprintf(p0,sizeof p0,"scratch/frames/btex_%02d_t%d_rgb.ppm",bi,ti);
                std::snprintf(p1,sizeof p1,"scratch/frames/btex_%02d_t%d_a.ppm",bi,ti);
                if (FILE* f=fopen(p0,"wb")){ fprintf(f,"P6\n%u %u\n255\n",w,h);
                    for(uint32_t i=0;i<w*h;++i) fwrite(px+i*4,1,3,f); fclose(f); }
                if (FILE* f=fopen(p1,"wb")){ fprintf(f,"P6\n%u %u\n255\n",w,h);
                    for(uint32_t i=0;i<w*h;++i){ uint8_t a=px[i*4+3]; uint8_t t[3]={a,a,a}; fwrite(t,1,3,f);} fclose(f); }
            }
        }
    }
    // g_frame was already incremented above in every branch, so the frame number that actually
    // satisfied the dump window is g_frame-1. Name the file by THAT so SB_FRAME_DUMP_START=240
    // writes boot_0240.ppm (not boot_0241): the off-by-one made every START=N run leave the
    // boot_000N.ppm STALE while writing boot_000(N+1), so analysis read a leftover frame.
    const int df = g_frame - 1;

    // ── Render the frame through the SDL3 GPU backend — the GX seam's ONLY renderer (the nvk
    // Vulkan rasterizer was retired; docs/gx_sdlgpu_switch.md). ──
    // The GC renders the file-select frame across multiple EFB targets (a pre-pass/mirror copied to
    // a texture + an EFB CLEAR, the main scene copied to a texture, then a post pass whose full-screen
    // quads SAMPLE those copy textures). Honor that structure: render the scene in SEGMENTS split at
    // the EFB-copy boundaries, snapshot the framebuffer at each (so a consumer samples the REAL scene,
    // not stale RAM → the flat-white wash), and honor the EFB clear (so the off-screen pre-pass/mirror
    // stops double-compositing → the additive overbright). See the 2026-06-30 overbright journal.
    SbEfbCopy copies[8];
    int ncopies = sb_boot_capture_efb_copy_count();
    if (ncopies > 0) ncopies = sb_boot_capture_efb_copies(copies, 8);

    // SB_SEG_DUMP=1: diagnostic — render the cumulative scene up to each EFB-copy boundary as its
    // own single-pass frame and dump it (seg_NN_cumXX.ppm), so the per-segment composition is
    // VISIBLE (which segment owns the sky checker / sea wash). Drilling tool, off by default.
    static const bool segDump = [](){ const char* v = std::getenv("SB_SEG_DUMP"); return v && v[0] && v[0] != '0'; }();
    if (segDump && dumpThis && ncopies > 0 && !sceneFiltered) {
        static std::vector<uint8_t> seg_pix;
        auto dump_range = [&](int lo, int hi, const char* tag){
            sb::gxsdl::frame_begin(c[0], c[1], c[2], c[3]);
            sb::gxsdl::draw_tev(verts.data(), (int)verts.size(),
                                batches.data() + lo, hi - lo);
            sb::gxsdl::frame_end();
            seg_pix.assign((size_t)kW * kH * 4, 0);
            sb::gxsdl::readback(seg_pix.data(), (int)kW, (int)kH);
            char p[160]; std::snprintf(p, sizeof p, "scratch/frames/seg_%04d_%s.ppm", df, tag);
            write_ppm_buf(p, seg_pix.data(), kW, kH);
            std::fprintf(stderr, "[segdump] %s [%d,%d)\n", tag, lo, hi);
        };
        int prev = 0;
        for (int k = 0; k < ncopies; ++k) {
            int bnd = copies[k].batch_index; if (bnd > nScenePushed) bnd = nScenePushed;
            char t[32]; std::snprintf(t, sizeof t, "A%d_only", k); dump_range(prev, bnd, t);
            prev = bnd;
        }
        dump_range(prev, (int)batches.size(), "tail_only");
        dump_range(0, nScenePushed, "scene_all");
    }

    // SB_PASS_DUMP=1: the NATIVE side of the per-pass A/B differ (tools/render/passdiff.py). Dump
    // native's CUMULATIVE framebuffer at each EFB-copy boundary — pass{k}_native = everything drawn
    // up to copy k (the off-screen render-to-texture content at that boundary), the direct counterpart
    // of the oracle's Dolphin-decoded efb1_n{k} dump (SUNBRIGHT_DUMP_EFB). pass3 (the visible frame)
    // is the normal present (boot_NNNN.ppm). Comparing pass-by-pass localizes the FIRST divergent pass
    // as an IMAGE — the interleaved divergence detector that replaces the per-process aggregate diff.
    static const bool passDump = [](){ const char* v = std::getenv("SB_PASS_DUMP"); return v && v[0] && v[0] != '0'; }();
    if (passDump && dumpThis && ncopies > 0 && !sceneFiltered) {
        static std::vector<uint8_t> pass_pix;
        for (int k = 0; k < ncopies; ++k) {
            int bnd = copies[k].batch_index; if (bnd > nScenePushed) bnd = nScenePushed; if (bnd < 0) bnd = 0;
            sb::gxsdl::frame_begin(c[0], c[1], c[2], c[3]);
            sb::gxsdl::draw_tev(verts.data(), (int)verts.size(), batches.data(), bnd);
            sb::gxsdl::frame_end();
            pass_pix.assign((size_t)kW * kH * 4, 0);
            sb::gxsdl::readback(pass_pix.data(), (int)kW, (int)kH);
            char p[160]; std::snprintf(p, sizeof p, "scratch/frames/pass%d_native_%04d.ppm", k + 1, df);
            write_ppm_buf(p, pass_pix.data(), kW, kH);
            std::fprintf(stderr, "[passdump] pass%d_native = cumulative [0,%d) %dx%d copy(dest=%p %dx%d clear=%d)\n",
                         k + 1, bnd, kW, kH, copies[k].dest, copies[k].wd, copies[k].ht, copies[k].clear);
        }
    }

    // SB_BLEND_DRILL=1: attribute the per-pass blend OVERBRIGHT (bug #1) to a specific blend class in
    // ONE run (a native run is ~4 min, so N separate SB_ABLATE_BM runs is too slow). Operates on the
    // MAIN-pass batches only (phase != 1) to isolate the single scene render from the ph1 mirror
    // double-draw AND the imm soft-focus quad — so the dumps are directly comparable to the oracle's
    // 320x224 pass2 (the clean single scene). For every distinct (src,dst) blend factor pair among the
    // main-pass blended batches it dumps: drill_base (all main-pass), drill_drop_S_D (that pair removed),
    // drill_only_S_D (only that pair, over the opaque base). tools/render/blend_drill.py then region-
    // diffs each vs oracle_pass2 and ranks which class's brightness dominates the sky/sea overbright.
    static const bool blendDrill = [](){ const char* v = std::getenv("SB_BLEND_DRILL"); return v && v[0] && v[0] != '0'; }();
    if (blendDrill && dumpThis && !sceneFiltered) {
        static std::vector<uint8_t> drill_pix;
        // main-pass batches (phase != 1), in order
        std::vector<int> mainIdx;
        for (int i = 0; i < nScenePushed; ++i) if (batches[(size_t)i].phase != 1) mainIdx.push_back(i);
        // distinct (src,dst) among the BLENDED main-pass batches
        std::vector<std::pair<int,int>> sigs;
        for (int i : mainIdx) { const auto& b = batches[(size_t)i];
            if (b.blend_mode == 0) continue;
            std::pair<int,int> s{b.src_factor, b.dst_factor};
            if (std::find(sigs.begin(), sigs.end(), s) == sigs.end()) sigs.push_back(s); }
        auto render_dump = [&](const std::vector<NvkTevBatch>& bs, const char* tag){
            sb::gxsdl::frame_begin(c[0], c[1], c[2], c[3]);
            sb::gxsdl::draw_tev(verts.data(), (int)verts.size(), bs.data(), (int)bs.size());
            sb::gxsdl::frame_end();
            drill_pix.assign((size_t)kW * kH * 4, 0);
            sb::gxsdl::readback(drill_pix.data(), (int)kW, (int)kH);
            char p[160]; std::snprintf(p, sizeof p, "scratch/frames/drill_%s_%04d.ppm", tag, df);
            write_ppm_buf(p, drill_pix.data(), kW, kH);
            std::fprintf(stderr, "[blenddrill] %s : %zu batches\n", tag, bs.size());
        };
        // base = all main-pass batches
        std::vector<NvkTevBatch> base; for (int i : mainIdx) base.push_back(batches[(size_t)i]);
        render_dump(base, "base");
        for (auto [s, d] : sigs) {
            std::vector<NvkTevBatch> drop, only;
            for (int i : mainIdx) { const auto& b = batches[(size_t)i];
                bool match = (b.blend_mode != 0 && b.src_factor == s && b.dst_factor == d);
                if (!match) drop.push_back(b);                 // everything except this class
                if (match || b.blend_mode == 0) only.push_back(b);  // this class over the opaque base
            }
            char td[32], to[32];
            std::snprintf(td, sizeof td, "drop_%d_%d", s, d);
            std::snprintf(to, sizeof to, "only_%d_%d", s, d);
            render_dump(drop, td);
            render_dump(only, to);
        }
        std::fprintf(stderr, "[blenddrill] %zu distinct blend classes among %zu main-pass batches\n",
                     sigs.size(), mainIdx.size());
    }

    // SB_FS_CANDS=1: ONE-RUN composite-structure sweep. The file-select frame is a render-to-EFB-
    // texture composite (journal 2026-06-30): the scene renders OFF-SCREEN to スクリーンテクスチャ
    // and the visible frame is composite3 (an imm quad SAMPLING that texture) + 2D menu. native's
    // capture splits the scene into ph1 (unk40 drawBufferGroup = whole scene incl palm/blocks), ph4
    // (mPerformListGX main scene), ph6 (GXPost = Mario + composite3). Which off-screen batch set
    // reproduces the clean single scene (no ph1/ph4 double-draw) is an empirical question — dump
    // candidates A..D in ONE run (each: render the candidate off-screen → snapshot to BOTH copy
    // dests → CLEAR → draw the imm overlay, whose soft-focus quad re-displays the snapshot), then
    // compare each scratch/frames/cand_*.ppm vs scratch/passes/oracle_pass3_final.png with
    // fileselect_overbright.py to pick the structure, which becomes the SB_FS_COMPOSITE default.
    static const bool fsCands = [](){ const char* v = std::getenv("SB_FS_CANDS"); return v && v[0] && v[0] != '0'; }();
    if (fsCands && dumpThis && ncopies > 0 && !sceneFiltered) {
        int ph1End2 = 0; while (ph1End2 < nScenePushed && batches[(size_t)ph1End2].phase == 1) ++ph1End2;
        int ph6Start = nScenePushed; for (int i = 0; i < nScenePushed; ++i) if (batches[(size_t)i].phase == 6) { ph6Start = i; break; }
        std::fprintf(stderr, "[fscands] nScene=%d ph1End=%d ph6Start=%d ncopies=%d\n",
                     nScenePushed, ph1End2, ph6Start, ncopies);
        static std::vector<uint8_t> cand_pix;
        const int total = (int)batches.size();
        auto render_cand = [&](const std::vector<NvkTevBatch>& off, const char* tag){
            sb::gxsdl::frame_begin(c[0], c[1], c[2], c[3]);
            // 1) render the candidate scene OFF-SCREEN (into g_color, not shown)
            sb::gxsdl::draw_tev_segment(verts.data(), (int)verts.size(),
                                        off.data(), (int)off.size(), /*clearFirst=*/true);
            // 2) snapshot to every EFB-copy dest so whichever the imm quad samples re-displays it
            for (int k = 0; k < ncopies; ++k) sb::gxsdl::snapshot_efb(copies[k].dest);
            // 3) CLEAR the visible FB and draw ONLY the imm overlay (soft-focus quad samples snapshot)
            sb::gxsdl::draw_tev_segment(verts.data(), (int)verts.size(),
                                        batches.data() + nScenePushed, total - nScenePushed,
                                        /*clearFirst=*/true);
            sb::gxsdl::frame_end();
            cand_pix.assign((size_t)kW * kH * 4, 0);
            sb::gxsdl::readback(cand_pix.data(), (int)kW, (int)kH);
            char p[160]; std::snprintf(p, sizeof p, "scratch/frames/cand_%s_%04d.ppm", tag, df);
            write_ppm_buf(p, cand_pix.data(), kW, kH);
            std::fprintf(stderr, "[fscands] %s : %zu offscreen batches\n", tag, off.size());
        };
        auto slice = [&](int lo, int hi){ std::vector<NvkTevBatch> v;
            for (int i = lo; i < hi && i < nScenePushed; ++i) v.push_back(batches[(size_t)i]); return v; };
        // A = whole scene (ph1+ph4+ph6) off-screen
        render_cand(slice(0, nScenePushed), "A_all");
        // B = ph1 only (the drawBufferGroup whole-scene flush)
        render_cand(slice(0, ph1End2), "B_ph1");
        // C = ph4+ph6 (no ph1)
        render_cand(slice(ph1End2, nScenePushed), "C_ph46");
        // D = ph1 + ph6 (whole-scene flush + Mario, skip the ph4 redundant main re-draw)
        { std::vector<NvkTevBatch> v = slice(0, ph1End2);
          for (int i = ph6Start; i < nScenePushed; ++i) v.push_back(batches[(size_t)i]);
          render_cand(v, "D_ph1_6"); }
    }

    // SB_EFB_HONOR_CLEAR_SEG (default OFF): honor the EFB *clear* between segments. The GC clears the
    // EFB after the pre-pass/mirror copy so that off-screen render does NOT stay in the visible frame.
    // Honoring it is only correct once native's main pass redraws everything the pre-pass held — until
    // then a clearing copy DROPS content native captured only in the pre-pass segment (e.g. the
    // file-select palm tree, captured at scene index < 49). So default to CUMULATIVE compositing (LOAD
    // every segment) — same coverage as the single pass — while still snapshotting at each boundary so
    // the post-pass EFB-sampler quads (the soft-focus) composite over the REAL accumulated scene
    // instead of stale white. Flip this on once the per-pass geometry split is faithful.
    static const bool honorClearSeg = [](){ const char* v = std::getenv("SB_EFB_HONOR_CLEAR_SEG"); return v && v[0] && v[0] != '0'; }();
    // SB_FS_NOCLEAR=1: revert to the OLD cumulative compositing (no clear between segments) for A/B.
    // Default = the file-select composite fix: render the ph1 (unk40) MIRROR pre-pass OFF-SCREEN and
    // CLEAR at the ph1/ph4 PHASE boundary, so the main scene (ph4+ph6) starts on a clean framebuffer
    // and the soft-focus snapshot (texB) holds the scene ONCE — not the ph1+ph4 double-draw that made
    // the file-select overbright. The crux vs the retired SB_EFB_HONOR_CLEAR_SEG: that cleared at the
    // mirror COPY's recorded batch index (which lands mid-ph4 and dropped ph4 content); this clears at
    // the real phase boundary (the last phase==1 batch), so all of ph4 is preserved. The mirror is
    // near-empty on file-select, so dropping it from the visible FB is faithful (its snapshot still
    // feeds the sea reflection sampler). See the 2026-07-01 per-pass-differ journal.
    // OPT-IN (default OFF): the phase-boundary off-screen composite is WIP — it correctly removes the
    // ph1 double-draw, but (a) native renders the palm + A/B/C blocks in ph1 (not ph4/ph6), so clearing
    // ph1 drops them, and (b) ph4+ph6 alone is STILL overbright (an intrinsic per-pass additive-blend
    // issue, not just the double-draw). Both must be fixed before this can be the default. See the
    // 2026-07-01 per-pass-differ journal. Until then default to the legacy cumulative composite.
    static const bool fsComposite = [](){ const char* v = std::getenv("SB_FS_COMPOSITE"); return v && v[0] && v[0] != '0'; }();
    const bool fsNoClear = !fsComposite;
    // The ph1/ph4 boundary: ph1 (the unk40 mirror pre-pass) batches are captured first and carry
    // phase==1. ph1End = one past the last leading phase==1 batch (0 if none → no special handling).
    int ph1End = 0;
    while (ph1End < nScenePushed && batches[(size_t)ph1End].phase == 1) ++ph1End;
    sb::gxsdl::frame_begin(c[0], c[1], c[2], c[3]);
    if (ncopies > 0 && !sceneFiltered) {
        const int total = (int)batches.size();
        int  segStart = 0;
        bool clearFirst = true;                       // segment 0 starts on the frame clear
        for (int k = 0; k < ncopies; ++k) {
            // The mirror copy (256x256) bounds the ph1 pre-pass: snapshot it at the PHASE boundary
            // (ph1End) — NOT its recorded batch index (which lands mid-ph4 because native's perform-
            // list ordering differs from GC's copy timing) — and CLEAR after, so the off-screen mirror
            // is not composited into the visible frame. The SOFT-FOCUS copy (the scene's 通常シーン
            // texture that the post-pass quad re-displays = the actual visible image) must snapshot the
            // WHOLE main scene, so bound it at nScenePushed (after ALL of ph4+ph6), not its recorded
            // mid-scene index — otherwise texB holds a half-drawn scene and the soft-focus quad shows
            // garbage (the palm-loss + white wash seen when bound=recorded index).
            const bool useFix  = !fsNoClear && ph1End > 0;
            const bool isMirror = useFix && copies[k].wd == 256 && copies[k].ht == 256;
            int bound;
            if (isMirror)      bound = ph1End;
            else if (useFix)   bound = nScenePushed;          // soft-focus → snapshot the full scene
            else               bound = copies[k].batch_index; // legacy (SB_FS_NOCLEAR)
            if (bound > nScenePushed) bound = nScenePushed;   // copy at scene-list end → imm follows
            if (bound < segStart)     bound = segStart;
            sb::gxsdl::draw_tev_segment(verts.data(), (int)verts.size(),
                                        batches.data() + segStart, bound - segStart, clearFirst);
            sb::gxsdl::snapshot_efb(copies[k].dest);          // snapshot this segment for its consumer
            segStart   = bound;
            // Clear after the mirror pre-pass (GC clears the EFB post-mirror-copy); otherwise honor the
            // legacy SB_EFB_HONOR_CLEAR_SEG knob (still default-off for non-mirror copies).
            clearFirst = isMirror || (honorClearSeg && (copies[k].clear != 0));
        }
        // Final segment: any scene tail past the last copy + the 2D imm overlay (the post pass).
        sb::gxsdl::draw_tev_segment(verts.data(), (int)verts.size(),
                                    batches.data() + segStart, total - segStart, clearFirst);
    } else {
        sb::gxsdl::draw_tev(verts.data(), (int)verts.size(), batches.data(), (int)batches.size());
    }
    sb::gxsdl::frame_end();
    static std::vector<uint8_t> present_pix;
    present_pix.assign((size_t)kW * kH * 4, 0);
    sb::gxsdl::readback(present_pix.data(), (int)kW, (int)kH);

    if (dumpThis) {
    char path[160];
    std::snprintf(path, sizeof path, "scratch/frames/boot_%04d.ppm", df);
    write_ppm_buf(path, present_pix.data(), kW, kH);

    // SB_CAM_DUMP: alongside the PPM, write the exact view+proj the renderer used (cam_NNNN.txt)
    // so the Dolphin-GX oracle can be transplanted to the SAME viewpoint (rung-6 camera align).
    static const bool camDump = [](){ const char* v = std::getenv("SB_CAM_DUMP"); return v && v[0] && v[0] != '0'; }();
    if (camDump && (&sb_boot_dump_camera)) sb_boot_dump_camera(df);
    std::printf("[present] frame %d clear=(%.2f,%.2f,%.2f,%.2f) scene_verts=%d scene_batches=%d "
                "imm_tris=%d imm_batches=%d (textured=%d) -> %s\n",
                df, c[0], c[1], c[2], c[3], nscene, nsbatch, nimm / 3, nibatch,
                n_tex_batches, path);
    std::fflush(stdout);   // survive a SIGKILL'd headless run — the per-present batch count is
                           // our parity progress metric; block-buffered stdout would otherwise drop it
    ++g_dumped;

    // VALUE-TRACK parity dump (SB_PARITY_DUMP=path): one JSON line per dumped frame with the
    // scene geometry (per-batch NDC AABB / checksum / bad-vertex count), lighting, and XFB region
    // grid — consumed by tools/render/parity_sweep.py (check / diff). Pairs 1:1 with the PPM.
    static std::FILE* s_parity = [](){ const char* p = std::getenv("SB_PARITY_DUMP");
        return (p && p[0]) ? std::fopen(p, "w") : nullptr; }();
    if (s_parity) {
        SbParityLights L{};
        if (sb_gx_get_lights) {
            float lraw[8][16]; const int nl = sb_gx_get_lights(lraw);
            for (int i = 0; i < nl && i < 8; ++i) {
                if (lraw[i][0] == 0.f) continue;
                L.col[L.n][0]=lraw[i][1]; L.col[L.n][1]=lraw[i][2]; L.col[L.n][2]=lraw[i][3];
                L.pos[L.n][0]=lraw[i][4]; L.pos[L.n][1]=lraw[i][5]; L.pos[L.n][2]=lraw[i][6];
                ++L.n;
            }
        }
        if (sb_gx_get_chan_amb)      sb_gx_get_chan_amb(0, L.amb);
        if (sb_gx_get_chan_matcolor) sb_gx_get_chan_matcolor(0, L.matc);
        SbParityProj P{};
        if (sb_gx_get_projection) sb_gx_get_projection(&P.type, P.proj, P.vp);
        sb_parity_emit(s_parity, df, c, verts.data(), (int)verts.size(),
                       batches.data(), (int)batches.size(), L, P,
                       present_pix.data(), (int)kW, (int)kH);
    }
    } // if (dumpThis)

    // SB_WINDOW: the game thread does NOT present — it only renders into g_cpu (above, every frame in
    // windowMode). The SDL MAIN thread (boot.cpp's window loop) calls present_window() to show g_cpu.
    // Presenting here would be on the game/scheduler thread → the X11/WSI crash+deadlock we fixed.
}

} // namespace

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
            if (const char* s = std::getenv("SB_FRAME_DUMP_SETTLE"))
                g_settle_dump = (s[0] && s[0] != '0');
            ::mkdir("scratch", 0755);
            ::mkdir("scratch/frames", 0755);
        }
    }
    sb_vi_set_present_hook(present_hook, nullptr);
}

// Event-triggered frame dump: request the next N presented frames be written to
// scratch/frames/ regardless of the SB_FRAME_DUMP_START window. Used to capture a
// specific game state (e.g. TCardLoad reaching the file-select mState 0) whose present
// frame number isn't known ahead of time.
extern "C" void sb_boot_request_dump(int n) {
    if (n <= 0) return;
    ::mkdir("scratch", 0755);
    ::mkdir("scratch/frames", 0755);
    g_request_dump = n;
}

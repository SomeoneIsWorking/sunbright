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
#include "nvk.h"
#include "gx_imm_xform.h"      // SbImmVtx / SbImmBatch (immediate-mode capture)
#include "ngx_render_data.h"   // NgxTevState (for the 2D passthrough / modulate shaders)
#include "tev_shader.h"        // sb_tev_gen_fragment
#include "tex_decode.h"        // sb_tex_decode (J2D textured-pane decode)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

using namespace sb::render;

extern "C" {
void sb_vi_set_present_hook(void (*fn)(void*, void*), void* user);
void sb_gx_get_clear_color(float* rgba);
int  sb_gx_imm_take(const SbImmVtx** out);
int  sb_gx_imm_take_batches(const SbImmVtx** verts, const SbImmBatch** batches, int* nbatch);
}
// The frame's captured live J3D scene: vertex list + per-material TEV batches. WEAK — null when
// the capture TU (sms_boot_j3d_capture.cpp) isn't linked, in which case the present draws only 2D.
int sb_boot_capture_tev_take(const NvkTevVertex** verts,
                             const Nvk::NvkTevBatch** batches, int* nbatches) __attribute__((weak));

namespace {

Nvk g_nvk;
bool g_init_tried = false;
bool g_init_ok = false;
int g_frame = 0;
int g_max_dump = 0;
int g_start_dump = 0;
int g_dumped = 0;
bool g_on_scene = false;
bool g_dump_started = false;
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
    // SB_BBOX_TRACE: map the captured scene's NDC bbox across frames (no GPU, no dump) to
    // see whether/when the camera brings geometry in-view.
    static const bool bbtrace = [](){ const char* v = std::getenv("SB_BBOX_TRACE"); return v && v[0] && v[0] != '0'; }();
    if (bbtrace) {
        const NvkTevVertex* sv = nullptr; const Nvk::NvkTevBatch* sb = nullptr; int nb = 0;
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
    const Nvk::NvkTevBatch* sbatches = nullptr;
    int nsbatch = 0;
    int nscene = (&sb_boot_capture_tev_take) ? sb_boot_capture_tev_take(&scene, &sbatches, &nsbatch) : 0;
    const SbImmVtx* imm = nullptr;
    const SbImmBatch* ibatches = nullptr;
    int nibatch = 0;
    int nimm = sb_gx_imm_take_batches(&imm, &ibatches, &nibatch);

    if (g_request_dump <= 0 && (!g_max_dump || g_dumped >= g_max_dump)) return;

    bool dumpThis;
    if (g_request_dump > 0) {
        // Event-triggered dump (e.g. TCardLoad reaching mState 0): capture the next N
        // presented frames regardless of the start/max window.
        dumpThis = true;
        --g_request_dump;
        ++g_frame;
    } else if (g_on_scene) {
        if (!g_dump_started && nscene > 0) g_dump_started = true;
        dumpThis = g_dump_started;
        ++g_frame;
    } else {
        if (g_frame < g_start_dump) { ++g_frame; return; }
        dumpThis = (g_frame < g_start_dump + g_max_dump);
        ++g_frame;
    }
    if (!dumpThis) return;

    if (!g_init_tried) {
        g_init_tried = true;
        g_init_ok = g_nvk.init(kW, kH) || g_nvk.init(kW, kH, true);
        if (!g_init_ok)
            std::fprintf(stderr, "[present] no Vulkan device (dump disabled)\n");
        else
            std::printf("[present] nvk up (%ux%u) — dumping %d frames to scratch/frames/\n",
                        kW, kH, g_max_dump);
    }
    if (!g_init_ok) { g_max_dump = 0; return; }
    ensure_pass_shader();
    ensure_tex_shader();

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
    std::vector<Nvk::NvkTevBatch> batches;
    batches.reserve((size_t)nsbatch + 1);
    for (int i = 0; i < nsbatch; ++i) {
        Nvk::NvkTevBatch b = sbatches[i];
        if (solid) { b.fragGlsl = g_pass_frag.c_str(); b.shaderKey = g_pass_key; b.blend_mode = 0;
                     b.z_test = 0; b.z_write = 0; }
        batches.push_back(b);
    }
    // 2D immediate batches: each is either a colour-only run (gradient, solid window fill →
    // passthrough shader) or a textured pane (window border, picture digit, glyph → decode
    // the bound GX texture once, MODULATE by vertex colour). Decoded textures are cached by
    // image ptr for this present (4 window-corner draws share one border texture). Storage
    // outlives renderTevFrame below (the batch's tex.rgba points into it).
    std::vector<std::vector<uint32_t>> tex_storage;
    std::unordered_map<const void*, int> tex_cache;   // image ptr → tex_storage index
    int n_tex_batches = 0;
    for (int bi = 0; bi < nibatch; ++bi) {
        const SbImmBatch& ib = ibatches[bi];
        Nvk::NvkTevBatch b{};
        b.vstart = (uint32_t)nscene + ib.vstart;
        b.vcount = ib.vcount;
        std::memset(b.push.kcolor, 0xFF, sizeof b.push.kcolor);
        b.z_test = 0; b.z_write = 0;                 // 2D draws on top, no depth
        b.blend_mode = 1; b.src_factor = 4; b.dst_factor = 5;  // SRCALPHA / INVSRCALPHA

        // Gate the bound texture before decoding (cf. sb_resolve_textures): a bad fmt,
        // absurd dims, or a paletted format with no loaded TLUT would send the tiled
        // decoder reading wild memory (a SEGV). An unrenderable texmap falls back to the
        // colour-only (passthrough) batch — not a silent nil, it's the GX white default.
        auto fmt_ok = [](int f){ return f==0||f==1||f==2||f==3||f==4||f==5||f==6||f==8||f==9||f==0xA||f==0xE; };
        auto paletted = [](int f){ return f==8||f==9||f==0xA; };
        bool decodable = ib.textured && ib.image && fmt_ok(ib.fmt) &&
                         ib.w > 0 && ib.h > 0 && ib.w <= 4096 && ib.h <= 4096 &&
                         (!paletted(ib.fmt) || ib.tlut);
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
            b.fragGlsl = g_tex_frag.c_str(); b.shaderKey = g_tex_key;
            b.tex[0].rgba = (const uint8_t*)tex_storage[idx].data();
            b.tex[0].w = ib.w; b.tex[0].h = ib.h; b.tex[0].linear = ib.linear;
            ++n_tex_batches;
        } else {
            b.fragGlsl = g_pass_frag.c_str(); b.shaderKey = g_pass_key;
        }
        batches.push_back(b);
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
    // SB_BATCH_DBG=1 → first scene frame; SB_BATCH_DBG=N>1 → fire once at present frame >= N.
    if (batchdbg && !s_batchdone && nscene > 0 && (batchdbg <= 1 || g_frame >= batchdbg)) {
        s_batchdone = true;
        for (int bi = 0; bi < nsbatch; ++bi) {
            const Nvk::NvkTevBatch& b = batches[bi];
            float zmn=1e30f,zmx=-1e30f,zsum=0; float ymn=1e30f,ymx=-1e30f,xmn=1e30f,xmx=-1e30f;
            double rr=0,gg=0,bb=0,r2=0,g2=0,b2=0; uint32_t cnt=0;
            for (uint32_t i = b.vstart; i < b.vstart + b.vcount && i < verts.size(); ++i) {
                const NvkTevVertex& v = verts[i];
                if (v.z<zmn)zmn=v.z; if (v.z>zmx)zmx=v.z; zsum+=v.z;
                if (v.y<ymn)ymn=v.y; if (v.y>ymx)ymx=v.y;
                if (v.x<xmn)xmn=v.x; if (v.x>xmx)xmx=v.x;
                rr+=v.rgba[0]; gg+=v.rgba[1]; bb+=v.rgba[2];
                r2+=v.rgba[0]*v.rgba[0]; g2+=v.rgba[1]*v.rgba[1]; b2+=v.rgba[2]*v.rgba[2]; ++cnt;
            }
            if (!cnt) continue;
            double mr=rr/cnt,mg=gg/cnt,mb=bb/cnt;
            double var=(r2/cnt-mr*mr)+(g2/cnt-mg*mg)+(b2/cnt-mb*mb);  // color variance (rainbow letters高い)
            std::fprintf(stderr, "[batchdbg] b%d vc=%u z[%.5f,%.5f]m%.5f ndcX[%.3f,%.3f] ndcY[%.3f,%.3f] "
                         "zt=%u zf=%u zw=%u bm=%u/%u/%u rgb=%.2f,%.2f,%.2f cvar=%.3f key=%llx\n",
                         bi, b.vcount, zmn, zmx, zsum/cnt, xmn, xmx, ymn, ymx,
                         b.z_test, b.z_func, b.z_write, b.blend_mode, b.src_factor, b.dst_factor,
                         mr, mg, mb, var, (unsigned long long)b.shaderKey);
        }
    }
    g_nvk.renderTevFrame(verts, batches, NvkClear{c[0], c[1], c[2], c[3]});

    char path[160];
    std::snprintf(path, sizeof path, "scratch/frames/boot_%04d.ppm", g_frame);
    write_ppm(path);
    std::printf("[present] frame %d clear=(%.2f,%.2f,%.2f,%.2f) scene_verts=%d scene_batches=%d "
                "imm_tris=%d imm_batches=%d (textured=%d) -> %s\n",
                g_frame, c[0], c[1], c[2], c[3], nscene, nsbatch, nimm / 3, nibatch,
                n_tex_batches, path);
    ++g_dumped;
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

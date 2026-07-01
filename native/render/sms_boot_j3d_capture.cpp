// sms_boot_j3d_capture.cpp — the native scene-geometry + MATERIAL capture (ONE owned path).
//
// In sms-boot the renderer is PC-native and fully owned: native/src/scene_drive.cpp drives the
// real GC draw flow (TSmJ3DScn::perform(8)) each frame, which runs entry()+viewCalc on only the
// ACTIVE scene models and draws the draw buffers. The draw bottoms out in J3DShape::draw(), whose
// GX issue is a no-op natively — so this TU taps J3DShape::draw and captures, per shape:
//   • geometry: decoded + transformed to Vulkan NDC (imm_project: per-shape draw matrix + the
//     live GX projection + viewport), as NvkTevVertex (pos + raster color0/1 + 8 texgen UVs);
//   • material: the shape's J3DMaterial → a TEV combiner fragment shader + decoded textures +
//     konst/reg push constants + depth/blend state (sms_boot_material.cpp).
// These group into per-material BATCHES (consecutive same-material shapes merge) that the present
// hook (sms_boot_present.cpp) draws through nvk's multi-material TEV frame (renderTevFrame).
//
// FIRST-SLICE LIMITS (documented, staged — not bandaids):
//   • raster colour = vertex CLR0/CLR1 (NO per-vertex lighting yet; lighting is the next stage);
//   • texgen = pass-through (uv[i] = the vertex's tex[i]); GX texgen matrices/modes are next;
//   • backface cull deferred (rendered double-sided; depth handles overdraw).
//
// ENDIANNESS (the #1 risk): the ngx vertex/DL decoders read BIG-ENDIAN. In the live host BMD
// buffer the SHP1 display-list stays BE (bmd_swap defers it) and swap_VTX1 leaves the vertex
// arrays BE too (the ngx contract); texel bytes stay BE (swap_TEX1 swaps only the ResTIMG header).
// If geometry/textures come back garbage, the BE contract was violated UPSTREAM — fix it there.

#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DVertex.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>   // j3dSys.getModel()/getMatPacket() — active draw state
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DColorBlocks.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DColorChan.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DLightObj.hpp>  // J3DLightObj (material-bound lights)
#include <JSystem/J3D/J3DGraphBase/J3DStruct.hpp>               // J3DLightInfo
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>

#include "gx_geom.h"               // NvkTevVertex, NvkTevBatch, NvkTevPush
#include "gx_imm_xform.h"      // SbImmRawVtx / SbImmVtx / imm_project / imm_project_eye_clip
#include "ngx_mesh.h"          // NgxCP, NgxVertex, ngx_build_mesh
#include "ngx_render_data.h"   // NgxTevState
#include "tev_shader.h"        // sb_tev_gen_fragment
#include "sms_boot_material.h" // sb_build_tev_state, sb_resolve_textures, SbTexImage
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp> // J3DTexture::getNum (table-size probe)
#include "sms_boot_lighting.h" // sb_light_vertex_color0 (pure, unit-tested)
#include "sms_boot_skin_bounds.h" // sb::skin_drawmtx_bound / resolve_skin_index (pure, unit-tested)
#include "ngx_light.h"         // ngx::LightSrc

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <execinfo.h>   // SB_B76_BT backtrace (name the mask's pass-routing owner)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>

using namespace sb::render;

extern "C" int  sb_present_frame(void);   // sms_boot_present.cpp — settled-frame gating
extern "C" int  sb_camera_view_settled(void);   // scene_drive.cpp — view matrix stationary
extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]);
extern "C" void sb_gx_get_live_projection(int* type, float proj[6], float vp[6]);
extern "C" int  sb_gx_get_proj44(float m[16]);
extern "C" int  sb_gx_get_lights(float out[8][16]);
extern "C" int  sb_boot_get_scene_camera(float view[12], float proj[16]) __attribute__((weak));

// sb_boot_dump_camera — write the EXACT view+projection the native renderer composited the 3D
// scene with this frame, so the Dolphin-GX oracle can be transplanted to the SAME viewpoint (the
// rung-6 "camera transplant": static plaza geometry then overlaps and the per-region diff measures
// RENDERER divergence, not camera/moment misalignment). Source is the SCENE camera (g_graphics in
// scene_drive.cpp) — NOT j3dSys.mViewMtx at present time, which holds whatever the LAST (often 2D/
// HUD) draw set. Both are engine-agnostic matrices, so injection into Dolphin needs no struct field
// offsets. Format: a `view` line of 12 floats (GC Mtx, row-major 3x4) then a `proj` line of 16 (4x4).
extern "C" void sb_boot_dump_camera(int frame) {
    float view[12] = {0}, proj[16] = {0};
    int have = (&sb_boot_get_scene_camera) ? sb_boot_get_scene_camera(view, proj) : 0;
    char path[160];
    std::snprintf(path, sizeof path, "scratch/frames/cam_%04d.txt", frame);
    FILE* f = std::fopen(path, "w");
    if (!f) { std::fprintf(stderr, "[cam-dump] cannot open %s\n", path); return; }
    std::fprintf(f, "# sms-boot camera transplant: view(3x4 row-major) + proj(4x4). frame=%d\n", frame);
    std::fprintf(f, "view");
    for (int i = 0; i < 12; ++i) std::fprintf(f, " %.9g", view[i]);
    std::fprintf(f, "\nproj");
    for (int i = 0; i < 16; ++i) std::fprintf(f, " %.9g", proj[i]);
    std::fprintf(f, "\nview_valid %d\n", have);
    std::fclose(f);
    std::fprintf(stderr, "[cam-dump] frame %d -> %s (view[0]=%.3f %.3f %.3f %.1f valid=%d)\n",
                 frame, path, view[0], view[1], view[2], view[3], have);
}
extern "C" void sb_gx_get_chan_amb(int slot, float rgb[3]);
extern "C" void sb_gx_get_chan_matcolor(int slot, float rgba[4]);
extern "C" void sb_gx_get_cur_posmtx(float m[3][4]);
extern "C" void sb_gx_get_color_alpha_update(int* color_update, int* alpha_update);
extern "C" void sb_gx_colupd_history(long* calls, long* last_false);  // b76 overbright drill
extern "C" void sb_gx_colupd_ring(int out[16]);                       // b76 overbright drill
extern "C" unsigned long sb_gx_light_load_count(void);
extern "C" void sb_host_alloc_push(void);
extern "C" void sb_host_alloc_pop(void);

namespace {

struct HostAllocScope { HostAllocScope() { sb_host_alloc_push(); } ~HostAllocScope() { sb_host_alloc_pop(); } };

bool dbg_enabled() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_J3D_DBG"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v != 0;
}

uint64_t fnv64(const char* s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

// Per-material render state, built ONCE per J3DMaterial* and reused across frames (materials
// are stable objects; texture/animation changes are a later stage). Owns the GLSL + decoded
// textures so the present's NvkTevBatch pointers stay valid frame-to-frame.
struct MatEntry {
    bool ok = false;
    std::string frag;
    uint64_t key = 0;
    NvkTevPush push{};
    uint8_t z_test = 1, z_func = 3, z_write = 1, blend_mode = 0, src_factor = 1, dst_factor = 0;
    uint8_t num_stages = 0;   // NgxTevState.num_stages (GX GENMODE numtevstages+1) — for the SB_GXDRAW diff
    std::vector<SbTexImage> tex;
    // Per colour-channel raster BASE colour (first slice = UNLIT): the material colour
    // register, used as the GX raster colour unless the channel sources from the vertex.
    // SMS world geometry is matSource=REG + lit; without lighting yet, matColor (usually
    // white/tint) modulates the texture so the scene isn't black (lighting is next stage).
    uint8_t matColor[2][4] = {{255,255,255,255},{255,255,255,255}};
    bool    matSrcVtx[2] = {false, false};   // channel sources from the vertex colour attr
    // Per-channel GX colour-channel control + ambient, for per-vertex lighting. chanCtrl is
    // J3DColorChan::mChanCtrl (== ngx::decode_chanctl layout); ambColor is the ambient register
    // (0..255). `lit` = any channel enables lighting (a J3DColorBlockLightOn material).
    uint16_t chanCtrl[2] = {0, 0};
    uint8_t  ambColor[2][4] = {{0,0,0,255},{0,0,0,255}};
    bool     hasAmb[2] = {false, false};   // material carries its own ambient block (else use the
                                           // global GX ambient register — the AmbGroup the light
                                           // loader set; faithful to ambSrc=register semantics)
    bool     lit = false;
    // GX texgen per texcoord-gen slot (i = texgen index, NOT the vertex attribute index). The
    // capture must apply these to the raw vertex source coord — pass-through (uv=raw tex) drops the
    // GX texture-coordinate transform (the file-select sea-foam stripes were the foam material's two
    // TEXMTX-scaled texgens, both sourcing TEX0, rendered with no matrix → wrong asymmetric tiling).
    struct TexGenG {
        uint8_t type = 1;        // GXTexGenType: 0=MTX3x4, 1=MTX2x4, 10=SRTG
        uint8_t src  = 4;        // GXTexGenSrc: 0=POS 1=NRM 4..11=TEX0..7 19/20=COLOR0/1
        uint8_t mtxsel = 60;     // GX_TEXMTX0=30 step3, GX_IDENTITY=60
        bool    has_mtx = false;
        float   m[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};   // mTotalMtx (3x4)
    };
    int     ntexgen = 0;
    TexGenG tg[8];
    // GX fog (captured into st.pe for the shader); mirrored here only for the SB_FOG_DBG eye-z probe.
    uint8_t fog_type = 0;
    float   fog_startz = 0.f, fog_endz = 0.f;
};
std::unordered_map<J3DMaterial*, MatEntry> g_matcache;

// GX per-vertex texgen (mirrors runtime/overrides/ngx_j3d_shape.cpp texgen_uv — the proven
// convention: TEXn input = (s,t,1,1); MTX2x4 → s'=row0·in, t'=row1·in; MTX3x4 → divide by row2·in;
// SRTG/COLOR src → the lit colour channel). litcol0 = this vertex's lit RGBA (0..1).
inline void sb_texgen_uv(const MatEntry::TexGenG& g, const NgxVertex& v,
                         const float litcol0[4], float out[2]) {
    if (g.type == 10 || g.src == 19 || g.src == 20) {
        out[0] = litcol0 ? litcol0[0] : 0.f;
        out[1] = litcol0 ? litcol0[1] : 0.f;
        return;
    }
    float in4[4];
    if (g.src >= 4 && g.src <= 11) { const int t = g.src - 4; in4[0]=v.tex[t][0]; in4[1]=v.tex[t][1]; in4[2]=1.f; in4[3]=1.f; }
    else if (g.src == 0)           { in4[0]=v.pos[0]; in4[1]=v.pos[1]; in4[2]=v.pos[2]; in4[3]=1.f; }
    else if (g.src == 1)           { in4[0]=v.nrm[0]; in4[1]=v.nrm[1]; in4[2]=v.nrm[2]; in4[3]=1.f; }
    else                           { in4[0]=v.tex[0][0]; in4[1]=v.tex[0][1]; in4[2]=1.f; in4[3]=1.f; }
    if (!g.has_mtx) { out[0]=in4[0]; out[1]=in4[1]; return; }
    const float* m = g.m;
    float s = m[0]*in4[0]+m[1]*in4[1]+m[2]*in4[2]+m[3]*in4[3];
    float t = m[4]*in4[0]+m[5]*in4[1]+m[6]*in4[2]+m[7]*in4[3];
    if (g.type == 0) {
        const float q = m[8]*in4[0]+m[9]*in4[1]+m[10]*in4[2]+m[11]*in4[3];
        if (q > 1e-6f || q < -1e-6f) { s /= q; t /= q; }
    }
    out[0]=s; out[1]=t;
}

// Frame-global output: the present-ready vertex list + per-material batches. Cleared on the
// first append after the present consumed them (take sets g_consumed).
std::vector<NvkTevVertex>   g_verts;
std::vector<NvkTevBatch> g_batches;
bool g_consumed = true;
// SB_OWN_GXLIST per-perform-list phase tag (see sb_boot_capture_set_phase + gx_geom.h NvkTevBatch::phase).
// Stamped onto every batch opened while set; lets the overbright harness attribute an over-composited
// layer to the source pass (the EFB pre-passes 1..3 are off-screen on GC and shouldn't composite).
int g_capture_phase = 0;
// b76 overbright drill: capture publishes the b76 mask material ptr (key eb5c8e74) so the entry-pass
// trace (J3DDrawBuffer::entryMatSort, SB_ENTRY_MAT=1) backtraces what enters the mask into MapXlu.
void* g_b76_material = nullptr;
// Active draw-buffer name (TDrawBufObj::getName()), stamped onto batches for overbright attribution.
const char* g_capture_drawbuf = nullptr;
// EFB→texture copy boundaries recorded during the scene capture (GXCopyTex → sb_boot_capture_efb_copy).
// Each marks a render-target boundary at batch_index: the EFB so far is snapshotted to `dest` (a host
// pointer into guest RAM, matchable against a later batch's resolved texmap src) and, if clear, wiped.
// The native present honors these to stop the pre-pass double-composite and to feed the post-pass
// EFB-sampler quads the real snapshot. See the 2026-06-30 overbright journal.
struct EfbCopyMark { size_t batch_index; const void* dest; bool clear; int wd, ht; };
std::vector<EfbCopyMark> g_efb_copies;
// PERSISTENT set of every EFB-copy destination address ever seen (across frames). An EFB-copy dest
// (スクリーンテクスチャ / the sea mirror) is a STABLE guest texture address — the producer GXCopyTex
// and the consumer composite quad are in the same frame, but the consumer can be CAPTURED before its
// producing copy is recorded into the per-frame g_efb_copies list (the 通常シーン copy lands at the
// end of the main pass, yet the GXPost composite3 quad that samples it is checked at its own capture
// point). Matching the per-frame list alone therefore MISSES composite3 (its src == the dest exactly,
// but the dest isn't in g_efb_copies yet → isEfbSrc=0, the long-standing "no efb_src consumer" bug).
// The persistent set recognizes the consumer regardless of intra-frame order.
std::unordered_set<const void*> g_efb_dest_seen;
// Capture-once-per-present lock (see sb_boot_capture_begin_scene / _end_scene below).
bool g_locked = false;          // when true, sb_boot_capture_j3d/sphere skip (interval already done)
bool g_want_capture = true;     // re-armed by the present consuming the buffer
// Immediate-mode (gx_imm) draws — the TMapObjWave sea grid — are NOT gated by the g_locked J3D
// capture lock: they append to the shared imm buffer on EVERY drive_scene, which under TURBO fires
// several times per present, so the wave (1352 verts) accumulated 2-3× -> a scene-vert OVERDRAW vs
// the oracle (native 3900 imm vs oracle 1352). This latch runs the imm-wave draw EXACTLY once per
// present interval (re-armed at present drain, like g_want_capture), so it matches the game's
// once-per-VI draw. See scene_drive.cpp drive_wave().
bool g_wave_want = true;
J3DMaterial* g_last_mat = nullptr;   // for consecutive-shape batch merging within a frame
// SB_CAP_COUNT diagnostic: per-present shape/triangle accounting (reset in begin_scene, dumped in
// end_scene) — measures whether the gameplay scene over-emits (the ~6M-vert OOM-ceiling question).
long g_present_shapes = 0;       // shapes captured this present (idx non-empty) — one increment
                                  // PER CALL, so a shape entered into multiple draw-buffer phases
                                  // (e.g. MapOpa/Mirror/Sky drawing identically in BOTH ph1 and ph4,
                                  // see fileselect-scene-underdraw-not-overdraw memory) is counted
                                  // once per phase, NOT once per distinct object. Compare
                                  // g_present_shapes_distinct_n against the oracle's own
                                  // distinct-J3DShape-instance count (SUNBRIGHT_GX_ATTRIB_SHAPES),
                                  // not this raw call count, for an apples-to-apples shape total.
long g_present_idx    = 0;       // summed idx (≈ vertices triangulated) this present
long g_present_skipped = 0;      // shapes skipped by the OOM/runaway guards this present
// Unique J3DShape* this present. A plain array + linear scan (not std::unordered_set<const void*>)
// — the guest-intrinsics shim this TU compiles under pollutes libstdc++'s hashtable template
// instantiation for pointer keys (a hard compile error, not a runtime concern); shape counts here
// are in the low hundreds at most, so linear dedup is cheap enough.
constexpr int kMaxDistinctShapes = 2048;
const void* g_present_shapes_distinct[kMaxDistinctShapes];
int g_present_shapes_distinct_n = 0;
void mark_shape_distinct(const void* shape) {
    for (int i = 0; i < g_present_shapes_distinct_n; ++i)
        if (g_present_shapes_distinct[i] == shape) return;
    if (g_present_shapes_distinct_n < kMaxDistinctShapes)
        g_present_shapes_distinct[g_present_shapes_distinct_n++] = shape;
}

const MatEntry* get_mat_entry(J3DMaterial* mat, J3DTexture* modelTex) {
    auto it = g_matcache.find(mat);
    if (it != g_matcache.end()) return &it->second;
    MatEntry e;
    NgxTevState st{};
    if (sb_build_tev_state(mat, st)) {
        e.frag = sb_tev_gen_fragment(st);
        e.key  = fnv64(e.frag.c_str());
        if ((unsigned)(e.key >> 32) == 0xeb5c8e74u) {   // b76 sea: dump raw per-stage TEV envs (one-shot)
            J3DTevBlock* tb2 = mat->getTevBlock();
            std::fprintf(stderr, "[b76-tev] num_stages=%u tbStageNum=%d\n",
                         st.num_stages, tb2 ? tb2->getTevStageNum() : -1);
            for (int s = 0; s < st.num_stages && s < 8; ++s) {
                const uint8_t* sb2 = tb2 ? reinterpret_cast<const uint8_t*>(tb2->getTevStage(s)) : nullptr;
                std::fprintf(stderr, "[b76-tev] s%d ce=%06x ae=%06x bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                             s, st.stage[s].color_env, st.stage[s].alpha_env,
                             sb2?sb2[0]:0, sb2?sb2[1]:0, sb2?sb2[2]:0, sb2?sb2[3]:0,
                             sb2?sb2[4]:0, sb2?sb2[5]:0, sb2?sb2[6]:0, sb2?sb2[7]:0);
            }
        }
        for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) {
            e.push.kcolor[c][k] = st.kcolor[c][k];
            e.push.tevreg[c][k] = st.tev_color[c][k];
        }
        e.fog_type = st.pe.fog_type; e.fog_startz = st.pe.fog_startz; e.fog_endz = st.pe.fog_endz;
        e.z_test = st.pe.z_test; e.z_func = st.pe.z_func; e.z_write = st.pe.z_write;
        e.blend_mode = st.pe.blend_mode; e.src_factor = st.pe.src_factor; e.dst_factor = st.pe.dst_factor;
        e.num_stages = st.num_stages;
        // Bisection: SB_TEV_NOBLEND forces every batch opaque (no blend) to test whether
        // blend-over-the-black-clear is what's blanking the scene.
        static const bool noblend = [](){ const char* v = std::getenv("SB_TEV_NOBLEND"); return v && v[0] && v[0] != '0'; }();
        if (noblend) e.blend_mode = 0;
        // Resolve textures against the table chosen by the caller (the PER-PACKET
        // J3DMatPacket::mTexture — see the call site). NOT modelData->getTexture(): that is the
        // static embedded TEX1, which a shared-material-table model (map.bmd uses setMaterialTable)
        // does NOT render with — its materials index a 59-entry shared table while the embedded one
        // holds 4, so every map/beach texmap went OOB → no sampler → flat-white sand/buildings.
        sb_resolve_textures(mat, modelTex ? (void*)modelTex : j3dSys.getTexture(), e.tex);
        if (J3DColorBlock* cb = mat->getColorBlock()) {
            int nchan = cb->getColorChanNum();
            for (int c = 0; c < 2; ++c) {
                if (J3DGXColor* mc = cb->getMatColor(c)) {
                    e.matColor[c][0] = mc->color.r; e.matColor[c][1] = mc->color.g;
                    e.matColor[c][2] = mc->color.b; e.matColor[c][3] = mc->color.a;
                }
                if (J3DGXColor* ac = cb->getAmbColor(c)) {   // null for LightOff blocks
                    e.ambColor[c][0] = ac->color.r; e.ambColor[c][1] = ac->color.g;
                    e.ambColor[c][2] = ac->color.b; e.ambColor[c][3] = ac->color.a;
                    e.hasAmb[c] = true;
                }
                if (c < nchan) {
                    if (J3DColorChan* ch = cb->getColorChan(c)) {
                        e.matSrcVtx[c] = (ch->getMatSrc() == GX_SRC_VTX);
                        e.chanCtrl[c]  = ch->mChanCtrl;
                        if (ch->getEnable()) e.lit = true;
                    }
                }
            }
        }
        e.ok = true;
        // SB_AMB_DBG: one-shot per distinct material — dump lit / hasAmb / ambColor / matColor so we
        // can tell (per the parity handoff) whether the file-select map materials carry their OWN
        // ambient block (getAmbColor non-null) or fall back to the global GX register.
        if (std::getenv("SB_AMB_DBG")) {
            static std::unordered_set<uint64_t> s_seen;
            if (s_seen.insert(e.key).second && s_seen.size() <= 64) {
                J3DColorBlock* cb = mat->getColorBlock();
                uint32_t ty = cb ? cb->getType() : 0;
                J3DGXColor* ac = cb ? cb->getAmbColor(0) : nullptr;
                std::fprintf(stderr, "[amb] key=%llx lit=%d hasAmb0=%d amb0=%u,%u,%u matc0=%u,%u,%u cc0=%04x ntex=%zu "
                    "cb=%p ty=%c%c%c%c ac=%p acv=%u,%u,%u\n",
                    (unsigned long long)e.key, (int)e.lit, (int)e.hasAmb[0],
                    e.ambColor[0][0], e.ambColor[0][1], e.ambColor[0][2],
                    e.matColor[0][0], e.matColor[0][1], e.matColor[0][2], e.chanCtrl[0], e.tex.size(),
                    (void*)cb, (char)(ty>>24),(char)(ty>>16),(char)(ty>>8),(char)ty,
                    (void*)ac, ac?ac->color.r:0, ac?ac->color.g:0, ac?ac->color.b:0);
            }
        }
        // Capture the GX texgen block: per texgen slot, the type/src/mtx-sel + the material's OWN
        // mTotalMtx (@J3DTexMtx+0x64 — the matrix J3D bakes into the per-frame DL replay; animated
        // materials re-patch it each frame, so it is exactly what the GPU uses). Applied per-vertex
        // below to produce uv[i] (replaces the old pass-through that dropped the transform).
        if (J3DTexGenBlock* tgb = mat->getTexGenBlock()) {
            int ntg = (int)tgb->getTexGenNum(); if (ntg > 8) ntg = 8; if (ntg < 0) ntg = 0;
            e.ntexgen = ntg;
            for (int i = 0; i < ntg; ++i) {
                MatEntry::TexGenG& g = e.tg[i];
                if (J3DTexCoord* tc = tgb->getTexCoord(i)) {
                    g.type = (uint8_t)tc->getTexGenType();
                    g.src  = (uint8_t)tc->getTexGenSrc();
                    g.mtxsel = tc->getTexGenMtx();
                }
                if (J3DTexMtx* tm = tgb->getTexMtx(i)) {
                    const Mtx& m = tm->mTotalMtx;
                    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) g.m[r*4+c] = m[r][c];
                    g.has_mtx = true;
                }
            }
        }
        // SB_MAT_DUMP_ALL: write every material's generated TEV fragment to scratch/frames/mat_<key>.txt
        // (the foam key eb5c8e74 needs its combiner inspected — the first-3 mat_glsl dump misses it).
        if (std::getenv("SB_MAT_DUMP_ALL")) {
            char pth[96]; std::snprintf(pth,sizeof pth,"scratch/frames/mat_%llx.txt",(unsigned long long)e.key);
            if (FILE* f = std::fopen(pth,"w")) { std::fputs(e.frag.c_str(), f); std::fclose(f); }
        }
        if (std::getenv("SB_TEXGEN_DBG")) {
            std::fprintf(stderr, "[texgen] key=%llx ntex=%zu ntg=%d\n",
                         (unsigned long long)e.key, e.tex.size(), e.ntexgen);
            for (int i = 0; i < e.ntexgen; ++i) {
                const MatEntry::TexGenG& g = e.tg[i];
                std::fprintf(stderr, "   tg%d type=%u src=%u mtx=%u has_mtx=%d m=[%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n",
                             i, g.type, g.src, g.mtxsel, (int)g.has_mtx,
                             g.m[0],g.m[1],g.m[2],g.m[3], g.m[4],g.m[5],g.m[6],g.m[7]);
            }
        }
        // One-shot probe on the FIRST lit material: where do its lights live (bound objects?),
        // and what does each colour channel control say. Decides the light-source port.
        if (dbg_enabled() && e.lit) {
            static int s_litprobe = 0;
            if (s_litprobe < 4) { ++s_litprobe;
                J3DColorBlock* cb = mat->getColorBlock();
                std::fprintf(stderr, "[litprobe] cb=%p type=%c%c%c%c nchan=%d cc0=%04x cc1=%04x amb0=%u,%u,%u\n",
                    (void*)cb,
                    cb?(char)(cb->getType()>>24):'?', cb?(char)(cb->getType()>>16):'?',
                    cb?(char)(cb->getType()>>8):'?', cb?(char)(cb->getType()):'?',
                    cb?cb->getColorChanNum():-1, e.chanCtrl[0], e.chanCtrl[1],
                    e.ambColor[0][0],e.ambColor[0][1],e.ambColor[0][2]);
                if (cb) for (int li = 0; li < 8; ++li) {
                    J3DLightObj* lo = cb->getLight(li);
                    if (!lo) continue;
                    const J3DLightInfo* in = static_cast<const J3DLightInfo*>(lo);
                    std::fprintf(stderr, "  [matlight] slot=%d pos=(%.1f,%.1f,%.1f) col=%u,%u,%u\n",
                                 li, in->mLightPosition.x, in->mLightPosition.y, in->mLightPosition.z,
                                 in->mColor.r, in->mColor.g, in->mColor.b);
                }
            }
        }
        if (dbg_enabled()) {
            static int s_matdbg = 0;
            if (s_matdbg < 16) {
                ++s_matdbg;
                uint32_t ce = st.stage[0].color_env;
                std::fprintf(stderr, "[mat] ns=%d ce=%06x a=%u b=%u c=%u d=%u cchan=%u tmap=%u "
                             "at=%u bm=%u msv0=%d msv1=%d cc0=%04x cc1=%04x matc0=%u,%u,%u,%u ntex=%zu key=%llx\n",
                             st.num_stages, ce, (ce>>12)&0xf,(ce>>8)&0xf,(ce>>4)&0xf,ce&0xf,
                             st.stage[0].color_chan, st.stage[0].texmap, st.pe.alpha_test,
                             (st.pe.blend_mode | (st.pe.src_factor<<4) | (st.pe.dst_factor<<8)), (int)e.matSrcVtx[0],
                             (int)e.matSrcVtx[1], e.chanCtrl[0], e.chanCtrl[1],
                             e.matColor[0][0],e.matColor[0][1],e.matColor[0][2],e.matColor[0][3],
                             e.tex.size(), (unsigned long long)e.key);
                if (s_matdbg <= 3) {
                    char pth[96]; std::snprintf(pth,sizeof pth,"scratch/frames/mat_glsl_%d.txt",s_matdbg);
                    if (FILE* f = std::fopen(pth,"w")) { std::fputs(e.frag.c_str(), f); std::fclose(f); }
                }
            }
        }
    }
    auto res = g_matcache.emplace(mat, std::move(e));
    return &res.first->second;
}

extern "C" int sb_boot_capture_texsrc_is_efb_dest(const void* src);   // defined below (live efb check)
// Fill an NvkTevBatch's tex[] slots + push/state from a MatEntry (called when opening a batch).
void fill_batch_material(NvkTevBatch& b, const MatEntry& e) {
    b.push = e.push; b.shaderKey = e.key; b.fragGlsl = e.frag.c_str();
    b.z_test = e.z_test; b.z_func = e.z_func; b.z_write = e.z_write;
    b.blend_mode = e.blend_mode; b.src_factor = e.src_factor; b.dst_factor = e.dst_factor;
    b.num_stages = e.num_stages;
    // GXSetColorUpdate / GXSetAlphaUpdate are LIVE global GX state at draw time, NOT in the
    // J3DMaterial PE block — read them now (the batch is opened during the shape's draw). The
    // file-select composite3 (b76, SRCALPHA/SRCCLR) runs under GXSetColorUpdate(GX_FALSE): the GC
    // writes no colour, so it must NOT paint; capturing it as colour-writing made it a full-screen
    // white wash (the dominant overbright). See the per-draw blend value oracle finding.
    int cu = 1, au = 1; sb_gx_get_color_alpha_update(&cu, &au);
    b.color_update = (uint8_t)cu; b.alpha_update = (uint8_t)au;
    for (const SbTexImage& t : e.tex) {
        if (t.slot < 0 || t.slot >= 8) continue;
        // RE-EVALUATE the EFB-sampler status LIVE (the material+texture decode is cached per
        // J3DMaterial, but whether a texmap samples an EFB copy is dynamic — composite3's screen-
        // texture material is first cached before its 通常シーン EFB copy is known, so the cached
        // efb_src is a stale null). If the texmap's src is now a recorded EFB-copy dest, bind the
        // snapshot (the present's draw_tev_segment prefers efb_src over the stale decoded rgba).
        const void* efb = t.efb_src;
        if (!efb && t.src_ptr && sb_boot_capture_texsrc_is_efb_dest(t.src_ptr)) efb = t.src_ptr;
        if (t.rgba.empty() && !efb) continue;   // unbound (a snapshot texmap has empty rgba)
        b.tex[t.slot].rgba = t.rgba.empty() ? nullptr : (const uint8_t*)t.rgba.data();
        b.tex[t.slot].w = t.w; b.tex[t.slot].h = t.h; b.tex[t.slot].linear = t.linear ? 1 : 0;
        b.tex[t.slot].min_filter = t.min_filter; b.tex[t.slot].max_aniso = t.max_aniso;
        b.tex[t.slot].wrap_s = t.wrap_s; b.tex[t.slot].wrap_t = t.wrap_t;
        b.tex[t.slot].efb_src = efb;
    }
}

bool build_native_cp(J3DShape* shape, J3DVertexData& vd, const uint8_t* base, NgxCP& cp) {
    GXVtxDescList* desc = shape->getVtxDesc();
    GXVtxAttrFmtList* fmt = vd.getVtxAttrFmtList();
    if (!desc || !fmt) return false;

    for (GXVtxDescList* e = desc; e->attr != 0xFF; ++e) {
        u32 attr = e->attr, type = e->type;
        if (attr == 0)                     cp.vcd_lo |= (type ? 1u : 0u) << 0;
        else if (attr <= 8)                cp.vcd_lo |= (type ? 1u : 0u) << attr;
        else if (attr == 9)                cp.vcd_lo |= (type & 3) << 9;    // POS
        else if (attr == 10)               cp.vcd_lo |= (type & 3) << 11;   // NRM
        else if (attr == 11)               cp.vcd_lo |= (type & 3) << 13;   // CLR0
        else if (attr == 12)               cp.vcd_lo |= (type & 3) << 15;   // CLR1
        else if (attr >= 13 && attr <= 20) cp.vcd_hi |= (type & 3) << (2 * (attr - 13));
    }

    u32 pos_type = 4, nrm_type = 4, tex_type[8] = {4,4,4,4,4,4,4,4};
    u32& A = cp.vat[0][0]; u32& B = cp.vat[0][1]; u32& C = cp.vat[0][2];
    for (GXVtxAttrFmtList* e = fmt; e->attr != 0xFF; ++e) {
        u32 attr = e->attr, cnt = e->cnt, type = e->type, frac = e->frac & 0x1f;
        switch (attr) {
        case 9:  A |= (cnt & 1) | ((type & 7) << 1) | (frac << 4); pos_type = type; break;
        case 10: A |= ((cnt ? 1u : 0u) << 9) | ((type & 7) << 10); if (cnt == 2) A |= 1u << 31;
                 nrm_type = type; break;
        case 11: A |= ((cnt & 1) << 13) | ((type & 7) << 14); break;
        case 12: A |= ((cnt & 1) << 17) | ((type & 7) << 18); break;
        case 13: A |= ((cnt & 1) << 21) | ((type & 7) << 22) | (frac << 25); tex_type[0]=type; break;
        case 14: B |= (cnt & 1) | ((type & 7) << 1) | (frac << 4);           tex_type[1]=type; break;
        case 15: B |= ((cnt & 1) << 9)  | ((type & 7) << 10) | (frac << 13); tex_type[2]=type; break;
        case 16: B |= ((cnt & 1) << 18) | ((type & 7) << 19) | (frac << 22); tex_type[3]=type; break;
        case 17: B |= ((cnt & 1) << 27) | ((type & 7) << 28); C |= frac;     tex_type[4]=type; break;
        case 18: C |= ((cnt & 1) << 5)  | ((type & 7) << 6)  | (frac << 9);  tex_type[5]=type; break;
        case 19: C |= ((cnt & 1) << 14) | ((type & 7) << 15) | (frac << 18); tex_type[6]=type; break;
        case 20: C |= ((cnt & 1) << 23) | ((type & 7) << 24) | (frac << 27); tex_type[7]=type; break;
        default: break;
        }
    }

    // base IS the POS array, so POS lives at offset 0 — a VALID offset. Mark genuinely
    // ABSENT arrays with a sentinel (0xFFFFFFFF) so the resolver can tell "offset 0" (the
    // POS array) from "no array". (The old `0` for null collapsed POS → null → every
    // vertex decoded to (0,0,0) → the whole scene projected to one off-screen point.)
    auto off = [&](const void* p) -> u32 { return p ? (u32)((const uint8_t*)p - base) : 0xFFFFFFFFu; };
    cp.array_base[0] = off(vd.getVtxPosArray());     cp.array_stride[0] = (pos_type == 4) ? 12 : 6;
    cp.array_base[1] = off(vd.getVtxNormArray());    cp.array_stride[1] = (nrm_type == 4) ? 12 : 6;
    cp.array_base[2] = off(vd.getVtxColorArray(0));  cp.array_stride[2] = 4;
    cp.array_base[3] = off(vd.getVtxColorArray(1));  cp.array_stride[3] = 4;
    for (int i = 0; i < 8; ++i) {
        cp.array_base[4 + i]   = off(vd.getVtxTexCoordArray(i));
        cp.array_stride[4 + i] = (tex_type[i] == 4) ? 8 : 4;
    }
    return true;
}

struct ResolveCtx { const uint8_t* base; size_t size; };
const unsigned char* resolve_native(unsigned off, void* user) {
    auto* c = (ResolveCtx*)user;
    if (off == 0xFFFFFFFFu || (c->size && off >= c->size)) return nullptr;   // 0xFFFFFFFF = absent
    return c->base + off;
}

} // namespace

// ============================ THE CAPTURE (single owned path) =============================
extern "C" bool sb_boot_capture_j3d(J3DShape* shape) {
    if (g_locked) return true;   // capture-once-per-present: this interval already captured
    if (!shape) return false;
    HostAllocScope _hostalloc;

    J3DModel* model = j3dSys.getModel();
    if (!model || !model->getModelData()) return true;

    J3DVertexData* vd = shape->mVertexData;
    if (!vd) return true;
    const uint8_t* base = (const uint8_t*)vd->getVtxPosArray();
    if (!base) return true;

    // The material being drawn (set by J3DMatPacket::draw before its shapes).
    J3DMatPacket* mp = j3dSys.getMatPacket();
    J3DMaterial* mat = mp ? mp->getMaterial() : nullptr;
    if (!mat) return true;
    // Texture table: prefer the PER-PACKET table (J3DMatPacket::mTexture, set per material
    // packet at DL build — line J3DModelData.cpp:558). The global j3dSys model/table can be
    // stale (a different model bound it last), and modelData->getTexture() is the static
    // embedded TEX1 which a shared-material-table model (setMaterialTable) does NOT use.
    J3DTexture* packetTex   = mp ? mp->mTexture : nullptr;
    J3DTexture* modelDataTex = model->getModelData()->getTexture();
    J3DTexture* texTbl = packetTex ? packetTex : modelDataTex;
    if (dbg_enabled()) {   // one-shot per material: which candidate table is large enough?
        static std::unordered_map<J3DMaterial*, char> seen;
        if (seen.find(mat) == seen.end() && seen.size() < 64) { seen[mat] = 1;
            J3DTexture* sysTex = j3dSys.getTexture();
            std::fprintf(stderr, "[textbl] mat=%p packet=%u modelData=%u sys=%u\n", (void*)mat,
                packetTex?packetTex->getNum():0, modelDataTex?modelDataTex->getNum():0,
                sysTex?sysTex->getNum():0); }
    }
    const MatEntry* me = get_mat_entry(mat, texTbl);
    if (!me || !me->ok) return true;
    if (dbg_enabled() && (unsigned)(me->key >> 32) == 0x7bc0841du) {  // raw colorblock for the ray mat
        static int rn = 0;
        if (rn < 2) { ++rn;
            J3DColorBlock* cb = mat->getColorBlock();
            J3DColorBlock* mdcb = nullptr;
            // also read the modelData base material's colorblock for the same material index
            std::fprintf(stderr, "[rawcc] mat=%p cb=%p nchan=%d cc[0..3]=%04x,%04x,%04x,%04x matc0=%u,%u,%u,%u\n",
                (void*)mat, (void*)cb, cb?cb->getColorChanNum():-1,
                cb&&cb->getColorChan(0)?cb->getColorChan(0)->mChanCtrl:0xDEAD,
                cb&&cb->getColorChan(1)?cb->getColorChan(1)->mChanCtrl:0xDEAD,
                cb&&cb->getColorChan(2)?cb->getColorChan(2)->mChanCtrl:0xDEAD,
                cb&&cb->getColorChan(3)?cb->getColorChan(3)->mChanCtrl:0xDEAD,
                cb&&cb->getMatColor(0)?cb->getMatColor(0)->color.r:0, cb&&cb->getMatColor(0)?cb->getMatColor(0)->color.g:0,
                cb&&cb->getMatColor(0)?cb->getMatColor(0)->color.b:0, cb&&cb->getMatColor(0)?cb->getMatColor(0)->color.a:0);
            (void)mdcb;
        }
    }

    NgxCP cp{};
    if (!build_native_cp(shape, *vd, base, cp)) return true;

    ResolveCtx rc{ base, 0 };
    std::vector<NgxVertex> verts;
    std::vector<unsigned> idx;
    const u16 mtxGroupNum = shape->getMtxGroupNum();
    if (const char* ev = std::getenv("SB_CAP_DBG"); ev && ev[0] && ev[0] != '0') {
        static int cdn = 0;
        if (cdn < 200) { ++cdn;
            std::fprintf(stderr, "[cap] shape=%p model=%p mtxGroups=%u\n",
                         (void*)shape, (void*)model, mtxGroupNum);
            std::fflush(stderr);
        }
    }
    for (u16 e = 0; e < mtxGroupNum; ++e) {
        J3DShapeDraw* dp = shape->getShapeDraw(e);
        if (!dp || !dp->getDisplayList()) continue;
        const size_t v0 = verts.size();
        if (const char* ev = std::getenv("SB_CAP_DBG"); ev && ev[0] && ev[0] != '0') {
            static int cdn2 = 0;
            if (cdn2 < 400) { ++cdn2;
                std::fprintf(stderr, "[cap]   group %u dl=%p dlSize=%u\n",
                             e, (void*)dp->getDisplayList(), (unsigned)dp->getDisplayListSize());
                std::fflush(stderr);
            }
        }
        // FAIL-FAST bound BEFORE the parse. A real J3D shape display list is at most tens of KB; a
        // multi-hundred-KB+ size here means the capture mis-read this shape (build_native_cp / the
        // J3DShapeDraw fields don't match the real model layout) — seen on NPC parts/body models
        // (same root as their 32768x32768 garbage texture dims). ngx_build_mesh trusts dl_size as the
        // primitive-bound ceiling, so a garbage-huge size lets it append millions of 112-B NgxVertex
        // entries → the vector reallocs into a multi-GB livelock that never returns (the frame hangs).
        // Skip + log so the frame completes and the mis-parse stays visible; PROPER FIX = read the
        // NPC models' vertex/CP/texture data correctly in the capture.
        const u32 dlSize = dp->getDisplayListSize();
        if (dlSize > 256u * 1024) {
            static int s_rr = 0;
            if (s_rr < 20) { ++s_rr;
                std::fprintf(stderr, "[capture] SKIP runaway shape: model=%p mat=%p group=%u/%u "
                             "dlSize=%u — NPC vertex-format mis-parse (fail-fast)\n",
                             (void*)model, (void*)mat, e, mtxGroupNum, dlSize); }
            continue;
        }
        ngx_build_mesh(cp, dp->getDisplayList(), dlSize, resolve_native, &rc, verts, idx);
        // Tag each vertex with the matrix-PACKET (mtx group) that drew it, so the per-vertex
        // skinning matrix resolves against THIS packet's J3DShapeMtx table (getUseMtxIndex),
        // not a global last-writer — a skinned shape draws in N packets each reloading the slots.
        for (size_t vi = v0; vi < verts.size(); ++vi) verts[vi].packet = (unsigned char)e;
    }
    if (idx.empty()) return true;

    // FAIL-FAST bound: a single legit J3D shape's display list emits at most a few thousand
    // triangles. A runaway count here means the native capture MIS-PARSED this shape's vertex
    // format (ngx_build_mesh read a garbage primitive vertex count) — observed on the NPC parts/
    // body models (same root as their 32768x32768 garbage texture dims): the model data the NPC
    // managers load via SDLModel/the keeper isn't being read correctly by the capture's CP/vtx-desc
    // setup. Feeding a runaway idx into g_verts reallocates a multi-GB vector and livelocks the
    // frame (the once-per-present gate alone can't help: it's ONE shape). Skip + log loudly so the
    // frame completes and the mis-parse is visible; the PROPER FIX is to read NPC J3D vertex/texture
    // data correctly in the capture (build_native_cp / ngx_build_mesh for these models).
    if (idx.size() > 200000) {
        static int s_runaway = 0;
        if (s_runaway < 20) { ++s_runaway;
            std::fprintf(stderr, "[capture] SKIP mis-parsed shape: model=%p mat=%p idx=%zu verts=%zu "
                         "mtxGroups=%u — NPC vertex-format mis-parse (see fail-fast note)\n",
                         (void*)model, (void*)mat, idx.size(), verts.size(), mtxGroupNum); }
        ++g_present_skipped;
        return true;
    }
    g_present_shapes += 1;
    mark_shape_distinct(shape);
    g_present_idx    += (long)idx.size();

    int projType; float proj[6]; float vp[6];
    sb_gx_get_projection(&projType, proj, vp);
    float ident[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    // The shape's draw-matrix table (model->VIEW/eye space, computed by the model's viewCalc) for
    // the current view. Each entry is a Mtx (f32[3][4]). Per-vertex skinning selects WHICH entry.
    Mtx* drawTbl = shape->mDrawMatrices ? shape->mDrawMatrices[*shape->mCurrentViewNo] : nullptr;
    // BOUND for the per-vertex skin-matrix index (mtx_for() below), MUST come from the shape's OWN
    // draw-matrix table, not from j3dSys.getModel(). j3dSys.getModel() is whichever model's
    // calc/entry/viewCalc ran MOST RECENTLY (every J3DModel entry point calls j3dSys.setModel(this))
    // — NOT necessarily the model that owns THIS shape, since draw() happens later at buffer-flush
    // time, possibly after several OTHER models' calc/entry re-set j3dSys's "current model" pointer.
    // PROVEN (SB_SKIN_BOUNDS_DBG, file-select picker): TMario's body shapes (own table 106 entries)
    // captured while j3dSys.getModel() pointed at the CAP model (table size 1) — TMarioCap's cap
    // sub-models run their OWN calc/entry/viewCalc INSIDE TMario::perform, AFTER the body's
    // entryModels in the SAME call, so by the time ChrOpa/ChrXlu actually draw() (much later, at the
    // GXPost pass), j3dSys.getModel() is the cap, not the body. Using the model's (wrong) drawMtxNum=1
    // as the bounds ceiling truncated every body vertex needing skin index ≥1 to drawIdx=0 — every
    // limb collapsed onto whichever joint matrix lived at slot 0 (the visually mangled/scattered
    // Mario at the settled file-select picker). FIX: read the ceiling from shape->mDrawMtxData
    // (J3DDrawMtxData*), which is bound ONCE per-shape at model-init (setDrawMtxDataPointer) and is
    // therefore always correct for THIS shape regardless of j3dSys's current-model state.
    const int drawMtxNum = sb::skin_drawmtx_bound(
        shape->mDrawMtxData != nullptr,
        shape->mDrawMtxData ? (int)shape->mDrawMtxData->mEntryNum : 0,
        model->getModelData() ? (int)model->getModelData()->getDrawMtxNum() : 0);
    float posMtx[3][4];   // matrix 0 — for the debug probes below only (real geometry skins per-vertex)
    if (drawTbl) std::memcpy(posMtx, &drawTbl[0][0], sizeof(posMtx));
    else         std::memcpy(posMtx, ident, sizeof(posMtx));

    // SB_B76_XF: at the b76 overbright wash shape (key low32 0xc39d96b8), dump the LIVE vs LATCHED
    // projection + viewport + posMtx + resulting NDC, so we see exactly which matrix that draw
    // wants. This is the ph6 transform probe — b76 is a later phase-6 shape the per-phase-first
    // SB_PH_XF missed.
    if (const char* e = std::getenv("SB_B76_XF"); e && e[0] && e[0] != '0'
        && (unsigned)(me->key) == 0xc39d96b8u && !idx.empty() && sb_camera_view_settled()) {
        static int n = 0; if (n < 4) { ++n;
            int lt; float lp[6], lv[6]; sb_gx_get_live_projection(&lt, lp, lv);
            const NgxVertex& s0 = verts[idx[0]];
            SbImmVtx qLatch = imm_project(SbImmRawVtx{ s0.pos[0],s0.pos[1],s0.pos[2],0,0,0,0 },
                                          projType, proj, posMtx, vp);
            SbImmVtx qLive  = imm_project(SbImmRawVtx{ s0.pos[0],s0.pos[1],s0.pos[2],0,0,0,0 },
                                          lt, lp, posMtx, lv);
            std::fprintf(stderr, "[b76xf] phase=%d pos0=(%.1f,%.1f,%.1f)\n"
                         "        LATCH type=%d proj[%.3f %.3f %.3f %.3f %.3f %.3f] vp[%.0f %.0f %.0f %.0f] -> ndc(%.3f,%.3f,%.3f)\n"
                         "        LIVE  type=%d proj[%.3f %.3f %.3f %.3f %.3f %.3f] vp[%.0f %.0f %.0f %.0f] -> ndc(%.3f,%.3f,%.3f)\n"
                         "        posMtx r0[%.2f %.2f %.2f %.2f] r1[%.2f %.2f %.2f %.2f] r2[%.2f %.2f %.2f %.2f]\n",
                         g_capture_phase, s0.pos[0],s0.pos[1],s0.pos[2],
                         projType, proj[0],proj[1],proj[2],proj[3],proj[4],proj[5], vp[0],vp[1],vp[2],vp[3], qLatch.x,qLatch.y,qLatch.z,
                         lt, lp[0],lp[1],lp[2],lp[3],lp[4],lp[5], lv[0],lv[1],lv[2],lv[3], qLive.x,qLive.y,qLive.z,
                         posMtx[0][0],posMtx[0][1],posMtx[0][2],posMtx[0][3],
                         posMtx[1][0],posMtx[1][1],posMtx[1][2],posMtx[1][3],
                         posMtx[2][0],posMtx[2][1],posMtx[2][2],posMtx[2][3]);
        }
    }

    // SB_MARIO_XF: dump every captured shape's first vertex world pos + projected ndc + which
    // texture table was chosen. Gated to a SETTLED frame window (the early frames are the intro
    // camera pan, where everything projects off-screen). Locates Mario's 11 body shapes and shows
    // whether their textures resolve against Mario's own table or a stale (map building-atlas) one.
    // Gate on the camera having SETTLED (robust; the choice-state settle is ~frame 1690+, far past
    // the old fixed 270-276 window) and cap the per-shape spam so one settled frame's shapes dump
    // once. This locates Mario's body shape(s) AND any duplicate/ghost entry (the double-enter).
    static int s_marioxf_n = 0;
    if (std::getenv("SB_MARIO_XF") && !idx.empty() && sb_camera_view_settled() && s_marioxf_n < 60) {
        ++s_marioxf_n;
        const NgxVertex& s0 = verts[idx[0]];
        SbImmVtx q = imm_project(SbImmRawVtx{ s0.pos[0],s0.pos[1],s0.pos[2],0,0,0,0 },
                                 projType, proj, posMtx, vp);
        std::fprintf(stderr, "[marioxf] f%d model=%p mat=%p nidx=%zu ntex=%zu key=%llx pkt=%u mdl=%u pos0=(%.1f,%.1f,%.1f) ndc=(%.3f,%.3f,%.3f) pmt=(%.1f,%.1f,%.1f)\n",
                     sb_present_frame(), (void*)model, (void*)mat, idx.size(), me->tex.size(),
                     (unsigned long long)me->key, packetTex?packetTex->getNum():0,
                     modelDataTex?modelDataTex->getNum():0,
                     s0.pos[0],s0.pos[1],s0.pos[2], q.x,q.y,q.z,
                     posMtx[0][3],posMtx[1][3],posMtx[2][3]);
    }
    if (dbg_enabled()) {
        static int s_tx = 0;
        if (s_tx < 6 && !idx.empty()) {
            ++s_tx;
            const NgxVertex& s0 = verts[idx[0]];
            SbImmVtx q = imm_project(SbImmRawVtx{ s0.pos[0],s0.pos[1],s0.pos[2],0,0,0,0 },
                                     projType, proj, posMtx, vp);
            std::fprintf(stderr, "[xform] pt=%d proj[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f] vp[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f]\n"
                         "        pos0=(%.1f,%.1f,%.1f) -> ndc(%.3f,%.3f,%.3f)\n"
                         "        posMtx r0[%.3f,%.3f,%.3f,%.3f] r1[%.3f,%.3f,%.3f,%.3f] r2[%.3f,%.3f,%.3f,%.3f]\n",
                         projType, proj[0],proj[1],proj[2],proj[3],proj[4],proj[5],
                         vp[0],vp[1],vp[2],vp[3],vp[4],vp[5],
                         s0.pos[0],s0.pos[1],s0.pos[2], q.x,q.y,q.z,
                         posMtx[0][0],posMtx[0][1],posMtx[0][2],posMtx[0][3],
                         posMtx[1][0],posMtx[1][1],posMtx[1][2],posMtx[1][3],
                         posMtx[2][0],posMtx[2][1],posMtx[2][2],posMtx[2][3]);
        }
    }

    // The live hardware lights (view-space), populated by J3DColorBlockLightOn::load ->
    // J3DLightObj::load -> GXLoadLightObjImm during the scene draw. Lighting only engages when
    // the material enables it AND at least one light exists; otherwise the raster stays the
    // full-bright material colour (the light pipeline is the next thing to own if nlights==0).
    ngx::LightSrc lsrc[8];
    float lraw[8][16];
    const int nlights = sb_gx_get_lights(lraw);
    for (int i = 0; i < 8; ++i) {
        ngx::LightSrc& L = lsrc[i];
        L.valid = lraw[i][0] != 0.f;
        L.color[0]=lraw[i][1]; L.color[1]=lraw[i][2]; L.color[2]=lraw[i][3];
        L.pos[0]=lraw[i][4];   L.pos[1]=lraw[i][5];   L.pos[2]=lraw[i][6];
        L.dir[0]=lraw[i][7];   L.dir[1]=lraw[i][8];   L.dir[2]=lraw[i][9];
        L.cosA[0]=lraw[i][10]; L.cosA[1]=lraw[i][11]; L.cosA[2]=lraw[i][12];
        L.distA[0]=lraw[i][13];L.distA[1]=lraw[i][14];L.distA[2]=lraw[i][15];
    }
    const bool do_light = me->lit && nlights > 0;
    if ((unsigned)(me->key >> 32) == 0xeb5c8e74u) {   // b76 sea-reflection raster probe (one-shot)
        static int s_b76 = 0;
        if (s_b76 < 3 && sb_camera_view_settled()) { ++s_b76;
            std::fprintf(stderr, "[b76-ras] lit=%d do_light=%d nlights=%d matSrcVtx0=%d matColor0=%u,%u,%u,%u "
                         "ambColor0=%u,%u,%u chanCtrl0=%04x\n",
                         (int)me->lit, (int)do_light, nlights, (int)me->matSrcVtx[0],
                         me->matColor[0][0], me->matColor[0][1], me->matColor[0][2], me->matColor[0][3],
                         me->ambColor[0][0], me->ambColor[0][1], me->ambColor[0][2], me->chanCtrl[0]);
            if (!verts.empty())
                std::fprintf(stderr, "[b76-ras] vtxClr0[0]=%u,%u,%u,%u  L0(valid=%d c=%.2f,%.2f,%.2f)\n",
                             verts[0].clr[0][0], verts[0].clr[0][1], verts[0].clr[0][2], verts[0].clr[0][3],
                             (int)lsrc[0].valid, lsrc[0].color[0], lsrc[0].color[1], lsrc[0].color[2]); }
    }
    if (dbg_enabled()) {
        static int s_ld = 0;
        if (s_ld < 8) { ++s_ld;
            std::fprintf(stderr, "[light] nlights=%d loads=%lu do_light=%d cc0=%04x amb0=%u,%u,%u L0(valid=%d c=%.2f,%.2f,%.2f p=%.0f,%.0f,%.0f)\n",
                         nlights, sb_gx_light_load_count(), (int)do_light, me->chanCtrl[0],
                         me->ambColor[0][0], me->ambColor[0][1], me->ambColor[0][2],
                         (int)lsrc[0].valid, lsrc[0].color[0],lsrc[0].color[1],lsrc[0].color[2],
                         lsrc[0].pos[0],lsrc[0].pos[1],lsrc[0].pos[2]); }
    }

    if (g_consumed) { g_verts.clear(); g_batches.clear(); g_last_mat = nullptr; g_consumed = false; }
    if (g_verts.size() > 6u * 1024 * 1024) return true;   // OOM guard for an undrained config

    const uint32_t vstart = (uint32_t)g_verts.size();
    // Reserve room for this shape's vertices, but PRESERVE geometric growth: a bare
    // reserve(size()+idx.size()) pins capacity to exactly the new size every shape, so each
    // subsequent shape reallocates and memmoves the WHOLE accumulated buffer -> O(n^2). On a
    // big scene (the full NPC population, millions of verts toward the 6M guard) that is tens
    // of GB of copying = the frame never completes (the SB_NPC_ON livelock: gdb showed 100% of
    // time in this vector's element copy with capacity==size). Grow at least 2x so push_back's
    // amortized O(1) is restored while still guaranteeing this batch fits without re-realloc.
    const size_t need = g_verts.size() + idx.size();
    if (g_verts.capacity() < need)
        g_verts.reserve(std::max(need, g_verts.capacity() * 2));

    // PER-VERTEX SKINNING: select the vertex's draw matrix the way GX/J3D does. A vertex's
    // PNMTXIDX (s.matidx) is the GX position-matrix-memory ROW; the slot = matidx/3. The packet's
    // J3DShapeMtx maps that slot to a draw-matrix-table index (J3DShapeMtxMulti::getUseMtxIndex →
    // unkC[slot]; single J3DShapeMtx → unk4). So posMtx = drawTbl[ getUseMtxIndex(matidx/3) ].
    // Using drawTbl[0] flat (the old code) collapsed every vertex of a multi-matrix SKINNED shape
    // (Mario) onto joint 0 → the mesh deformed (the "skinning slab"). Returns the 3x4 (Mtx).
    static const bool no_skin = [](){ const char* v = std::getenv("SB_NO_SKIN"); return v && v[0] && v[0] != '0'; }();
    auto mtx_for = [&](const NgxVertex& s) -> const float (*)[4] {
        if (!drawTbl) return ident;
        int drawIdx = 0;
        if (!no_skin && s.packet < shape->getMtxGroupNum()) {
            if (J3DShapeMtx* sm = shape->getShapeMtx(s.packet)) {
                const u16 di = sm->getUseMtxIndex((u16)(s.matidx / 3));
                drawIdx = sb::resolve_skin_index(di, drawMtxNum);
            }
        }
        return drawTbl[drawIdx];
    };
    // model -> eye via the per-vertex draw matrix. Shared by the vertex builder and the debug probes.
    auto eye_of = [&](const NgxVertex& s, float& ex, float& ey, float& ez) {
        const float (*m)[4] = mtx_for(s);
        ex = m[0][3] + m[0][0]*s.pos[0] + m[0][1]*s.pos[1] + m[0][2]*s.pos[2];
        ey = m[1][3] + m[1][0]*s.pos[0] + m[1][1]*s.pos[1] + m[1][2]*s.pos[2];
        ez = m[2][3] + m[2][0]*s.pos[0] + m[2][1]*s.pos[1] + m[2][2]*s.pos[2];
    };
    // Build ONE shaded vertex carrying CLIP-space xyzw (the GPU divides → perspective-correct UV/
    // colour interpolation + hardware near/side clipping). No CPU perspective divide, no hand-rolled
    // clipper — that re-derivation was the floor texture-warp (w=1 affine) and near-plane slab.
    auto make_v = [&](const NgxVertex& s) -> NvkTevVertex {
        NvkTevVertex tv{};
        float ex, ey, ez; eye_of(s, ex, ey, ez);
        SbImmClip c = imm_project_eye_clip(ex, ey, ez, projType, proj, vp);
        tv.x = c.x; tv.y = c.y; tv.z = c.z; tv.w = c.w;
        // Raster base per channel: vertex colour if the channel sources from the vertex, else the
        // material colour register. COLOR0 RGB is per-vertex LIT when the material enables lighting.
        const unsigned char* c0 = me->matSrcVtx[0] ? s.clr[0] : me->matColor[0];
        const unsigned char* c1 = me->matSrcVtx[1] ? s.clr[1] : me->matColor[1];
        tv.rgba[0]  = c0[0]/255.f; tv.rgba[1]  = c0[1]/255.f; tv.rgba[2]  = c0[2]/255.f; tv.rgba[3]  = c0[3]/255.f;
        tv.rgba1[0] = c1[0]/255.f; tv.rgba1[1] = c1[1]/255.f; tv.rgba1[2] = c1[2]/255.f; tv.rgba1[3] = c1[3]/255.f;
        if (do_light) {
            const float matc0[3] = { me->matColor[0][0]/255.f, me->matColor[0][1]/255.f, me->matColor[0][2]/255.f };
            // Ambient: the material's own block if it has one, else the global GX ambient register
            // (= "Ambient Group", set by the stage light loader; GX uses it when ambSrc=register).
            float ambc0[3];
            if (me->hasAmb[0]) { ambc0[0]=me->ambColor[0][0]/255.f; ambc0[1]=me->ambColor[0][1]/255.f; ambc0[2]=me->ambColor[0][2]/255.f; }
            else               { sb_gx_get_chan_amb(0, ambc0); }
            const float vcol0[3] = { s.clr[0][0]/255.f, s.clr[0][1]/255.f, s.clr[0][2]/255.f };
            float lit[3];
            // Use the vertex's OWN skinning matrix so the normal transforms with the right joint.
            float skin[3][4]; const float (*m)[4] = mtx_for(s); std::memcpy(skin, m, sizeof skin);
            sb_light_vertex_color0(me->chanCtrl[0], matc0, ambc0, lsrc, skin, s.pos, s.nrm, vcol0, lit);
            tv.rgba[0] = lit[0]; tv.rgba[1] = lit[1]; tv.rgba[2] = lit[2];
        }
        // Per-texgen UV: apply the material's GX texgen (matrix/source) so uv[i] = texgen i's
        // output, NOT the raw vertex attribute. The TEV stage samples uv[stage.texcoord]. Slots
        // beyond the texgen count keep the raw attribute (unused — harmless).
        for (int t = 0; t < 8; ++t) {
            if (t < me->ntexgen) sb_texgen_uv(me->tg[t], s, tv.rgba, &tv.uv[t][0]);
            else { tv.uv[t][0] = s.tex[t][0]; tv.uv[t][1] = s.tex[t][1]; }
        }
        return tv;
    };
    // ngx_build_mesh emits a TRIANGLE LIST (idx in triples). Emit clip-space verts straight through;
    // the GPU does the divide, perspective-correct interpolation, and frustum/guard-band clipping.
    for (size_t k = 0; k + 2 < idx.size(); k += 3) {
        g_verts.push_back(make_v(verts[idx[k]]));
        g_verts.push_back(make_v(verts[idx[k+1]]));
        g_verts.push_back(make_v(verts[idx[k+2]]));
    }
    const uint32_t vcount = (uint32_t)g_verts.size() - vstart;
    // SB_FOG_DBG eye-z probe: for a fogged material, report this shape's eye-distance (clip.w) range
    // vs its fog ramp — proves whether the geometry actually reaches the fog band (fog_factor>0) or
    // sits entirely below fog_startz (then zero fog is the FAITHFUL result, matching GX).
    if (me->fog_type && std::getenv("SB_FOG_DBG")) {
        float wmn = 1e30f, wmx = -1e30f;
        for (uint32_t i = vstart; i < (uint32_t)g_verts.size(); ++i) {
            float w = g_verts[i].w; if (w < wmn) wmn = w; if (w > wmx) wmx = w;
        }
        static int s_fz = 0;
        if (s_fz < 24) { ++s_fz;
            std::fprintf(stderr, "[fogz] type=%u ramp=[%.1f,%.1f] shape eyeZ=[%.1f,%.1f] vc=%u -> %s\n",
                         me->fog_type, me->fog_startz, me->fog_endz, wmn, wmx, vcount,
                         (wmx >= me->fog_startz) ? "REACHES FOG" : "below ramp (no fog, faithful)");
        }
    }
    // SB_SKIN_DBG=1: for SKINNED shapes (any mtx group with >1 used matrix), dump the count of
    // DISTINCT draw matrices the vertices selected + the eye-space AABB. A skinned shape should
    // span many matrices and a body-sized AABB; the old flat drawTbl[0] would show 1 matrix + a
    // collapsed AABB. This is the move-a-number proof that per-vertex skinning spreads the mesh.
    if (std::getenv("SB_SKIN_DBG") && sb_present_frame() > 200) {
        int maxUse = 0;
        for (u16 e = 0; e < shape->getMtxGroupNum(); ++e)
            if (J3DShapeMtx* sm = shape->getShapeMtx(e)) maxUse = std::max(maxUse, (int)sm->getUseMtxNum());
        static int s_skin = 0;
        if (maxUse > 1 && s_skin < 24) { ++s_skin;
            std::unordered_map<int,char> seen;
            float ex,ey,ez, mnx=1e30f,mxx=-1e30f,mny=1e30f,mxy=-1e30f,mnz=1e30f,mxz=-1e30f;
            for (const unsigned ii : idx) { const NgxVertex& s = verts[ii];
                int di = 0; if (s.packet < shape->getMtxGroupNum()) if (J3DShapeMtx* sm = shape->getShapeMtx(s.packet)) { u16 d=sm->getUseMtxIndex((u16)(s.matidx/3)); if(d!=0xffff)di=d; }
                seen[di] = 1;
                eye_of(s, ex, ey, ez);
                mnx=std::fmin(mnx,ex);mxx=std::fmax(mxx,ex);mny=std::fmin(mny,ey);mxy=std::fmax(mxy,ey);mnz=std::fmin(mnz,ez);mxz=std::fmax(mxz,ez);
            }
            std::fprintf(stderr, "[skin] model=%p mat=%p maxUse=%d distinctDrawMtx=%zu nv=%zu eyeAABB x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f] (extent %.0f,%.0f,%.0f)\n",
                         (void*)model, (void*)mat, maxUse, seen.size(), idx.size(), mnx,mxx,mny,mxy,mnz,mxz, mxx-mnx,mxy-mny,mxz-mnz);
            // For each distinct draw matrix this shape used, print its translation — a world-scale
            // (|t|~thousands) translation means that entry is model->WORLD (viewCalc didn't run for it).
            for (auto& kv : seen) { int di = kv.first; if (di>=0 && di<drawMtxNum) {
                const float (*dm)[4] = drawTbl[di];
                std::fprintf(stderr, "      drawMtx[%d].t=(%.0f,%.0f,%.0f)\n", di, dm[0][3],dm[1][3],dm[2][3]); } }
        }
    }
    if (dbg_enabled()) {   // per-shape coverage probe (first 24 shapes = the sky, drawn first)
        static int s_cov = 0;
        if (s_cov < 24 && !idx.empty()) { ++s_cov;
            // where does shape's first vert land? eye-space (ez>0 = BEHIND camera) + raw NDC.
            float ex, ey, ez; eye_of(verts[idx[0]], ex, ey, ez);
            SbImmVtx p = imm_project_eye(ex, ey, ez, projType, proj, vp);
            const auto& sv = verts[idx[0]];
            std::fprintf(stderr, "[cov] shape#%d src=%u emit=%u cc0=%04x key=%llx vclr0=%u,%u,%u,%u  eye(%.0f,%.0f,%.0f) ndc(%.2f,%.2f,%.2f)%s\n",
                         s_cov, (unsigned)idx.size(), vcount, me->chanCtrl[0], (unsigned long long)me->key,
                         sv.clr[0][0], sv.clr[0][1], sv.clr[0][2], sv.clr[0][3],
                         ex, ey, ez, p.x, p.y, p.z, ez > 0 ? " BEHIND" : ""); }
    }

    // SB_MAPXLU_PKT: dump every shape flushed under the "DrawBuf MapXlu" buffer (the 2 packets:
    // the sea + the mask) — material key, vColor[0], eye-z range (camera-spanning = the occlusion
    // VOLUME), to settle whether native HAS the tev=2 sea joint (→ TEV-misgen) or only the mask
    // (→ wrong-joint entry). Phase 6 only (the GX-Post flush that paints white).
    if (const char* e = std::getenv("SB_MAPXLU_PKT"); e && e[0] && e[0] != '0'
        && g_capture_phase == 6 && g_capture_drawbuf && std::strstr(g_capture_drawbuf, "MapXlu")
        && !idx.empty()) {
        static int n = 0; if (n < 16 && sb_present_frame() > 200) { ++n;
            float ex,ey,ez, mnz=1e30f,mxz=-1e30f;
            for (const unsigned ii : idx) { eye_of(verts[ii], ex, ey, ez);
                mnz=std::fmin(mnz,ez); mxz=std::fmax(mxz,ez); }
            const auto& sv = verts[idx[0]];
            std::fprintf(stderr, "[mapxlu-pkt] key=%llx vc=%u ntex=%zu vClr0=%u,%u,%u,%u eyeZ[%.0f,%.0f]%s\n",
                         (unsigned long long)me->key, vcount, me->tex.size(),
                         sv.clr[0][0], sv.clr[0][1], sv.clr[0][2], sv.clr[0][3], mnz, mxz,
                         (mnz < 0 && mxz > 0) ? " <-CAMERA-SPANNING-VOLUME" : "");
        }
    }

    // Merge into the previous batch if the same material drew the immediately-preceding
    // shape (J3DMatPacket draws all its shapes consecutively); else open a new batch.
    if (mat == g_last_mat && !g_batches.empty()
        && g_batches.back().phase == (uint8_t)g_capture_phase) {
        g_batches.back().vcount += vcount;
    } else {
        NvkTevBatch b{};
        b.vstart = vstart; b.vcount = vcount;
        fill_batch_material(b, *me);
        b.phase = (uint8_t)g_capture_phase;
        b.dbgName = g_capture_drawbuf;
        // SB_B76_DBG: identify the b76 overbright draw at its TRUE source. The dbgName ("DrawBuf
        // MapXlu") is a GLOBAL stamped by the last TDrawBufObj::perform and can be STALE for a draw
        // that is NOT a draw-buffer flush. Print the captured material's model+modelData name, the
        // live colorUpdate/alphaUpdate, the phase, and the draw-buffer name as it stands — so the
        // post-pass white-saturating composite (key eb5c8e74) is attributed to its real owner.
        if (const char* d = std::getenv("SB_B76_DBG"); d && d[0] && d[0] != '0'
            && (unsigned)(me->key) == 0xc39d96b8u /* low32 of eb5c8e74c39d96b8 */) {
            g_b76_material = (void*)mat;   // publish for SB_ENTRY_MAT=auto (entry-pass backtrace)
            int cu = 1, au = 1; sb_gx_get_color_alpha_update(&cu, &au);
            long calls = 0, lastFalse = -1; sb_gx_colupd_history(&calls, &lastFalse);
            int ring[16]; sb_gx_colupd_ring(ring);
            char rb[40]; for (int i = 0; i < 16; ++i) rb[i] = ring[i] ? '1' : '0'; rb[16] = 0;
            std::fprintf(stderr, "[b76] phase=%d drawbuf=\"%s\" liveCU=%d liveAU=%d colupd_calls=%ld last_false@%ld (delta=%ld) ring=%s model=%p modelData=%p mat=%p key=%llx vc=%u\n",
                         g_capture_phase, g_capture_drawbuf ? g_capture_drawbuf : "(none)",
                         cu, au, calls, lastFalse, calls - lastFalse, rb,
                         (void*)model, (void*)model->getModelData(), (void*)mat,
                         (unsigned long long)me->key, vcount);
            // RASTER-COLOUR DIAGNOSIS (2026-06-30): b76 washes white because the frag doubles its
            // raster colour (vColor) to saturation. Is vColor too bright because the joint is LIT
            // and native's lighting is too bright, or UNLIT and the raw CLR0 read is wrong? Print
            // lit/chanCtrl/matSrcVtx + the raw first-vertex CLR0 + matColor reg + do_light state.
            if (!idx.empty()) {
                const auto& sv0 = verts[idx[0]];
                const bool do_light_dbg = me->lit && nlights > 0;
                std::fprintf(stderr, "[b76-raster] lit=%d nlights=%d do_light=%d cc0=%04x matSrcVtx0=%d "
                             "rawCLR0=%u,%u,%u,%u matColor0=%u,%u,%u,%u\n",
                             (int)me->lit, nlights, (int)do_light_dbg, me->chanCtrl[0],
                             (int)me->matSrcVtx[0], sv0.clr[0][0], sv0.clr[0][1], sv0.clr[0][2], sv0.clr[0][3],
                             me->matColor[0][0], me->matColor[0][1], me->matColor[0][2], me->matColor[0][3]);
            }
            // SB_B76_BT=1: one backtrace per phase, to NAME the perform-list entry / TViewObj that
            // draws this MapXlu mask in ph1 (unk40) and ph6 (GXPost) — the pass-routing owner.
            if (const char* bt = std::getenv("SB_B76_BT"); bt && bt[0] && bt[0] != '0') {
                static int btdone[8] = {0};
                int ph = g_capture_phase & 7;
                if (!btdone[ph]) { btdone[ph] = 1;
                    void* fr[40]; int nf = backtrace(fr, 40);
                    std::fprintf(stderr, "[b76-bt] phase=%d stack:\n", g_capture_phase);
                    backtrace_symbols_fd(fr, nf, 2);
                }
            }
        }
        g_batches.push_back(b);
        g_last_mat = mat;
    }

    static long s_shapes = 0;
    if (dbg_enabled()) { ++s_shapes;
        if ((s_shapes % 4000) == 0)
            std::fprintf(stderr, "[j3dcap] shapes=%ld verts=%zu batches=%zu materials=%zu\n",
                         s_shapes, g_verts.size(), g_batches.size(), g_matcache.size()); }
    return true;
}

// Passthrough TEV fragment (out = raster colour, no textures/combiner) — the shader for solid
// PASSCLR geometry like the sky backdrop. Built once; the string outlives every batch. Must match
// sms_boot_present.cpp::ensure_pass_shader EXACTLY: a single RASC/RASA stage (a bare default
// NgxTevState outputs the zero PREV register = BLACK, and swap_table=0 is the "rrrr" trap).
static const std::string& pass_fragment() {
    static std::string frag = []{
        NgxTevState st{};
        st.num_stages = 1;
        st.stage[0].color_env = (15u<<12)|(15u<<8)|(15u<<4)|10u | (1u<<19);  // cc out = RASC
        st.stage[0].alpha_env = (7u<<13)|(7u<<10)|(7u<<7)|(5u<<4) | (1u<<19); // ac out = RASA
        st.stage[0].texmap = 0xff; st.stage[0].texcoord = 0xff; st.stage[0].color_chan = 0;
        for (int i = 0; i < 4; ++i) st.swap_table[i] = 0x1B;                  // identity swizzle
        return sb_tev_gen_fragment(st);
    }();
    return frag;
}

// GXDrawSphere capture (the TSky procedural backdrop dome). Un-stubbed GXDrawSphere
// (gx_fb_impl.cpp) calls this. TSky sets GX_PNMTX0 = scale(100000) (NO view — so the dome is
// centred on the CAMERA in eye space) + GXSetChanMatColor(COLOR0A0, blue) + TEV PASSCLR, then
// GXDrawSphere(numMajor,numMinor) (GXDraw.c: a unit sphere of triangle strips). We replicate
// the sphere, transform by the current pos matrix → eye space, colour every vertex with the
// matColor, and run it through the SAME near+side clipper as the scene (it surrounds the
// camera, so it needs the near-clip), emitting a depth-tested PASSCLR batch BEHIND the scene.
extern "C" void sb_boot_capture_sphere(int numMajor, int numMinor) {
    if (g_locked) return;   // capture-once-per-present (see sb_boot_capture_begin_scene)
    if (numMajor < 1 || numMinor < 1) return;
    HostAllocScope _hostalloc;
    if (g_consumed) { g_verts.clear(); g_batches.clear(); g_last_mat = nullptr; g_consumed = false; }
    if (g_verts.size() > 6u * 1024 * 1024) return;

    float posMtx[3][4]; sb_gx_get_cur_posmtx(posMtx);
    int projType; float proj[6], vp[6]; sb_gx_get_projection(&projType, proj, vp);
    float mc[4]; sb_gx_get_chan_matcolor(0, mc);
    if (std::getenv("SB_SKY_RED")) { mc[0]=1.f; mc[1]=0.f; mc[2]=0.f; mc[3]=1.f; }  // diag

    NvkTevVertex base{};
    base.rgba[0]=mc[0]; base.rgba[1]=mc[1]; base.rgba[2]=mc[2]; base.rgba[3]=mc[3];
    base.rgba1[0]=mc[0]; base.rgba1[1]=mc[1]; base.rgba1[2]=mc[2]; base.rgba1[3]=mc[3];
    // model -> eye (camera-centred pos matrix) -> CLIP-space xyzw (GPU divides + near-clips the
    // triangles that straddle ez=0 — the dome surrounds the camera). Same PC-way path as the scene.
    auto to_clip = [&](float mx, float my, float mz) -> NvkTevVertex {
        const float ex = posMtx[0][3] + posMtx[0][0]*mx + posMtx[0][1]*my + posMtx[0][2]*mz;
        const float ey = posMtx[1][3] + posMtx[1][0]*mx + posMtx[1][1]*my + posMtx[1][2]*mz;
        const float ez = posMtx[2][3] + posMtx[2][0]*mx + posMtx[2][1]*my + posMtx[2][2]*mz;
        SbImmClip c = imm_project_eye_clip(ex, ey, ez, projType, proj, vp);
        NvkTevVertex tv = base; tv.x = c.x; tv.y = c.y; tv.z = c.z; tv.w = c.w; return tv;
    };

    const uint32_t vstart = (uint32_t)g_verts.size();
    const float majorStep = 3.14159265f / numMajor, minorStep = 6.2831853f / numMinor;
    for (int i = 0; i < numMajor; ++i) {           // GXDraw.c GXDrawSphere, unit radius
        const float a = i*majorStep, b = a + majorStep;
        const float r0 = std::sin(a), r1 = std::sin(b), z0 = std::cos(a), z1 = std::cos(b);
        NvkTevVertex strip[2*256 + 2];             // (numMinor+1)*2 verts; numMinor is small (<=256)
        int sn = 0;
        const int nm = numMinor < 255 ? numMinor : 255;
        for (int j = 0; j <= nm; ++j) {
            const float c = j*minorStep, x = std::cos(c), y = std::sin(c);
            strip[sn++] = to_clip(x*r1, y*r1, z1);
            strip[sn++] = to_clip(x*r0, y*r0, z0);
        }
        for (int j = 0; j + 2 < sn; ++j) {         // triangle strip -> triangles (cull-none)
            g_verts.push_back(strip[j]); g_verts.push_back(strip[j+1]); g_verts.push_back(strip[j+2]);
        }
    }
    // Backdrop depth: the sphere (radius 100000) projects to NDC z==1.0 EXACTLY — the far-clip
    // boundary, which the GPU rejects. Pin it just inside the far plane so it passes depth-clip +
    // the LEQUAL test while every nearer surface (the scene at z<1) still draws over it. This is
    // the standard "skybox at max depth" technique, not a fudge of the geometry.
    //
    // The pin MUST sit strictly BEHIND the sky.bmd gradient dome (the "空グループ" model, drawn
    // immediately after this sphere). That gradient is a FINITE dome whose far rim projects to
    // z≈0.99992 — genuinely nearer than this radius-100000 backdrop, so it must always win LEQUAL
    // and cover the backdrop completely (the real game shows only the gradient, never the deep-blue
    // dome). A 0.9999 pin under-stated the backdrop's depth: it landed NEARER than the gradient's
    // far rim (0.99992 > 0.9999), so the gradient lost LEQUAL at the horizon band and the deep-blue
    // sphere bled through as a "dome" (the file-select sky bug). Pin to 0.99997 — safely behind the
    // gradient rim, still inside the far plane.
    constexpr float kBackdropZ = 0.99997f;   // NDC depth (= clip_z/clip_w); pin clip_z to kBackdropZ*w
    for (uint32_t i = vstart; i < g_verts.size(); ++i) {
        NvkTevVertex& v = g_verts[i];
        if (v.w > 0.f && v.z >= kBackdropZ * v.w) v.z = kBackdropZ * v.w;
    }

    const uint32_t vcount = (uint32_t)g_verts.size() - vstart;
    if (!vcount) return;

    NvkTevBatch b{};
    b.vstart = vstart; b.vcount = vcount;
    b.fragGlsl = pass_fragment().c_str(); b.shaderKey = fnv64(pass_fragment().c_str());
    std::memset(b.push.kcolor, 0xFF, sizeof b.push.kcolor);   // konst=1 (identity) — the passthrough
    std::memset(b.push.tevreg, 0, sizeof b.push.tevreg);      // shader multiplies raster by konst
    b.z_test = 1; b.z_func = 3 /*GX_LEQUAL*/; b.z_write = 1;   // backdrop: write far depth, scene draws over
    b.blend_mode = 0;
    g_batches.push_back(b);
    g_last_mat = nullptr;   // not a J3DMaterial; don't let the next shape merge into this batch
    if (dbg_enabled()) {
        float xmn=1e9f,xmx=-1e9f,ymn=1e9f,ymx=-1e9f,zmn=1e9f,zmx=-1e9f; int onscr=0, top=0, bot=0;
        for (uint32_t i=vstart;i<g_verts.size();++i){ const NvkTevVertex&t=g_verts[i];
            xmn=std::fmin(xmn,t.x);xmx=std::fmax(xmx,t.x);ymn=std::fmin(ymn,t.y);ymx=std::fmax(ymx,t.y);
            zmn=std::fmin(zmn,t.z);zmx=std::fmax(zmx,t.z);
            if(t.x>=-1&&t.x<=1&&t.y>=-1&&t.y<=1&&t.z>=0&&t.z<=1)++onscr;
            if(t.y>=-1&&t.y<=1){ if(t.y<0)++top; else ++bot; } }
        std::fprintf(stderr, "[sky-sphere-vdist] in-box verts top(y<0)=%d bottom(y>0)=%d\n", top, bot);
        std::fprintf(stderr, "[sky-sphere] major=%d minor=%d emit=%u onscr=%d col=%.2f,%.2f,%.2f "
                     "ndcX[%.2f,%.2f] ndcY[%.2f,%.2f] ndcZ[%.2f,%.2f] projType=%d proj[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f] vp[%.0f,%.0f,%.0f,%.0f,%.2f,%.2f]\n",
                     numMajor, numMinor, vcount, onscr, mc[0], mc[1], mc[2],
                     xmn,xmx,ymn,ymx,zmn,zmx, projType, proj[0],proj[1],proj[2],proj[3],proj[4],proj[5],
                     vp[0],vp[1],vp[2],vp[3],vp[4],vp[5]);
    }
}

// Begin a fresh scene-capture frame. scene_drive calls this at the very start of each
// scene draw (before drive_sky/drive_map/scene->perform), so one captured frame == one
// drawn scene. Without it, multiple TMarDirector::direct() calls between two VI presents
// (the logic loop can run faster than retrace under TURBO) accumulate 2-3 duplicate scene
// copies into the same buffer — they interleave at the horizon (sky/sea/white-overlay
// triangles from different copies covering the same pixels) and composite blended layers
// N×, producing the dithered horizon band. Resetting per scene draw keeps only the latest
// full frame; the present then drains exactly one scene. (Same g_consumed mechanism as a
// take, so the next append clears — but here the reset is keyed to the draw, not the present.)
extern "C" void sb_boot_capture_frame_begin() {
    g_consumed = true;
}

// ── Capture-once-per-present lock ──────────────────────────────────────────────────────────────
// The capture taps J3DShape::draw, so BOTH the game's real perform-list draw (TMarDirector::direct's
// render branch: PerformList GX / the conductor / etc.) AND the hand-driven sb_boot_drive_scene walk
// feed it — and under TURBO direct() fires MANY times per VI present. The real-path captures were
// always discarded (drive_scene's capture_frame_begin reset the buffer), so re-walking the whole
// scene every direct() (and twice: real-path + drive_scene) was pure waste — fatal once the scene
// holds the NPC population (thousands of shapes -> a frame never finished).
// Now drive_scene captures the authoritative scene EXACTLY ONCE per present, bracketed by
// begin/end_scene, and the capture is LOCKED (every sb_boot_capture_j3d/sphere returns early) the
// rest of the interval — so the redundant real-path and repeat-direct() captures are skipped.
// g_locked starts false so the SB_NO_DRIVE_SCENE bisection mode (drive_scene returns before
// begin_scene, never locks) still captures the real path. g_want_capture is re-armed by the present
// consuming the buffer, so exactly one drive_scene walk lands per shown frame.
//
// Called by drive_scene before its sky/scene/chr draws. Returns 1 if this is the first drive_scene
// of a new present interval (capture now), 0 to skip. On 1 it resets the buffer and unlocks capture.
extern "C" int sb_boot_capture_begin_scene() {
    if (!g_want_capture) return 0;
    g_want_capture = false;
    g_locked = false;
    g_consumed = true;   // discard anything captured earlier this interval; next draw clears
    g_efb_copies.clear();
    g_present_shapes = g_present_idx = g_present_skipped = 0;
    g_present_shapes_distinct_n = 0;
    return 1;
}

// Once-per-present gate for the immediate-mode wave (drive_wave). Returns 1 on the first call of a
// present interval (draw the wave now), 0 afterwards (skip — already appended). Re-armed at present
// drain (sb_boot_capture_tev_take). Independent of the J3D capture lock so it works in BOTH the
// OWN_GXLIST and hand-driven paths. Prevents the TURBO multi-direct() wave-accumulation overdraw.
extern "C" int sb_boot_wave_begin() {
    if (!g_wave_want) return 0;
    g_wave_want = false;
    return 1;
}

// SB_OWN_GXLIST: TMarDirector::direct stamps the current perform-list index before each list runs
// (1=unk40, 2=unk38, 3=unk3C, 4=mPerformListGX, 5=Silhouette, 6=mPerformListGXPost) so every batch
// captured carries its source pass. Reset to 0 in end_scene.
extern "C" void sb_boot_capture_set_phase(int phase) { g_capture_phase = phase; }
extern "C" int  sb_boot_capture_phase() { return g_capture_phase; }   // SB_DBHEAD_DBG cross-ref
extern "C" void* sb_b76_material() { return g_b76_material; }

// Active draw-buffer name, stamped by TDrawBufObj::perform(flag&8) before mDrawBuffer->draw() flushes
// it (the J3DShape::draw tap source). Lets the overbright harness attribute a batch (e.g. b76) to its
// source draw buffer ("DrawBuf Sky Xlu" / "...LensFlare" / etc.) by NAME instead of guessing. The name
// strings are static (the draw buffers outlive the program), so the pointer is safe to keep on a batch.
extern "C" void sb_boot_capture_set_drawbuf(const char* name) { g_capture_drawbuf = name; }

// GXCopyTex tap (gx_fb_impl.cpp): record an EFB-copy render-target boundary at the current batch
// position. Only while a scene capture is live (not locked) so it aligns with g_batches indices.
extern "C" void sb_boot_capture_efb_copy(const void* dest, int clear, int wd, int ht) {
    if (g_locked || g_consumed) return;
    g_efb_copies.push_back({g_batches.size(), dest, clear != 0, wd, ht});
    if (dest) g_efb_dest_seen.insert(dest);   // persistent: recognize the consumer regardless of order
    if (const char* v = std::getenv("SB_EFB_PROBE"); v && v[0] && v[0] != '0') {
        static long n = 0; if (n < 12) { ++n;
            std::fprintf(stderr, "[efbprobe] COPY dest=%p clear=%d %dx%d (phase %d, batch %zu)\n",
                         dest, clear, wd, ht, g_capture_phase, g_batches.size()); }
    }
    if (dbg_enabled())
        std::fprintf(stderr, "[efbcopy] mark batch_index=%zu dest=%p clear=%d %dx%d (phase %d)\n",
                     g_batches.size(), dest, clear, wd, ht, g_capture_phase);
}
extern "C" int sb_boot_capture_efb_copy_count() { return (int)g_efb_copies.size(); }
// True if `src` (a resolved texmap image-data host pointer) is the destination of an EFB copy
// recorded this frame — i.e. this texmap samples a snapshot of the EFB, not a real asset texture.
// (The post-pass soft-focus/bloom composite quads.) Used to attribute the overbright.
extern "C" int sb_boot_capture_texsrc_is_efb_dest(const void* src) {
    if (!src) return 0;
    for (const auto& c : g_efb_copies) if (c.dest == src) return 1;
    // Fall back to the persistent across-frames set: composite3 binds a stable EFB-copy dest address
    // but is captured before this frame's copy is recorded (see g_efb_dest_seen).
    return g_efb_dest_seen.count(src) ? 1 : 0;
}
// The batch index of the LAST EFB→texture copy that CLEARED the EFB this frame (or -1 if none).
// On GC that clear wipes the EFB, so every batch captured before it was snapshotted to a texture
// and is NOT in the visible framebuffer — the native present drops them to stop the pre-pass
// double-composite (the file-select overbright). Returns the boundary into the present batch list.
extern "C" int sb_boot_capture_last_clear_boundary() {
    int b = -1;
    for (const auto& c : g_efb_copies) if (c.clear) b = (int)c.batch_index;
    return b;
}

// Export the full ordered EFB-copy list this frame for the segmented present. Each entry is one
// EFB→texture copy boundary: `batch_index` (into the scene batch list — batches captured before it
// were rendered to that target), `dest` (the snapshot key, == a consumer's Tex::efb_src), `clear`
// (whether the copy wiped the EFB → the NEXT segment starts on a cleared framebuffer), and the copy
// dims. Returns the number written (≤ max). Layout mirrors SbEfbCopy in sms_boot_present.cpp.
struct SbEfbCopyOut { int batch_index; const void* dest; int clear; int wd; int ht; };
extern "C" int sb_boot_capture_efb_copies(SbEfbCopyOut* out, int max) {
    int n = 0;
    for (const auto& c : g_efb_copies) {
        if (n >= max) break;
        out[n].batch_index = (int)c.batch_index; out[n].dest = c.dest;
        out[n].clear = c.clear ? 1 : 0; out[n].wd = c.wd; out[n].ht = c.ht;
        ++n;
    }
    return n;
}

// Called by drive_scene after its draws — relock so the rest of the interval's draws are skipped.
extern "C" void sb_boot_capture_end_scene() {
    g_locked = true;
    g_capture_phase = 0;
    if (const char* e = std::getenv("SB_CAP_COUNT"); e && e[0] && e[0] != '0') {
        static long n = 0; ++n;
        std::fprintf(stderr, "[capcount] present-walk #%ld: shapes=%ld distinct_shapes=%d idx_sum=%ld (~%ld tris) "
                     "g_verts=%zu skipped=%ld\n",
                     n, g_present_shapes, g_present_shapes_distinct_n, g_present_idx, g_present_idx / 3,
                     g_verts.size(), g_present_skipped);
        // Per-perform-list-phase triangle breakdown (1=unk40,2=unk38,3=unk3C,4=mPerformListGX,
        // 5=Silhouette,6=GXPost). Localizes which phase is short vs the oracle's 16651 scene tris.
        long ph_v[8] = {0}, ph_b[8] = {0};
        for (const auto& b : g_batches) { int p = b.phase & 7; ph_v[p] += b.vcount; ph_b[p]++; }
        std::fprintf(stderr, "[capcount]   by-phase tris:");
        for (int p = 0; p < 8; ++p) if (ph_v[p]) std::fprintf(stderr, " ph%d=%ld(%ldb)", p, ph_v[p]/3, ph_b[p]);
        std::fprintf(stderr, "\n");
        // Per-(phase, draw-buffer) triangle breakdown — names WHICH buffer/group each phase's tris come
        // from, so a scene under-draw can be pinned to a specific buffer (MapOpa/MapXlu/半透明優先/Sky/
        // Chr) vs the oracle. dbgName is nullptr for directly-performed TViewObjs (shown as "(direct)").
        {
            std::map<std::pair<int,std::string>, long> byBuf;
            for (const auto& b : g_batches) {
                std::string nm = b.dbgName ? b.dbgName : "(direct)";
                byBuf[{b.phase & 7, nm}] += b.vcount;
            }
            for (const auto& kv : byBuf)
                std::fprintf(stderr, "[capcount]     ph%d %-32s %ld tris\n",
                             kv.first.first, kv.first.second.c_str(), kv.second/3);
        }
        std::fflush(stderr);
    }
    // ORDERED per-draw GX-state dump (SB_GXDRAW) — one line per batch in draw/flush order. Mirrors
    // the oracle's SUNBRIGHT_DBG_GXDRAW (gx_capture.cpp) field-for-field so tools/render/gxstate_diff.py
    // can group both sides by GX-state SIGNATURE (blend/tev/proj) and diff colorUpdate per signature —
    // the deterministic native-vs-oracle diff that ends the pass/phase log-reading oscillation. The
    // native blend_mode (GXBlendMode: 0 none / 1 blend / 2 logic / 3 subtract) is normalised to the
    // oracle's blend_enable + subtract booleans. proj is not per-batch on native, so emit '?'.
    if (const char* e = std::getenv("SB_GXDRAW"); e && e[0] && e[0] != '0') {
        static long fr = 0; ++fr;
        for (size_t i = 0; i < g_batches.size(); ++i) {
            const NvkTevBatch& b = g_batches[i];
            int be  = (b.blend_mode == 1 || b.blend_mode == 3) ? 1 : 0;  // BLEND or SUBTRACT both enable blending
            int sub = (b.blend_mode == 3) ? 1 : 0;
            std::fprintf(stderr,
                "[gxdraw] fr=%ld i=%zu phase=%u drawbuf=%s key=%08x cU=%u aU=%u be=%d src=%u dst=%u sub=%d tev=%u v=%u\n",
                fr, i, b.phase, b.dbgName ? b.dbgName : "-",
                (unsigned)(b.shaderKey >> 32), b.color_update, b.alpha_update,
                be, b.src_factor, b.dst_factor, sub, b.num_stages, b.vcount);
        }
        std::fflush(stderr);
    }
}

// Present drains the frame's captured scene: the vertex list + per-material batches. Returns the
// vertex count; *batches/*nbatches point at the batch list. Marks the buffers consumed (next
// append clears). The NvkTevBatch fragGlsl/tex pointers stay valid (owned by g_matcache).
// Running counts of what's been captured into the current frame so far — used by scene_drive's
// SB_PASS_VERTS diagnostic to attribute the per-pass vertex/batch deltas (which native sub-pass
// — sky / scene-perform / chr / hud — contributes how much), to localize a geometry gap vs the
// per-pass oracle. Cheap: just the live vector sizes.
extern "C" int sb_boot_capture_vert_count(void)  { return (int)g_verts.size(); }
extern "C" int sb_boot_capture_batch_count(void) { return (int)g_batches.size(); }

int sb_boot_capture_tev_take(const NvkTevVertex** verts,
                             const NvkTevBatch** batches, int* nbatches) {
    if (verts)    *verts    = g_verts.empty() ? nullptr : g_verts.data();
    if (batches)  *batches  = g_batches.empty() ? nullptr : g_batches.data();
    if (nbatches) *nbatches = (int)g_batches.size();
    int n = (int)g_verts.size();
    g_consumed = true;
    g_want_capture = true;   // re-arm: the next drive_scene captures a fresh frame for this present
    g_wave_want = true;      // re-arm the imm-wave once-per-present gate (see drive_wave)
    return n;
}

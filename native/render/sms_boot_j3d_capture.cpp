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

#include "nvk.h"               // NvkTevVertex, Nvk::NvkTevBatch, NvkTevPush
#include "gx_imm_xform.h"      // SbImmRawVtx / SbImmVtx / imm_project
#include "sb_tri_clip.h"       // SbClipEyeVtx / sb_clip_emit_tri (shared near+side clipper)
#include "ngx_mesh.h"          // NgxCP, NgxVertex, ngx_build_mesh
#include "ngx_render_data.h"   // NgxTevState
#include "tev_shader.h"        // sb_tev_gen_fragment
#include "sms_boot_material.h" // sb_build_tev_state, sb_resolve_textures, SbTexImage
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp> // J3DTexture::getNum (table-size probe)
#include "sms_boot_lighting.h" // sb_light_vertex_color0 (pure, unit-tested)
#include "ngx_light.h"         // ngx::LightSrc

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

using namespace sb::render;

extern "C" int  sb_present_frame(void);   // sms_boot_present.cpp — settled-frame gating
extern "C" int  sb_camera_view_settled(void);   // scene_drive.cpp — view matrix stationary
extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]);
extern "C" int  sb_gx_get_lights(float out[8][16]);
extern "C" void sb_gx_get_chan_amb(int slot, float rgb[3]);
extern "C" void sb_gx_get_chan_matcolor(int slot, float rgba[4]);
extern "C" void sb_gx_get_cur_posmtx(float m[3][4]);
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
};
std::unordered_map<J3DMaterial*, MatEntry> g_matcache;

// Frame-global output: the present-ready vertex list + per-material batches. Cleared on the
// first append after the present consumed them (take sets g_consumed).
std::vector<NvkTevVertex>   g_verts;
std::vector<Nvk::NvkTevBatch> g_batches;
bool g_consumed = true;
J3DMaterial* g_last_mat = nullptr;   // for consecutive-shape batch merging within a frame

const MatEntry* get_mat_entry(J3DMaterial* mat, J3DTexture* modelTex) {
    auto it = g_matcache.find(mat);
    if (it != g_matcache.end()) return &it->second;
    MatEntry e;
    NgxTevState st{};
    if (sb_build_tev_state(mat, st)) {
        e.frag = sb_tev_gen_fragment(st);
        e.key  = fnv64(e.frag.c_str());
        for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) {
            e.push.kcolor[c][k] = st.kcolor[c][k];
            e.push.tevreg[c][k] = st.tev_color[c][k];
        }
        e.z_test = st.pe.z_test; e.z_func = st.pe.z_func; e.z_write = st.pe.z_write;
        e.blend_mode = st.pe.blend_mode; e.src_factor = st.pe.src_factor; e.dst_factor = st.pe.dst_factor;
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
                             "at=%u bm=%u msv0=%d matc0=%u,%u,%u,%u ntex=%zu key=%llx\n",
                             st.num_stages, ce, (ce>>12)&0xf,(ce>>8)&0xf,(ce>>4)&0xf,ce&0xf,
                             st.stage[0].color_chan, st.stage[0].texmap, st.pe.alpha_test,
                             (st.pe.blend_mode | (st.pe.src_factor<<4) | (st.pe.dst_factor<<8)), (int)e.matSrcVtx[0],
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

// Fill an NvkTevBatch's tex[] slots + push/state from a MatEntry (called when opening a batch).
void fill_batch_material(Nvk::NvkTevBatch& b, const MatEntry& e) {
    b.push = e.push; b.shaderKey = e.key; b.fragGlsl = e.frag.c_str();
    b.z_test = e.z_test; b.z_func = e.z_func; b.z_write = e.z_write;
    b.blend_mode = e.blend_mode; b.src_factor = e.src_factor; b.dst_factor = e.dst_factor;
    for (const SbTexImage& t : e.tex) {
        if (t.slot < 0 || t.slot >= 8 || t.rgba.empty()) continue;
        b.tex[t.slot].rgba = (const uint8_t*)t.rgba.data();
        b.tex[t.slot].w = t.w; b.tex[t.slot].h = t.h; b.tex[t.slot].linear = t.linear ? 1 : 0;
        b.tex[t.slot].min_filter = t.min_filter; b.tex[t.slot].max_aniso = t.max_aniso;
        b.tex[t.slot].wrap_s = t.wrap_s; b.tex[t.slot].wrap_t = t.wrap_t;
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
    if (!shape) return false;
    HostAllocScope _hostalloc;

    J3DModel* model = j3dSys.getModel();
    if (!model || !model->getModelData()) return true;

    J3DVertexData* vd = shape->unk44;
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

    NgxCP cp{};
    if (!build_native_cp(shape, *vd, base, cp)) return true;

    ResolveCtx rc{ base, 0 };
    std::vector<NgxVertex> verts;
    std::vector<unsigned> idx;
    for (u16 e = 0; e < shape->getMtxGroupNum(); ++e) {
        J3DShapeDraw* dp = shape->getShapeDraw(e);
        if (!dp || !dp->getDisplayList()) continue;
        ngx_build_mesh(cp, dp->getDisplayList(), dp->getDisplayListSize(),
                       resolve_native, &rc, verts, idx);
    }
    if (idx.empty()) return true;

    int projType; float proj[6]; float vp[6];
    sb_gx_get_projection(&projType, proj, vp);
    float ident[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    float posMtx[3][4];
    Mtx* drawTbl = shape->mDrawMatrices ? shape->mDrawMatrices[*shape->mCurrentViewNo] : nullptr;
    if (drawTbl) std::memcpy(posMtx, &drawTbl[0][0], sizeof(posMtx));
    else         std::memcpy(posMtx, ident, sizeof(posMtx));

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
    g_verts.reserve(g_verts.size() + idx.size());

    // Build one eye-space + shaded vertex (NDC filled by the clipper at projection time).
    auto make_cv = [&](const NgxVertex& s) -> SbClipEyeVtx {
        SbClipEyeVtx cv;
        cv.ex = posMtx[0][3] + posMtx[0][0]*s.pos[0] + posMtx[0][1]*s.pos[1] + posMtx[0][2]*s.pos[2];
        cv.ey = posMtx[1][3] + posMtx[1][0]*s.pos[0] + posMtx[1][1]*s.pos[1] + posMtx[1][2]*s.pos[2];
        cv.ez = posMtx[2][3] + posMtx[2][0]*s.pos[0] + posMtx[2][1]*s.pos[1] + posMtx[2][2]*s.pos[2];
        NvkTevVertex& tv = cv.tv; tv = NvkTevVertex{};
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
            sb_light_vertex_color0(me->chanCtrl[0], matc0, ambc0, lsrc, posMtx, s.pos, s.nrm, vcol0, lit);
            tv.rgba[0] = lit[0]; tv.rgba[1] = lit[1]; tv.rgba[2] = lit[2];
        }
        for (int t = 0; t < 8; ++t) { tv.uv[t][0] = s.tex[t][0]; tv.uv[t][1] = s.tex[t][1]; }
        return cv;
    };
    // ngx_build_mesh emits a TRIANGLE LIST (idx in triples); the shared clipper (sb_tri_clip.h)
    // near-clips in eye space, projects, side-clips against the NDC box, and appends to g_verts.
    for (size_t k = 0; k + 2 < idx.size(); k += 3) {
        const SbClipEyeVtx tri[3] = { make_cv(verts[idx[k]]), make_cv(verts[idx[k+1]]), make_cv(verts[idx[k+2]]) };
        sb_clip_emit_tri(tri, projType, proj, vp, g_verts);
    }
    const uint32_t vcount = (uint32_t)g_verts.size() - vstart;
    if (dbg_enabled()) {   // per-shape coverage probe (first 24 shapes = the sky, drawn first)
        static int s_cov = 0;
        if (s_cov < 24 && !idx.empty()) { ++s_cov;
            // where does shape's first vert land? eye-space (ez>0 = BEHIND camera) + raw NDC.
            const SbClipEyeVtx c = make_cv(verts[idx[0]]);
            SbImmVtx p = imm_project_eye(c.ex, c.ey, c.ez, projType, proj, vp);
            std::fprintf(stderr, "[cov] shape#%d src=%u emit=%u cc0=%04x  eye(%.0f,%.0f,%.0f) ndc(%.2f,%.2f,%.2f)%s\n",
                         s_cov, (unsigned)idx.size(), vcount, me->chanCtrl[0],
                         c.ex, c.ey, c.ez, p.x, p.y, p.z, c.ez > 0 ? " BEHIND" : ""); }
    }

    // Merge into the previous batch if the same material drew the immediately-preceding
    // shape (J3DMatPacket draws all its shapes consecutively); else open a new batch.
    if (mat == g_last_mat && !g_batches.empty()) {
        g_batches.back().vcount += vcount;
    } else {
        Nvk::NvkTevBatch b{};
        b.vstart = vstart; b.vcount = vcount;
        fill_batch_material(b, *me);
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
    auto to_eye = [&](float mx, float my, float mz) -> SbClipEyeVtx {
        SbClipEyeVtx o;
        o.ex = posMtx[0][3] + posMtx[0][0]*mx + posMtx[0][1]*my + posMtx[0][2]*mz;
        o.ey = posMtx[1][3] + posMtx[1][0]*mx + posMtx[1][1]*my + posMtx[1][2]*mz;
        o.ez = posMtx[2][3] + posMtx[2][0]*mx + posMtx[2][1]*my + posMtx[2][2]*mz;
        o.tv = base; return o;
    };

    const uint32_t vstart = (uint32_t)g_verts.size();
    const float majorStep = 3.14159265f / numMajor, minorStep = 6.2831853f / numMinor;
    for (int i = 0; i < numMajor; ++i) {           // GXDraw.c GXDrawSphere, unit radius
        const float a = i*majorStep, b = a + majorStep;
        const float r0 = std::sin(a), r1 = std::sin(b), z0 = std::cos(a), z1 = std::cos(b);
        SbClipEyeVtx strip[2*256 + 2];             // (numMinor+1)*2 verts; numMinor is small (<=256)
        int sn = 0;
        const int nm = numMinor < 255 ? numMinor : 255;
        for (int j = 0; j <= nm; ++j) {
            const float c = j*minorStep, x = std::cos(c), y = std::sin(c);
            strip[sn++] = to_eye(x*r1, y*r1, z1);
            strip[sn++] = to_eye(x*r0, y*r0, z0);
        }
        for (int j = 0; j + 2 < sn; ++j) {         // triangle strip -> triangles (cull-none)
            const SbClipEyeVtx tri[3] = { strip[j], strip[j+1], strip[j+2] };
            sb_clip_emit_tri(tri, projType, proj, vp, g_verts);
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
    constexpr float kBackdropZ = 0.99997f;
    for (uint32_t i = vstart; i < g_verts.size(); ++i)
        if (g_verts[i].z >= kBackdropZ) g_verts[i].z = kBackdropZ;

    const uint32_t vcount = (uint32_t)g_verts.size() - vstart;
    if (!vcount) return;

    Nvk::NvkTevBatch b{};
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

// Present drains the frame's captured scene: the vertex list + per-material batches. Returns the
// vertex count; *batches/*nbatches point at the batch list. Marks the buffers consumed (next
// append clears). The NvkTevBatch fragGlsl/tex pointers stay valid (owned by g_matcache).
int sb_boot_capture_tev_take(const NvkTevVertex** verts,
                             const Nvk::NvkTevBatch** batches, int* nbatches) {
    if (verts)    *verts    = g_verts.empty() ? nullptr : g_verts.data();
    if (batches)  *batches  = g_batches.empty() ? nullptr : g_batches.data();
    if (nbatches) *nbatches = (int)g_batches.size();
    int n = (int)g_verts.size();
    g_consumed = true;
    return n;
}

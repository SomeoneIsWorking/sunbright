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
#include "ngx_mesh.h"          // NgxCP, NgxVertex, ngx_build_mesh
#include "ngx_render_data.h"   // NgxTevState
#include "tev_shader.h"        // sb_tev_gen_fragment
#include "sms_boot_material.h" // sb_build_tev_state, sb_resolve_textures, SbTexImage
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

extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]);
extern "C" int  sb_gx_get_lights(float out[8][16]);
extern "C" void sb_gx_get_chan_amb(int slot, float rgb[3]);
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

const MatEntry* get_mat_entry(J3DMaterial* mat) {
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
        sb_resolve_textures(mat, j3dSys.getTexture(), e.tex);
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
    const MatEntry* me = get_mat_entry(mat);
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

    // Eye-space vertex + final shaded attributes (NDC filled in after the near-plane clip).
    // GX geometry that SURROUNDS the camera (the sky dome) has triangles straddling ez=0; the
    // perspective divide (w = 1/-ez in imm_project_eye) flips/explodes behind-camera verts into
    // garbage NDC, so those triangles get dropped -> black sky. We clip each triangle against the
    // near plane in EYE space (Sutherland-Hodgman), then project the survivors.
    struct ClipVtx { float ex, ey, ez; NvkTevVertex tv; };
    auto make_cv = [&](const NgxVertex& s) -> ClipVtx {
        ClipVtx cv;
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
    auto lerp_cv = [](const ClipVtx& A, const ClipVtx& B, float t) -> ClipVtx {
        ClipVtx R;
        R.ex = A.ex + t*(B.ex-A.ex); R.ey = A.ey + t*(B.ey-A.ey); R.ez = A.ez + t*(B.ez-A.ez);
        for (int k = 0; k < 4; ++k) { R.tv.rgba[k]  = A.tv.rgba[k]  + t*(B.tv.rgba[k]-A.tv.rgba[k]);
                                      R.tv.rgba1[k] = A.tv.rgba1[k] + t*(B.tv.rgba1[k]-A.tv.rgba1[k]); }
        for (int u = 0; u < 8; ++u) { R.tv.uv[u][0] = A.tv.uv[u][0] + t*(B.tv.uv[u][0]-A.tv.uv[u][0]);
                                      R.tv.uv[u][1] = A.tv.uv[u][1] + t*(B.tv.uv[u][1]-A.tv.uv[u][1]); }
        return R;
    };
    int dbg_onscreen = 0; float dbg_ymin = 1e9f, dbg_ymax = -1e9f; float dbg_lastcol[3] = {0,0,0};
    auto emit_cv = [&](const ClipVtx& cv) {
        SbImmVtx p = imm_project_eye(cv.ex, cv.ey, cv.ez, projType, proj, vp);
        NvkTevVertex tv = cv.tv; tv.x = p.x; tv.y = p.y; tv.z = p.z;
        if (p.x >= -1.f && p.x <= 1.f && p.y >= -1.f && p.y <= 1.f && p.z >= 0.f && p.z <= 1.f) ++dbg_onscreen;
        if (p.y < dbg_ymin) dbg_ymin = p.y;
        if (p.y > dbg_ymax) dbg_ymax = p.y;
        dbg_lastcol[0]=tv.rgba[0]; dbg_lastcol[1]=tv.rgba[1]; dbg_lastcol[2]=tv.rgba[2];
        g_verts.push_back(tv);
    };
    // ngx_build_mesh emits a TRIANGLE LIST (idx in triples). Clip each triangle against the near
    // plane ez <= -kNear (kNear small, just in front of the camera to dodge the w=1/-ez singularity
    // at ez=0); the front frustum (ez<=-realNear) is then handled by Vulkan's depth clip.
    const float kNear = 1.0f;
    for (size_t k = 0; k + 2 < idx.size(); k += 3) {
        const ClipVtx tri[3] = { make_cv(verts[idx[k]]), make_cv(verts[idx[k+1]]), make_cv(verts[idx[k+2]]) };
        ClipVtx poly[6]; int np = 0;
        for (int e = 0; e < 3; ++e) {
            const ClipVtx& A = tri[e]; const ClipVtx& B = tri[(e+1) % 3];
            const bool ai = A.ez <= -kNear, bi = B.ez <= -kNear;
            if (ai) poly[np++] = A;
            if (ai != bi) { const float t = (-kNear - A.ez) / (B.ez - A.ez); poly[np++] = lerp_cv(A, B, t); }
        }
        if (np < 3) continue;
        emit_cv(poly[0]); emit_cv(poly[1]); emit_cv(poly[2]);
        if (np == 4) { emit_cv(poly[0]); emit_cv(poly[2]); emit_cv(poly[3]); }
    }
    const uint32_t vcount = (uint32_t)g_verts.size() - vstart;
    if (dbg_enabled()) {   // per-shape on-screen coverage probe (first 24 shapes = the sky, drawn first)
        static int s_cov = 0;
        if (s_cov < 24) { ++s_cov;
            std::fprintf(stderr, "[cov] shape#%d srcverts=%u emit=%u onscreen=%d ndcY[%.2f..%.2f] lit=%d cc0=%04x col=%.2f,%.2f,%.2f ntex=%zu\n",
                         s_cov, (unsigned)idx.size(), vcount, dbg_onscreen, dbg_ymin, dbg_ymax,
                         (int)do_light, me->chanCtrl[0], dbg_lastcol[0],dbg_lastcol[1],dbg_lastcol[2], me->tex.size()); }
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

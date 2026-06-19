// ngx_j3d_shape — N4 live hook (docs/native_port_plan.md §3): observe the game's
// J3D world-geometry draws and assemble them into native meshes, Dolphin-free.
//
// A tee on J3DShape::draw (0x802e0390) reads the shape's vertex descriptor from
// the J3D OBJECTS (not by decoding a GX byte stream): GXVtxDescList (unk2C) →
// VCD, J3DVertexData::getVtxAttrFmtList (unk44+0xC) → VAT, and the live vertex
// arrays (j3dSys pos/nrm/clr0 override + J3DVertexData for clr1/tex). It then
// walks each J3DShapeDraw display list (mDraws[i]->mDisplayList — the GX
// primitive stream) through ngx_build_mesh, producing native vertices + a
// triangle-index list per shape. The real recompiled draw still runs (Dolphin
// keeps rasterizing during bring-up); we only OBSERVE, and publish stats over the
// probe (/ngxshape) so the native geometry can be verified vs the oracle.
//
// Layout (reference/sms JSystem decomp, verified addresses):
//   J3DShape:     mElementCount@0x6, mGDCommands@0x28, GXVtxDescList* unk2C@0x2C,
//                 bool unk30(NBT)@0x30, J3DShapeDraw** mDraws@0x38,
//                 J3DVertexData* unk44@0x44.
//   J3DShapeDraw: vtable@0, mDisplayListSize@4, const u8* mDisplayList@8 (the
//                 decomp header omits the vtable; confirmed by disasm of the ctor
//                 0x802dfe70 — stw r4(list),8(this) / stw r5(size),4(this) — and
//                 draw 0x802dfe88 — lwz list,8(this) / lwz size,4(this)).
//   J3DVertexData: GXVtxAttrFmtList* @0xC, posArr@0x10, nrmArr@0x14, nbtArr@0x18,
//                  colorArr[2]@0x1C, texCoordArr[8]@0x24.
//   j3dSys @0x804045DC: unk10C(vtxPos)@+0x10C, unk110(vtxNrm)@+0x110,
//                  unk114(vtxClr0)@+0x114  (loadVtxArray's live-buffer override).
//   GXVtxDescList entry = {u32 attr, u32 type} (8 B). GXVtxAttrFmtList entry =
//                  {u32 attr, u32 cnt, u32 type, u8 frac} (16 B). attr 0xFF = end.
//   GXAttr: POS=9 NRM=10 CLR0=11 CLR1=12 TEX0..7=13..20. GXAttrType=VCD class.

#include "../overrides.h"
#include "../intrinsics.h"
#include "../ngx/ngx_mesh.h"
#include "../ngx/ngx_render_data.h"
#include "../ngx/ngx_project.h"   // pure, unit-tested eye→clip→NDC (sunbright-render-test)
#include "../ngx/ngx_clip.h"      // pure, unit-tested near-plane triangle clip
#include "../ngx/ngx_light.h"     // pure, unit-tested GX per-vertex lighting (test_lighting)
#include "../ngx/ngx_imm_geom.h"  // pure, unit-tested immediate-mode GX geometry (GXDrawCube)
#include "../ngx/ngx_indirect.h"  // pure, unit-tested GX indirect-texturing mantissa/decode
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <algorithm>
#include "VideoCommon/XFMemory.h"   // ground-truth GX lighting state (Dolphin XF registers)
#include "VideoCommon/BPMemory.h"   // ground-truth GX pixel state (fog / blend / etc.)
#include "VideoCommon/CPMemory.h"   // ground-truth GX vertex array bases (g_main_cp_state)
#include "../render/tex_decode.h"   // DBG: decode a texture to check its brightness
#include "../render/tev_eval.h"     // shared pure GX TEV combiner evaluator (pixblend/render_test)
#include "../render/tev_shader.h"   // DBG: dump the generated TEV GLSL for a material

namespace {

constexpr u32 J3DSYS = 0x804045DCu;
// Honor the VALUE (=0 disables) so a recomp-GX oracle with NGX_PRESENT=0 truly runs Dolphin GX.
static bool sb_env_on(const char* n) { const char* v = getenv(n); return v && atoi(v) != 0; }
bool g_enabled = sb_env_on("SUNBRIGHT_NGX_SHAPE") || sb_env_on("SUNBRIGHT_NGX_PRESENT");  // present needs capture
// FULL-PC-ENGINE: ngx owns the J3DShape draw (no super-call into the recompiled draw). On when
// explicitly requested, or implied by NO_RECOMP while ngx presents (Dolphin GX output is discarded
// then anyway). See ov_j3dshape_draw for the native per-view j3dSys setup that replaces the draw.
bool g_native_draw = sb_env_on("SUNBRIGHT_NGX_NATIVE_DRAW") ||
                     (sb_env_on("SUNBRIGHT_NO_RECOMP") && sb_env_on("SUNBRIGHT_NGX_PRESENT")) ||
                     sunbright_purejit_mode();   // pure-JIT no-recomp: ngx always owns the draw (no
                                                 // recomp body to super-call; ngx draws natively)

// Run the original GX/J3D function after an ngx capture tee — EXCEPT under the pure-JIT no-recomp
// mode, where ngx OWNS rendering: there is no recomp body to super-call, the GX side effect (XF/BP
// register / FIFO write) is DISCARDED (ngx reads the guest J3D object model and presents its own
// frame), and call_ppc(cpu, cpu.lr) would route the JIT caller's continuation through the
// interpreter (which bypasses every override — forbidden). So under purejit each tee captures and
// returns to lr (the override's C return commits that). These are write-only GX setters; the game
// never reads their effect back for logic, so skipping the original is safe. Each tee'd address is
// marked purejit-safe (below) so it dispatches per-call via Run() rather than firing once at compile.
static void ngx_super(CPUState& cpu, u32 addr) {
    if (sunbright_purejit_mode()) return;   // capture-only; ngx owns the draw
    if (RecompFunc o = recomp_raw(addr)) o(cpu); else call_ppc(cpu, cpu.lr);
}

// Latest GX texmap-0 binding (from the GXLoadTexObj tee), associated with shapes
// drawn after it. Decoded from the GXTexObj's packed registers.
struct CurTex { u32 addr = 0; u16 w = 0, h = 0; u8 fmt = 0; u32 tlut_addr = 0; u8 tlut_fmt = 0; bool valid = false; };
CurTex g_curtex[8];   // live binding per GX texmap (0..7) — GX-tee path (diagnostic only)

// N6.7: the per-material texture binding read OBJECT-MODEL (J3D preloads textures
// into TMEM upfront, so the GXLoadTexObj tee is stale at draw time — the real
// per-shape texture is selected by the material). Filled by capture_textures.
NgxTexBind g_mat_tex[8];

// TLUT registry: tlut_name (GXTlut, the TMEM slot) → palette guest addr + format,
// populated by the GXLoadTlut tee. CI texobjs reference a tlut_name (texobj+0x18).
struct TlutEntry { u32 addr = 0; u8 fmt = 0; bool valid = false; };
TlutEntry g_tlut[256];

inline bool valid(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }
inline u32  r32(u32 a) { return valid(a) ? mem_r32(a) : 0; }
inline u8   rb(u32 a) { return valid(a) ? (u8)(mem_r32(a) >> 24) : 0; }  // big-endian byte @ word
inline float rf(u32 a) { u32 u = r32(a); float f; std::memcpy(&f, &u, 4); return f; }

// Decode a GXTexObj's packed registers (image0@0x08 = (w-1)[0:9]/(h-1)[10:19]/
// fmt&0xF[20:23]; image3@0x0C = (addr>>5)[0:20]; tlutName@0x18 for CI) into a
// CurTex. Shared by the GXLoadTexObj and GXLoadTexObjPreLoaded tees.
void decode_texobj(u32 obj, CurTex& t) {
    const u32 image0 = r32(obj + 0x08), image3 = r32(obj + 0x0C);
    t.w = (u16)((image0 & 0x3FF) + 1);
    t.h = (u16)(((image0 >> 10) & 0x3FF) + 1);
    t.fmt = (u8)((image0 >> 20) & 0xF);
    t.addr = 0x80000000u | ((image3 & 0x1FFFFFu) << 5);
    t.tlut_addr = 0; t.tlut_fmt = 0;
    if (t.fmt == 0x8 || t.fmt == 0x9 || t.fmt == 0xA) {        // CI → resolve TLUT
        const u32 name = r32(obj + 0x18) & 0xFF;               // texobj.tlutName
        if (g_tlut[name].valid) { t.tlut_addr = g_tlut[name].addr; t.tlut_fmt = g_tlut[name].fmt; }
    }
    t.valid = true;
}

// ── N6 color-channel LIGHTING (docs/native_port_plan.md §6) ─────────────────────
// GX rasterizes a lit "channel colour" per vertex (raster colour input to the TEV),
// not the raw CLR0 attribute, whenever a material's colour channel has lighting on
// (SMS world geometry uses a sun + ambient). We port that stage natively:
//   • capture the 8 hardware lights at GXLoadLightObjImm (the state the GPU uses,
//     already transformed into the position-matrix / eye space by the scene), and
//   • read the material's J3DColorBlock (channel control + material/ambient colour
//     registers), then evaluate the GX lighting equation per vertex in eye space
//     (vertex pos via the modelview, normal via j3dSys.mCurrentNormMtx @ +0x108).
// The result replaces the per-vertex raster colour fed to the TEV shader.
//
// GXLightObj packed layout (verified by disasm of GXInitLightPos/Color/Attn/
// SpecularDir): colour u32@0x0C, cosAtten(a0,a1,a2)@0x10/14/18, distAtten(k0,k1,k2)
// @0x1C/20/24, position@0x28/2C/30, direction(half-angle)@0x34/38/3C.
struct LightObj {
    bool  valid = false;
    float color[3] = {0, 0, 0};   // RGB 0..1
    float pos[3]   = {0, 0, 0};    // eye-space position
    float dir[3]   = {0, 0, 0};    // eye-space (half-angle) direction
    float cosA[3]  = {0, 0, 0};    // angle attenuation a0,a1,a2
    float distA[3] = {0, 0, 0};    // distance attenuation k0,k1,k2
};
LightObj g_light[8];
unsigned long g_light_loads = 0;

// The game's EFB copy-clear colour (GXSetCopyClear). The native present must clear the 3D
// target to THIS, not a hardcoded constant — the sky base is a screen-blend (dst=INVSRCCLR)
// layer, so a wrong background washes the whole sky toward grey. Default = black (GX's
// typical 3D clear) until the first GXSetCopyClear is observed.
float g_copy_clear[4] = {0.f, 0.f, 0.f, 1.f};
unsigned long g_copy_clear_sets = 0;
u32 g_copy_clear_arg = 0, g_copy_clear_arg4 = 0, g_copy_clear_deref = 0;

// GX position-matrix memory (64 rows × 4 floats), captured SYNCHRONOUSLY at GXLoadPosMtxImm so
// multi-matrix / skinned shapes (PNMTXIDX) can select the per-vertex matrix. The vertex's
// PNMTXIDX byte is the starting ROW here (GX_PNMTX0=0, _1=3, …); a matrix = rows idx,idx+1,idx+2.
// Captured on the emu thread (no xfmem GPU-thread lag). Holds the LAST packet's matrices — fine
// for single-packet multi-matrix shapes (the common case, incl. the title logo).
float g_posmtx[64][4] = {{0}};
unsigned char g_posmtx_src[64] = {0};   // per-slot load source: 0=none 1=Imm (Dolphin-correct by
                                        // construction) 2=Indx (ngx reconstructs → suspect)
bool  g_cur_pnmtx = false;            // current shape uses PNMTXIDX (set in capture)
// Per-PACKET resolved pos-matrices for the shape currently being captured. A skinned J3DShape draws
// in mElementCount packets, each (a J3DShapeMtxMulti) reloading XF pos slots 0,3,6,… from its OWN
// useMtxIndexTable (unkC). g_posmtx alone holds only the LAST packet → every earlier packet's verts
// transform by the wrong matrix (Mario shredded). capture() resolves each packet's slots here from
// the model draw-matrix buffer (seam base/stride) and transform_eye picks by (vertex.packet, slot).
constexpr int NGX_MAX_PKT = 64, NGX_MAX_SLOT = 11;   // slot = matidx/3 (XF rows 0,3,…,30)
float g_pkt_mtx[NGX_MAX_PKT][NGX_MAX_SLOT][12];
bool  g_pkt_have[NGX_MAX_PKT][NGX_MAX_SLOT];
unsigned g_seam_base = 0, g_seam_stride = 0;   // last Dolphin-correct draw-matrix array base/stride,
                                               // recorded by the LoadIndexedXF seam (ngx_capture_indexed_posmtx)
// Ordered log of the pos-matrix loads the seam resolves during ONE J3DShape::draw super-call. Each
// entry is a slot (XF row = id*3) + the matrix the seam read with ITS OWN per-load base (always
// correct, unlike the global g_seam_base which is the last writer across shapes). Reset before the
// super-call (ov_j3dshape_draw); capture() partitions it by packet (each packet loads its non-0xffff
// useMtxIndexTable entries, in order) → the true per-packet matrices.
struct SeamLoad { unsigned slot; float m[12]; };
SeamLoad g_seam_log[512];
int      g_seam_log_n = 0;
// LIVE skinned-matrix source toggle (/ngxmtxsrc?m=): 0=per-packet object-model (new, correct),
// 1=g_posmtx global last-writer (old), 2=single modelview m (no skinning). Lets me A/B the skinned
// transform on the running game with no rebuild. Counters report how many verts took each path.
int g_ngx_mtxsrc = 0;
// For a NON-skinned (single J3DShapeMtx) shape, the matrix used is drawMtx[unk4] — J3DShapeMtx::load
// does GXLoadPosMtxIndx(unk4, 0), loading draw-matrix unk4 into XF slot 0. ngx must index by unk4,
// NOT drawMtx[0] (mCurrentDrawMtx[0]); else a rigid sub-shape on a non-root joint (Mario's hat on
// the head joint) renders at the root and floats. Set per shape in capture(), used in transform_eye.
unsigned g_single_idx = 0;
unsigned long g_pkt_applied = 0, g_pkt_fallback = 0;   // CUMULATIVE (never reset) so the live
                                                       // /ngxshape read is timing-independent
unsigned long g_posmtx_loads = 0;

// ── Skinned-shred metric (timing-independent NUMBER for "is a skinned model exploding") ──
// A coherent skinned character has SMALL eye-space triangle edges (≲ its real size, a few
// hundred units); a shred scatters verts → an intra-shape edge spanning thousands of units.
// transform_eye measures, per skinned (PNMTXIDX) shape, the max eye-space edge over the
// shape's own triangles and keeps the session MAX (+ which shape/packet/matidx produced it)
// and a histogram of skinned shapes by edge magnitude. Read live via /ngxshape; a shred in
// ANY captured pose shows as g_shred_max spiking, so it survives the 8-frame sampling that
// missed it before. (NOT reset across frames — worst-ever pose is what we hunt.)
float         g_shred_max = 0.f;          // worst eye-space edge over all skinned shapes this run
u32           g_shred_shape = 0;          // the shape that produced it
unsigned      g_shred_pkt = 0;            // packet of the offending vertex
unsigned char g_shred_mi0 = 0, g_shred_mi1 = 0;  // matidx of the two edge endpoints
unsigned long g_shred_n[4] = {0,0,0,0};   // skinned-shape edge buckets: <500, <2k, <10k, ≥10k
float         g_shred_last = 0.f;         // worst edge in the MOST RECENT skinned shape (per-frame-ish)
// SCREEN-space variant: an eye-space-coherent model can still LOOK shredded if the projection/
// near-clip blows triangles apart on screen. Measure max NDC (post-w-divide) edge over a skinned
// shape's FRONT-facing triangles (all three verts w>eps, so near-plane straddlers — handled by the
// clipper — don't count as false shred). A coherent on-screen model keeps NDC edges ≲ ~2 (the whole
// viewport is [-1,1]); a real screen shred spans many viewport-widths.
float         g_shred_ndc_max = 0.f;      // worst NDC edge over all skinned shapes this run
u32           g_shred_ndc_shape = 0;
unsigned long g_shred_ndc_n[4] = {0,0,0,0};  // skinned-shape NDC-edge buckets: <2, <8, <40, ≥40
// Diagnostic snapshot of the offending vertex when a new NDC max is recorded.
float         g_shred_ndc_w = 0.f, g_shred_ndc_eye[3] = {0,0,0};
unsigned char g_shred_ndc_mi = 0, g_shred_ndc_pkt = 0;
bool          g_shred_ndc_usepkt = false;
float         g_shred_ndc_pos[3] = {0,0,0};   // model-space pos of the offending vertex
float         g_shred_ndc_M[12] = {0};        // the pos matrix applied to it
float         g_shred_ndc_ebb[6] = {0};       // eye-space bbox of the offending shape: xmin,xmax,ymin,ymax,zmin,zmax
unsigned      g_shred_ndc_nv = 0;             // vert count of the offending shape
int           g_shred_ndc_projtype = -1;      // g_proj_type at capture (0=persp,>0=ortho)
float         g_shred_ndc_P[16] = {0};        // the projection ngx applied to it

// ALL-SHAPE POST-CLIP NDC-spread shred metric (not gated on g_cur_pnmtx) — catches single-matrix
// shred like the title logo. NDC edge over the EMITTED (clipped) triangles of EVERY shape, split
// by projection type, so I can tell whether the visible shred is perspective or ortho geometry
// and which material (cc/tex0) is exploding.
float         g_shred_all_max = 0.f; u32 g_shred_all_shape = 0; int g_shred_all_projtype = -1;
bool          g_shred_all_pnmtx = false; u16 g_shred_all_cc = 0xFFFF; u32 g_shred_all_tex0 = 0;
float         g_shred_all_ndc[6] = {0}, g_shred_all_triw[3] = {0};   // the offending emitted tri
unsigned long g_shred_all_n[4] = {0,0,0,0};            // <2,<8,<40,≥40 over ALL emitted tris
unsigned long g_shred_all_npersp = 0, g_shred_all_northo = 0;   // worst-edge≥40 count by projtype
// POST-CLIP shred: NDC edge over the geometry ACTUALLY EMITTED to the GPU (after ngx_clip_near_tri
// + Vulkan will scissor). This is the TRUE visible shred — the pre-clip metric over-reports verts
// the clipper removes. If post>>0 the emitted triangles really do span the screen = visible spikes.
float         g_shred_post_max = 0.f;
u32           g_shred_post_shape = 0;
unsigned long g_shred_post_n[4] = {0,0,0,0};  // <2, <8, <40, ≥40
// Auto-freeze-on-shred: SUNBRIGHT_NGX_SHREDFREEZE=<ndc-thresh> latches the snapshot (ngx+GX
// oracle) on the FIRST frame a skinned NDC edge exceeds <thresh>, so /abshot2 captures the
// actual spike frame for an A/B with the oracle (transient shred → can't catch by hand).
const float   g_shred_freeze_thresh = []{ const char* v=getenv("SUNBRIGHT_NGX_SHREDFREEZE"); return v?(float)atof(v):0.f; }();
std::atomic<bool> g_shred_pending_freeze{false};

// ── ngx geometry differential vs Dolphin xfmem (SUNBRIGHT_NGX_DIFF) ───────────
// ⚠ RECORDED DEAD-END (2026-06-16): this compares ngx's g_posmtx against Dolphin's
// xfmem.posMatrices, but the POST-LOAD synchrony validator below PROVED xfmem is NOT a
// valid CPU-side oracle in this hybrid: GXLoadPosMtxImm matrices come straight from the
// call ARGS (= exactly what Dolphin loads), yet 77% of them disagree with xfmem read
// IMMEDIATELY after the real load runs. Dolphin's GP updates xfmem ASYNCHRONOUSLY, so
// every CPU-side read of xfmem lags the actual load. ⇒ Do NOT trust this differential's
// "diverges from Dolphin" verdicts as ground truth; the only trustworthy Dolphin oracle
// in the hybrid is RENDERED PIXELS (the reason tex_decode_selftest works is it calls
// Dolphin's decoder DIRECTLY, no async GP between).
// What DID survive: (a) the synchrony validator (g_pl_imm_*) that proved the above, and
// (b) the indexed reconstruction (Indx) produces ABSURD matrices (scale ~566, translation
// ~187802 — garbage independent of any oracle), so ngx's base+index*stride indexed read
// is reading wrong memory. Kept for those two signals; not as a pass/fail gate.
bool g_ngxdiff = sb_env_on("SUNBRIGHT_NGX_DIFF");
struct GeomDiffSlot { unsigned long n=0, nmiss=0; float maxd=0; unsigned char src=0; float ngx[12]={0}, dol[12]={0}; };
GeomDiffSlot g_gdiff[64];
unsigned long g_gdiff_mm_shapes=0, g_gdiff_frames=0;
// Post-load differential: read xfmem RIGHT AFTER the real GXLoadPosMtx* runs (no tee staleness).
//  Imm:  g_posmtx (from call ARGS) vs xfmem — MUST match if the load reached xfmem synchronously.
//        This is the meta-validator: Imm div=0 ⇒ xfmem is a usable CPU-side oracle; Imm div>0 ⇒
//        xfmem is NOT synchronous at the hook → it can't be the oracle at all (need Dolphin pixels).
//  Indx: g_posmtx (ngx's base+index*stride RECONSTRUCTION) vs xfmem (Dolphin's actual load) —
//        if it diverges while Imm is clean, ngx reads the WRONG source: the indexed bug, pinned.
unsigned long g_pl_imm_n=0, g_pl_imm_div=0;  float g_pl_imm_maxd=0;
unsigned long g_pl_indx_n=0, g_pl_indx_div=0; float g_pl_indx_maxd=0;
float g_pl_indx_ngx[12]={0}, g_pl_indx_dol[12]={0};
// Compare g_posmtx[slot] (just captured) vs xfmem.posMatrices[slot] (what the real load just wrote).
inline void ngx_postload_cmp(unsigned slot, bool is_imm) {
    if (!g_ngxdiff || slot + 2 >= 64) return;
    float md = 0, ng[12], dl[12];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) {
        const float a = g_posmtx[slot + r][c], b = xfmem.posMatrices[(slot + r) * 4 + c];
        ng[r*4+c] = a; dl[r*4+c] = b;
        float d = a - b; if (d < 0) d = -d; if (d > md) md = d;
    }
    if (is_imm) { g_pl_imm_n++; if (md > 1e-3f) g_pl_imm_div++; if (md > g_pl_imm_maxd) g_pl_imm_maxd = md; }
    else { g_pl_indx_n++; if (md > 1e-3f) g_pl_indx_div++;
           if (md > g_pl_indx_maxd) { g_pl_indx_maxd = md; for (int i=0;i<12;i++){g_pl_indx_ngx[i]=ng[i];g_pl_indx_dol[i]=dl[i];} } }
}

// Global hardware ambient colour register per colour channel (0,1), captured at
// GXSetChanAmbColor. J3DColorBlockLightOff blocks do NOT store/load an ambient
// (only LightOn does) — for them the ambient is whatever the scene set globally,
// so we must read the hardware register, not the block.
u8 g_amb_reg[2][4] = {{0,0,0,0}, {0,0,0,0}};
bool g_amb_have[2] = {false, false};
unsigned long g_amb_sets = 0;
// Diagnostic: use Dolphin's AUTHORITATIVE ambient (xfmem, captured GPU-thread) in light_vertex to
// confirm the black-materials bug is the ambient. SUNBRIGHT_NGX_DOLAMB=1.
extern "C" void sb_get_gx_ambient(unsigned* amb0, unsigned* mat0, unsigned long* count);
bool g_use_dolamb = sb_env_on("SUNBRIGHT_NGX_DOLAMB");
// J3D programs the ambient via J3DGDSetChanAmbColor (a GD/XF-direct write), NOT GXSetChanAmbColor
// — so the GX-function tee misses it. Capture the GD path too (this is the value the GPU actually
// uses). g_gd_amb is draw-order-current at capture_colorchan time.
u8 g_gd_amb[2][4] = {{0,0,0,0}, {0,0,0,0}};
bool g_gd_amb_have[2] = {false, false};
unsigned long g_gd_amb_sets = 0; u8 g_gd_amb_last[4] = {0,0,0,0};
// Persistent global XF ambient register (per colour channel). GX's ambient register is global
// hardware state: a J3DColorBlockLightOn material loads its block ambient INTO this register, and
// every later material with ambSource=REG (incl. LightOff blocks, which carry NO ambient field of
// their own) reads it back — last-writer-wins. The previous code read each material's OWN block
// ambient (0 for LightOff) and defaulted REG-source ambients to 0, dropping the scene's ambient
// floor (the "everything too dark" class). Now: writers = LightOn block load, GXSetChanAmbColor,
// J3DGDSetChanAmbColor; readers = any ambSrc=REG material. A small value histogram lets the probe
// confirm the live register matches Dolphin's xfmem ground truth without tapping xfmem at runtime.
u8 g_xf_amb[2][4] = {{0,0,0,0}, {0,0,0,0}};
bool g_xf_amb_have[2] = {false, false};
unsigned long g_xf_amb_writes = 0;
u32 g_xf_amb_hist_key[8] = {0}; unsigned long g_xf_amb_hist_cnt[8] = {0};
inline void xf_amb_write(int idx, u8 r, u8 g, u8 b, u8 a) {
    if (idx < 0 || idx > 1) return;
    g_xf_amb[idx][0]=r; g_xf_amb[idx][1]=g; g_xf_amb[idx][2]=b; g_xf_amb[idx][3]=a;
    g_xf_amb_have[idx]=true; g_xf_amb_writes++;
    if (idx == 0) { const u32 key = ((u32)r<<24)|((u32)g<<16)|((u32)b<<8)|a;
        for (int i=0;i<8;i++){ if(g_xf_amb_hist_key[i]==key){g_xf_amb_hist_cnt[i]++;break;}
                               if(g_xf_amb_hist_cnt[i]==0){g_xf_amb_hist_key[i]=key;g_xf_amb_hist_cnt[i]=1;break;} } }
}
// Authoritative per-channel lighting state captured SYNCHRONOUSLY from the GX commands
// (GXSetChanCtrl / GXSetChanMatColor) — the same LitChannel.hex / matColor Dolphin uses.
u16  g_gx_cc[2] = {0, 0};            bool g_gx_cc_have[2] = {false, false};
u8   g_gx_matcol[2][4] = {{255,255,255,255},{255,255,255,255}}; bool g_gx_matcol_have[2] = {false,false};
unsigned long g_gx_cc_sets[2] = {0,0}, g_gx_matcol_sets[2] = {0,0};   // call counters (per-draw liveness)

// TexGen (UV-generation) state, defined here so /gxstate's GxStateRec can carry it. The
// authoritative copy + capture is below (capture_texgen / g_cur_texgen).
struct TexGen {
    u8   type = 1;          // GXTexGenType: 0=MTX3x4, 1=MTX2x4
    u8   src  = 4;          // GXTexGenSrc: 0=POS,1=NRM,4..11=TEX0..7,19=COLOR0
    bool has_mtx = false;   // a non-identity texgen matrix is set
    float m[12] = {0};      // mTotalMtx 3x4 (row-major) when has_mtx
    u8   mtxsel = 60;       // raw GXTexMtx selector (30=TEXMTX0,step 3; 60=IDENTITY) → XF row
    u32  mtxptr = 0;        // the J3DTexMtx* ngx read (blk+0x28+idx*4) — match the load tee
    // LIVE XF-row texmtx (what GX's GPU ACTUALLY uses = last J3DGDLoadTexMtxImm into this row).
    // Differs from m[]/mtxptr when the material does NOT reload its texmtx per draw (J3D skips
    // a redundant load) → GX renders with whatever earlier material last wrote the XF row.
    bool  live_have = false;  float live_ss=0, live_st=0, live_ts=0, live_tt=0;
    u32   live_self = 0;      unsigned long live_seq = 0;  // src J3DTexMtx + load sequence #
    float obj_m[12] = {0};    // material's OWN mTotalMtx (diagnostic; m[] is the render matrix)
};
struct TexGenSet { u8 num = 0; TexGen tg[8]; };

// ── /gxstate: GX-command-stream vs ngx-object-model render-state diff ───────────────
// ngx reconstructs the per-material render state from the J3D OBJECT MODEL (the color/tev/PE
// blocks read straight from guest RAM). The GROUND TRUTH for what the GPU actually got is the
// GX COMMAND STREAM the game issues — captured SYNCHRONOUSLY at the GX function tees
// (GXSetChanCtrl/MatColor/AmbColor, GXLoadLightObjImm). They SHOULD agree; where they don't,
// ngx's reconstruction is wrong at that exact pipeline stage. This snapshots BOTH at a target
// material's draw so "at which step does the rendering differ" is a deterministic field-level
// answer (NOT a pixel comparison). Target chosen by tev_index (live /gxstate?ti=N, or env).
struct GxStateRec {
    bool have = false; int ti = -1; u32 sh = 0;
    // colour-channel control / lighting stage
    u16 obj_cc = 0, obj_ca = 0;            // ngx: block-parsed COLOR0 / ALPHA0 channel control
    u16 gx_cc = 0; bool gx_cc_have = false; unsigned long gx_cc_sets = 0;  // GX cmd: GXSetChanCtrl-packed COLOR0
    u8  obj_mat[4] = {0}, gx_mat[4] = {0}; bool gx_mat_have = false; unsigned long gx_mat_sets = 0;
    u8  obj_amb[4] = {0}, gx_amb[4] = {0}; bool gx_amb_have = false; unsigned long gx_amb_sets = 0;
    u8  gd_amb[4] = {0}; bool gd_amb_have = false; unsigned long gd_amb_sets = 0;  // J3DGDSetChanAmbColor (J3D's path)
    // xfmem: Dolphin's GPU-decoded XF channel state (authoritative in the ORACLE run; lags in
    // the ngx-present run because the GP is async — read in both and cross-check).
    u32 xf_cc = 0, xf_mat = 0, xf_amb = 0; bool xf_have = false;
    // resolved active light state (from the obj_cc mask), captured live at this draw
    u8  obj_mask = 0, gx_mask = 0;
    // raw color-block bytes ngx parses (to prove the offset/value it reads)
    u32 cb_addr = 0, cb_vt = 0; u8 cb_raw[0x20] = {0};
    // TEV combiner + PE (blend/alpha-test) — the COVERAGE/color stages downstream of lighting
    NgxTevState tev{}; bool tev_have = false;
    // TexGen (UV generation) + bound textures — for diagnosing tiling/checkerboard sampling
    TexGenSet tg{}; NgxTexBind tex[8]{};
    // tc0/tc1 VCD class (0=ABSENT) + array base + type/frac (verify 2nd-octave TEX1 decode)
    u8 tc0cls=0, tc1cls=0, tc0type=0, tc1type=0, tc0frac=0, tc1frac=0; u32 tc0base=0, tc1base=0;
    float tg_xf[8][8] = {{0}}; bool tg_xf_have[8] = {false};   // xfmem texmtx (row0[4]+row1[4]) per coord, at draw
    struct LRec { bool valid=false; float col[3]={0}; float pos[3]={0}; float dir[3]={0};
                  float cosA[3]={0}; float distA[3]={0}; } lights[8];
};
std::atomic<int> g_gxstate_ti{ []{ const char* v=getenv("SUNBRIGHT_NGX_GXSTATE"); return v?atoi(v):-1; }() };
GxStateRec g_gxstate, g_gxstate_pub;
extern "C" void sb_ngx_set_gxstate_ti(int t) { g_gxstate_ti.store(t); }
// Per-vertex UV capture for the gxstate target ti (filled in transform_eye): first vertex's raw
// texcoords + computed (texgen'd) UVs + the UV bbox over the shape's verts. Pins texgen/UV-tiling
// divergence (the file-select cloud over-tiling). Published with g_gxstate.
struct UvDbg { bool have=false; float tex0[2]={0},tex1[2]={0},uv0[2]={0},uv1[2]={0};
    float u0min[2]={1e9f,1e9f},u0max[2]={-1e9f,-1e9f},u1min[2]={1e9f,1e9f},u1max[2]={-1e9f,-1e9f}; unsigned long nv=0;
    // per-vertex CLR0 (RASC/RASA source) range across the shape's verts — pins whether the
    // additive cloud's alpha (RASA, matsrc=VTX) is a near-zero gradient (GX subtle) or uniform.
    float ca_min=1e9f, ca_max=-1e9f; float cr_min=1e9f, cr_max=-1e9f; };
UvDbg g_uvdbg, g_uvdbg_pub;
// Per-vertex dump for the gxstate target ti (NDC x,y + RASA alpha + tc0/tc1 UV). Pins whether
// the additive cloud's coverage is gated by geometry (NDC spread), per-vertex alpha, or noise UV.
struct VtxRec { float ndcx, ndcy, a, u0, v0, u1, v1; float ex, ey, ez, cw; float cx, cy, cz; };
std::vector<VtxRec> g_vtxdump, g_vtxdump_pub;
unsigned char g_vtxdump_projtype = 0, g_vtxdump_projtype_pub = 0;   // 0=persp 1+=ortho (g_proj_type at the shape)
float g_vtxdump_mv[12] = {0}, g_vtxdump_mv_pub[12] = {0};           // the shape's modelview
// DBG histograms for the brightness/wash investigation (vert-weighted).
unsigned long g_clr0cls_hist[4] = {0}, g_matsrc_hist[3] = {0}, g_litcfg_hist[2] = {0};
size_t g_bigmap_verts = 0; unsigned g_bigmap_clr0cls = 0; bool g_bigmap_matvtx = false, g_bigmap_en = false;
u8 g_bigmap_vcol[3] = {0}; u16 g_bigmap_cc = 0;
double g_colcat_sum[5] = {0}; unsigned long g_colcat_n[5] = {0};  // col0 lum by category
double g_uplit_sum=0, g_uplit_max=0, g_uplit_amb=0, g_uplit_ndl=0; unsigned long g_uplit_n=0;
unsigned long g_clr0fmt_hist[8] = {0};  // CLR0 VAT format (0=565,1=888,2=888x,3=4444,4=6666,5=8888)
size_t g_bigany_verts = 0; u16 g_bigany_cc = 0; bool g_bigany_hasnrm=false, g_bigany_matvtx=false, g_bigany_en=false;
unsigned g_bigany_clr0cls=0; u8 g_bigany_vcol[3]={0}; double g_bigany_vcolmean=0; u32 g_bigany_clr0base=0;
unsigned long g_pnmtx_shapes=0, g_pnmtx_verts=0; unsigned g_pnmtx_maxnelem=0, g_max_nelem=0;
bool g_bigany_pnmtx=false; u32 g_bigany_vcd=0, g_bigany_posbase=0, g_bigany_posstride=0;
unsigned long g_pnmtx_applied=0, g_pnmtx_nz=0;
float g_bigany_pmin[3]={0}, g_bigany_pmax[3]={0}, g_bigany_pos0[3]={0};

// J3DColorBlock variants (vtable ptrs, gmse01; from the block ctor/dtor disasm:
// LightOn sets 0x803E0CD4, LightOff sets 0x803E0D38). Field offsets from
// J3DColorBlocks.hpp — LightOn: mMatColor@0x04, mAmbColor@0x0C, mColorChan@0x16;
// LightOff: mMatColor@0x04, mColorChan@0x0E (no ambient / no lights).
constexpr u32 VT_CLON = 0x803E0CD4u, VT_CLOF = 0x803E0D38u;

// The current material's colour-channel state (set by capture_colorchan, consumed
// by light_vertex during transform_eye for the shape being captured).
struct ChanInfo {
    bool valid = false;
    u16  color0 = 0, alpha0 = 0;          // J3DColorChan COLOR0 / ALPHA0 ctrl regs
    u16  color1 = 0, alpha1 = 0;          // J3DColorChan COLOR1 / ALPHA1 ctrl regs (mColorChan[2..3])
    u8   matColor[4] = {255, 255, 255, 255};
    u8   ambColor[4] = {0, 0, 0, 0};
    u8   matColor1[4] = {255, 255, 255, 255};   // mMatColor[1] (COLOR1's material colour)
    u8   ambColor1[4] = {0, 0, 0, 0};           // mAmbColor[1] (COLOR1's ambient; CLON only, else 0)
    u8   cullMode = 0;                     // GXCullMode (color block mCullMode): NONE/FRONT/BACK/ALL
};
ChanInfo g_cur_chan;
unsigned long g_chan_lit = 0, g_chan_flat = 0;
// COLOR1 liveness: count verts where the computed COLOR1A1 raster differs meaningfully from
// COLOR0 (proves the 2nd channel is genuinely distinct, not an inert col1==col0 alias).
unsigned long g_col1_verts = 0, g_col1_diff_verts = 0;
u8 g_dbg_cb_raw[0x44] = {0}; bool g_dbg_cb_have = false; u32 g_dbg_cb_vt = 0;
u32 g_cbvt_key[8] = {0}; unsigned g_cbvt_cnt[8] = {0};   // colour-block vtable histogram
// Per-distinct-vtable raw block bytes (for RE'ing the real J3DColorBlock layout: where the true
// matColor=ffffff / ambColor / chan-ctrl live, vs the pointer-garbage the old offsets read).
u8 g_cbvt_raw[8][0x48] = {{0}}; u32 g_cbvt_blkaddr[8] = {0};
u32 g_cbvt_matpkt[8] = {0}; u8 g_cbvt_pktraw[8][0x40] = {{0}};   // J3DMatPacket bytes (to find its DL)
// Flat-path breakdown + brightness diagnostics (per-vertex sums; /ngxshape).
unsigned long g_flat_reg = 0, g_flat_vtx = 0;
double g_lit_lum_sum = 0, g_flat_lum_sum = 0;
double g_diff_sum = 0; unsigned long g_diff_n = 0;   // mean diffuse dot over lit verts/lights
// Last lit material's decoded state (snapshot for inspection).
u16 g_dbg_cc = 0; u8 g_dbg_mat[4] = {0}, g_dbg_amb[4] = {0};
float g_dbg_illum[3] = {0}, g_dbg_out[4] = {0};
double g_nrm_len_sum = 0; unsigned long g_nrm_n = 0;   // mean eye-normal length (sanity)
double g_lit_nrm_sum = 0; unsigned long g_lit_nrm_n = 0, g_lit_nrm_zero = 0;  // lit-vert normals
// Light-0 (sun) diffuse alignment probe: dot(en, ldir0) stats + a last sample.
double g_sun_ndl_sum = 0; unsigned long g_sun_ndl_n = 0, g_sun_ndl_pos = 0;
float g_sun_ndl_max = -2.f, g_dbg_en[3] = {0}, g_dbg_ld0[3] = {0};

// ── N6.6 GX texgen (per-texcoord UV generation) ─────────────────────────────────
// A TEV stage references a GX texcoord (0..7); each texcoord is GENERATED by GX from
// a source (vertex tex attr / position / normal) times a texgen matrix. We port that
// natively: read the material's J3DTexGenBlockBasic (mTexCoord[8] @ blk+0x8, each 4 B:
// type@+0 src@+1 mtx@+2; mTexMtx*[8] @ blk+0x28), and for each texcoord compute
// uv = TexMtx · input per vertex in the producer. The matrix is the LIVE computed
// J3DTexMtx::mTotalMtx @ ptr+0x64 (the matrix J3DTexMtx::load feeds GXLoadTexMtxImm),
// Mtx 3x4 row-major. Input = (s,t,1,1) for TEX src, (x,y,z,1) for POS/NRM. This works
// for both J3D SRT conventions (translation in matrix col2 OR col3, the other = 0).
TexGenSet g_cur_texgen;   // (TexGen/TexGenSet defined above, near GxStateRec)

// LIVE XF-row texmtx state — the matrix the GPU ACTUALLY has loaded in each GX_TEXMTX row,
// tracked by teeing the GD load J3D issues. GX state leaks across draws: a material whose
// texmtx is unchanged is NOT reloaded (J3DTexMtx::load skips it), so the XF row keeps an
// earlier material's matrix. ngx, reading each material's OWN mTotalMtx, then diverges.
struct XfTm { bool have=false; float m[12]={0}; u32 self=0; unsigned long seq=0; };
XfTm g_xfrow_tm[64];
unsigned long g_xftm_seq = 0;
// J3DGDLoadTexMtxImm(Mtx mtx, u32 id, GXTexMtxType type) @ 0x802f2d00. r3=mtx(3x4 row-major),
// r4=id (GX_TEXMTX0=30, step 3 → XF row), r5=type. SYNCHRONOUS — runs on the game thread in
// draw order, so g_xfrow_tm reflects exactly what GX has when the next shape draws.
SUNBRIGHT_OVERRIDE(ov_j3dgd_loadtexmtx, 0x802f2d00u) {
    if (g_enabled) {
        const u32 mp = cpu.gpr[3], id = cpu.gpr[4];
        if (id < 64 && valid(mp)) {
            XfTm& x = g_xfrow_tm[id];
            x.have = true; x.self = mp;        // mp = &mTotalMtx (= J3DTexMtx self+0x64)
            for (int k = 0; k < 12; k++) x.m[k] = rf(mp + k * 4);
            x.seq = ++g_xftm_seq;
        }
    }
    ngx_super(cpu, 0x802f2d00u);
}

// ── Delfino plaza POLLUTION darkening capture (TModelWaterManager::drawShineShadowVolume) ──────────
// The plaza pollution darkening (map 1 only; recedes as cleanliness flag 0x40000 rises). The guest
// draws concentric sphere volumes into the EFB alpha channel then blends a dark-blue colour masked by
// it — a darkening ngx never rendered (THE Delfino "wash"; debug_journal/2026-06-18_delfino_wash_
// ROOT_CAUSE_shineshadowvolume.md). We TEE the function to capture its live params + view matrix, then
// ngx_present reproduces it natively (sphere-volume pass into the present colour-alpha+depth target).
// Tee fires per-call under purejit (marked safe below) only when ngx capture is on; the GX body is
// discarded (ngx owns rendering). Sphere center (model) = (0,3600,-7458); eye center = viewMtx*center.
extern float g_proj[16]; extern bool g_have_proj;   // defined below (~line 1200); used by sb_ngx_get_proj
struct PollutionParams {
    bool  active = false;
    float center[3] = {0,0,0};   // EYE-space sphere centre (viewMtx * world (0,3600,-7458))
    float radius = 0;            // outer sphere radius (unk5E0C + unk5E40)
    float color[3] = {0,0,0};    // dark tint (unk5E47/48/49) 0..1
    float clearAlpha = 0;        // unk5E45 / 255  (outside-dome alpha => darken weight = 1-clearAlpha)
};
PollutionParams g_pollution, g_pollution_pub;
SUNBRIGHT_OVERRIDE(ov_shineshadowvol, 0x8027c67cu) {
    if (getenv("SUNBRIGHT_DBG_POLL")) {
        static unsigned long c = 0;
        if ((c++ % 60) == 0) fprintf(stderr, "[poll-tee] drawShineShadowVolume fired #%lu g_enabled=%d\n", c, (int)g_enabled);
    }
    if (g_enabled) {
        const u32 self = cpu.gpr[3], vm = cpu.gpr[4];      // this (TModelWaterManager), param_1 (viewMtx)
        if (valid(self) && valid(vm)) {
            const float rad  = rf(self + 0x5E0C);          // unk5E0C base radius (grows w/ cleanliness)
            const float span = rf(self + 0x5E40);          // unk5E40 radius span
            // viewMtx is a 3x4 row-major Mtx; eye = vm * (0,3600,-7458,1).
            float m[12]; for (int k = 0; k < 12; k++) m[k] = rf(vm + k * 4);
            const float wx = 0.0f, wy = 3600.0f, wz = -7458.0f;
            g_pollution.center[0] = m[0]*wx + m[1]*wy + m[2] *wz + m[3];
            g_pollution.center[1] = m[4]*wx + m[5]*wy + m[6] *wz + m[7];
            g_pollution.center[2] = m[8]*wx + m[9]*wy + m[10]*wz + m[11];
            g_pollution.radius     = rad + span;
            // bytes: r32 is big-endian word. self+0x5E44 = [5E44 5E45 5E46 5E47], +0x5E48 = [5E48 5E49 ..].
            const u32 w0 = r32(self + 0x5E44), w1 = r32(self + 0x5E48);
            g_pollution.clearAlpha = ((w0 >> 16) & 0xFF) / 255.f;   // unk5E45
            g_pollution.color[0]   = ( w0        & 0xFF) / 255.f;   // unk5E47
            g_pollution.color[1]   = ((w1 >> 24) & 0xFF) / 255.f;   // unk5E48
            g_pollution.color[2]   = ((w1 >> 16) & 0xFF) / 255.f;   // unk5E49
            g_pollution.active     = true;
        }
    }
    ngx_super(cpu, 0x8027c67cu);
}
// Accessors for ngx_present (the native pollution pass) — published frame state + the projection.
extern "C" int sb_ngx_get_pollution(float center[3], float* radius, float color[3], float* clearAlpha) {
    const PollutionParams& p = g_pollution_pub;
    if (!p.active) return 0;
    for (int k = 0; k < 3; k++) { center[k] = p.center[k]; color[k] = p.color[k]; }
    *radius = p.radius; *clearAlpha = p.clearAlpha;
    return 1;
}
extern "C" int sb_ngx_get_proj(float out[16]) {
    if (!g_have_proj) return 0;
    for (int i = 0; i < 16; i++) out[i] = g_proj[i];
    return 1;
}

// Diagnostic histograms (kept).
unsigned long g_tg_src_hist[24] = {0}, g_tg_type_hist[12] = {0};
unsigned long g_tg_mtx_id = 0, g_tg_mtx_set = 0, g_tg_n = 0;

void capture_texgen(u32 material) {
    g_cur_texgen = TexGenSet{};
    const u32 blk = r32(material + 0x24);   // J3DMaterial::mTexGenBlock
    if (!valid(blk)) return;
    const u8* B = sb_ram_fast(blk);
    if (!B) return;
    u32 num = r32(blk + 0x04); if (num > 8) num = 8;   // mTexGenNum @ blk+0x4
    g_cur_texgen.num = (u8)num;
    for (u32 i = 0; i < num; i++) {
        const u8 type = B[0x08 + i * 4], src = B[0x09 + i * 4], mtx = B[0x0A + i * 4];
        TexGen& g = g_cur_texgen.tg[i];
        g.type = type; g.src = src; g.mtxsel = mtx;
        // Matrix selector: GX_TEXMTX0=30, step 3; GX_IDENTITY=60 → none.
        if (mtx >= 30 && mtx < 60) {
            const u32 idx = (u32)(mtx - 30) / 3;           // → mTexMtx[] slot
            const u32 mp = idx < 8 ? r32(blk + 0x28 + idx * 4) : 0;   // J3DTexMtx*
            g.mtxptr = mp;
            // Render with the material's OWN mTotalMtx. J3D bakes this texmtx into the material
            // DISPLAY LIST (J3DGDLoadTexMtxImm → J3DGDWriteXFCmdHdr writes the GD buffer, NOT
            // immediate GX) which is replayed via the FIFO every frame, and animated materials
            // re-patch it to the current mTotalMtx each frame (J3DTexGenBlockBasic::patch). So
            // mTotalMtx@+0x64 IS what the GPU uses. (Do NOT use the J3DGDLoadTexMtxImm tee's
            // "live XF row" below — it only sees DIRECT calls, missing the per-frame DL replays,
            // and xfmem lags the FIFO: both falsely showed identity for the cloud = the xfmem-lag
            // trap. The cloud genuinely renders with scale-2, matching GX.)
            if (valid(mp)) { for (int k = 0; k < 12; k++) { g.m[k] = rf(mp + 0x64 + k * 4); g.obj_m[k] = g.m[k]; } g.has_mtx = true; }
            // Diagnostic only: what DIRECT J3DGDLoadTexMtxImm calls last wrote this XF row (NOT
            // authoritative — DL replays bypass it). The **LEAK** flag here is a tee artifact.
            if (mtx < 64 && g_xfrow_tm[mtx].have) { const XfTm& x = g_xfrow_tm[mtx];
                g.live_have = true; g.live_self = x.self; g.live_seq = x.seq;
                g.live_ss=x.m[0]; g.live_st=x.m[5]; g.live_ts=x.m[3]; g.live_tt=x.m[7]; }
        }
        // diagnostics
        if (src < 24) g_tg_src_hist[src]++;
        if (type < 12) g_tg_type_hist[type]++;
        if (mtx == 60) g_tg_mtx_id++; else g_tg_mtx_set++;
        g_tg_n++;
    }
}

// Compute the texgen'd UV for one texcoord of one vertex (the GX per-vertex texgen).
// litcol0 = the LIT colour-channel result for this vertex (0..1 RGBA) — needed for the
// GX_TG_SRTG / GX_TG_COLOR0/1 sources (src 19/20), where the texcoord IS the rasterized
// colour (a ramp/toon-shade technique: e.g. the file-select sky indexes an I4 ramp by the
// lit intensity). Without this the I4 ramp samples a stale texel → stage saturates → wash.
inline void texgen_uv(const TexGen& g, const NgxVertex& v, const float litcol0[4], float out[2]) {
    // GX_TG_SRTG (type 10): texcoord = the colour channel selected by src (COLOR0=19,
    // COLOR1=20). ngx uses col1==col0, so both map to the lit col0. No texgen matrix.
    if (g.type == 10 || g.src == 19 || g.src == 20) {
        out[0] = litcol0 ? litcol0[0] : 0.f;
        out[1] = litcol0 ? litcol0[1] : 0.f;
        return;
    }
    float in4[4];
    if (g.src >= 4 && g.src <= 11) { const int t = g.src - 4; in4[0]=v.tex[t][0]; in4[1]=v.tex[t][1]; in4[2]=1.f; in4[3]=1.f; }
    else if (g.src == 0)           { in4[0]=v.pos[0]; in4[1]=v.pos[1]; in4[2]=v.pos[2]; in4[3]=1.f; }  // POS
    else if (g.src == 1)           { in4[0]=v.nrm[0]; in4[1]=v.nrm[1]; in4[2]=v.nrm[2]; in4[3]=1.f; }  // NRM
    else                           { in4[0]=v.tex[0][0]; in4[1]=v.tex[0][1]; in4[2]=1.f; in4[3]=1.f; } // unknown → tex0 fallback
    if (!g.has_mtx) { out[0]=in4[0]; out[1]=in4[1]; return; }   // identity passthrough
    const float* m = g.m;
    float s = m[0]*in4[0]+m[1]*in4[1]+m[2]*in4[2]+m[3]*in4[3];
    float t = m[4]*in4[0]+m[5]*in4[1]+m[6]*in4[2]+m[7]*in4[3];
    if (g.type == 0) {                                          // MTX3x4 → divide by q
        const float q = m[8]*in4[0]+m[9]*in4[1]+m[10]*in4[2]+m[11]*in4[3];
        if (q > 1e-6f || q < -1e-6f) { s /= q; t /= q; }
    }
    out[0]=s; out[1]=t;
}

// Read the material's J3DColorBlock into g_cur_chan (best-effort; valid=false on a
// missing/unknown block → the consumer falls back to the raw vertex colour).
u32 g_cur_cb_addr = 0, g_cur_cb_vt = 0; u8 g_cur_cb_raw[0x20] = {0};   // for /gxstate raw-block dump
u32 g_cur_material = 0;   // the J3DMaterial whose block we captured (for /shapeat fresh re-read)
u32 g_cur_matpkt = 0;     // the live J3DMatPacket (its baked DL @+0x20/len@+0x24 = what GX replays)
void capture_colorchan(u32 material) {
    g_cur_chan = ChanInfo{};
    g_cur_cb_addr = 0;
    g_cur_material = material;
    const u32 cb = r32(material + 0x20);   // J3DMaterial::mColorBlock
    if (!valid(cb)) return;
    g_cur_cb_addr = cb;
    const u32 vt = r32(cb + 0x00);
    const u8* B = sb_ram_fast(cb);
    if (!B) return;
    g_cur_cb_vt = vt; for (int k = 0; k < 0x20; k++) g_cur_cb_raw[k] = B[k];
    for (int i = 0; i < 8; i++) {           // colour-block vtable histogram + per-vtable raw capture
        if (g_cbvt_key[i] == vt) { g_cbvt_cnt[i]++; break; }
        if (g_cbvt_key[i] == 0) { g_cbvt_key[i] = vt; g_cbvt_cnt[i] = 1;
            g_cbvt_blkaddr[i] = cb;
            for (int k = 0; k < 0x48; k++) g_cbvt_raw[i][k] = B[k];
            const u32 pkt = r32(J3DSYS + 0x3C); g_cbvt_matpkt[i] = pkt;     // live J3DMatPacket
            if (const u8* P = sb_ram_fast(pkt)) for (int k = 0; k < 0x40; k++) g_cbvt_pktraw[i][k] = P[k];
            break; }
    }
    if (!g_dbg_cb_have) {                    // one-shot raw dump of the first block
        for (int k = 0; k < 0x44; k++) g_dbg_cb_raw[k] = B[k];
        g_dbg_cb_vt = vt; g_dbg_cb_have = true;
    }
    // Channel control + matColor + ambient come from the J3D color block (synchronous guest
    // RAM — the function-level GX hooks miss J3D's direct-XF-write fast path, and xfmem lags
    // at draw time because the GPU thread is async). matColor@0x04 matches xfmem (white).
    u32 chan_off, amb_off = 0, cull_off;
    if (vt == VT_CLON)      { chan_off = 0x16; amb_off = 0x0C; cull_off = 0x40; }  // J3DColorBlockLightOn
    else if (vt == VT_CLOF) { chan_off = 0x0E; cull_off = 0x16; }                  // J3DColorBlockLightOff
    else return;
    g_cur_chan.cullMode = B[cull_off];
    for (int k = 0; k < 4; k++) g_cur_chan.matColor[k] = B[0x04 + k];
    g_cur_chan.color0 = (u16)((B[chan_off + 0] << 8) | B[chan_off + 1]);   // mColorChan[0] (COLOR0)
    g_cur_chan.alpha0 = (u16)((B[chan_off + 2] << 8) | B[chan_off + 3]);   // mColorChan[1] (ALPHA0)
    // Ambient: LightOn blocks store it in the block (+0x0C). LightOff (CLOF) blocks store NO
    // ambient field but their channel control can still enable lighting with ambsrc=REG — GX
    // reads the ambient from the global GXSetChanAmbColor register, which we track live per draw
    // (g_amb_reg, updated synchronously by the ov_gxsetchanambcolor hook, ~198k sets/scene).
    // NOTE: the global register value (g_amb_reg) is the WRONG ambient for these materials
    // (it reads ~purple (128,66,99) and tints CLOF lit surfaces purple — verified vs the live
    // oracle). The correct per-material ambient is written by J3D directly to XF (xfmem), which
    // LAGS in this capture process — capturing it correctly is an open RE item. Default: 0.
    // (SUNBRIGHT_NGX_AMBGLOBAL=1 = opt-in A/B with the global register.)
    // CLOF lit materials read the ambient from the global XF ambient register. OPEN PROBLEM:
    // that register is programmed neither via the J3DColorBlock (CLOF has no ambient field) nor
    // via the GX/GD ambient tees we capture — J3DGDSetChanAmbColor fires 0× in file-select and
    // GXSetChanAmbColor reads a wrong ~purple value; neither matches the oracle. So default to 0
    // (the studied-safe value; gameplay renders correctly with it). Opt-in A/B:
    // SUNBRIGHT_NGX_AMBGD=1 uses the J3DGDSetChanAmbColor capture (correct in LightOn scenes).
    // Ambient: the faithful per-draw ambient is the global XF ambient register the game programs via
    // GXSetChanAmbColor, read LIVE here at the material's draw. (The per-material display list loads
    // matColor XF 0x100C/D but NOT ambient — RE'd via the MatPacket DL walker in /ngxshape; the
    // ambient is a separate GXSetChanAmbColor write that varies per draw-group: grey 808080 for the
    // 3D scene, 0/white for others.) The earlier "purple" was a by-VALUE read of the GXColor POINTER
    // arg — now fixed (ov_gxsetchanambcolor derefs gpr[4]). ambSrc=VTX materials ignore this
    // (light_vertex substitutes the vertex colour). A/B: SUNBRIGHT_NGX_AMB0=1 forces the old 0.
    // PER-MATERIAL ambient (the wash fix, 2026-06-18): a J3DColorBlockLightOn block carries its own
    // ambient color in the block at +0x0C (right after matColor@0x04), exactly like matColor. The old
    // code computed amb_off but then IGNORED it and used the GLOBAL GXSetChanAmbColor register
    // (g_amb_reg) for every material — that register reads grey (128,128,128) while the GPU's true
    // per-material ambient (xfmem) is 0, so every lit surface got +0.5 grey ambient → the Delfino
    // floor rendered ~2.2x too bright (/gxstate: ngx amb 128 vs xfmem 0 on every material). Read the
    // block's own ambient for LightOn materials (matches xfmem); LightOff blocks have no ambient field
    // → fall back to the global register (or 0). A/B: SUNBRIGHT_NGX_AMBGLOBAL=1 forces the old global
    // register for all; SUNBRIGHT_NGX_AMB0=1 forces 0.
    static const bool amb_global = sb_env_on("SUNBRIGHT_NGX_AMBGLOBAL");  // A/B: force the old global reg
    if (amb_global && g_amb_have[0])
                       for (int k = 0; k < 4; k++) g_cur_chan.ambColor[k] = g_amb_reg[0][k];
    else if (amb_off)  for (int k = 0; k < 4; k++) g_cur_chan.ambColor[k] = B[amb_off + k];  // CLON: per-material block
    else               for (int k = 0; k < 4; k++) g_cur_chan.ambColor[k] = 0;               // CLOF: 0 (matches xfmem)
    // COLOR1A1 (mColorChan[2]=COLOR1@chan_off+4, [3]=ALPHA1@chan_off+6) — the 2nd GX colour
    // channel. Materials whose TEV reads rasChan=COLOR1/ALPHA1 (floor s4, buildings s1) need a
    // DISTINCT channel-1 raster; ngx used to alias col1=col0 (→ over-bright). Its material colour
    // is mMatColor[1]@0x08 and ambient mAmbColor[1]@0x10 — EXACTLY symmetric to COLOR0
    // (matColor[0]@0x04, ambColor[0]@0x0C), verified vs J3DColorBlocks.hpp. CLOF blocks have no
    // ambient field → 0 (same as COLOR0). Earlier attempt used ambient=0 for ALL channel-1 →
    // scene went black; the real ambient is the block's own mAmbColor[1].
    g_cur_chan.color1 = (u16)((B[chan_off + 4] << 8) | B[chan_off + 5]);   // mColorChan[2] (COLOR1)
    g_cur_chan.alpha1 = (u16)((B[chan_off + 6] << 8) | B[chan_off + 7]);   // mColorChan[3] (ALPHA1)
    for (int k = 0; k < 4; k++) g_cur_chan.matColor1[k] = B[0x08 + k];     // mMatColor[1]
    if (amb_off) for (int k = 0; k < 4; k++) g_cur_chan.ambColor1[k] = B[0x10 + k];  // CLON: mAmbColor[1]
    else         for (int k = 0; k < 4; k++) g_cur_chan.ambColor1[k] = 0;            // CLOF: 0
    g_cur_chan.valid = true;
}

// GX per-vertex colour-channel lighting for COLOR0/ALPHA0 (faithful to the GC
// hardware lighting model; math cross-checked vs Dolphin VertexShaderGen, re-
// derived). eye = eye-space position, en = eye-space UNIT normal, vcol0 = vertex
// CLR0 (0..1). Writes the lit RGBA (0..1) to out. With lighting disabled this
// reduces to the channel's material source (register colour or vertex colour), so
// the common vertex-lit world materials are unchanged.
bool g_nolight = (getenv("SUNBRIGHT_NGX_NOLIGHT") != nullptr);   // A/B diag: bypass lighting
// Runtime toggle (via /ngxdbg?nolight=N) so the harness can flip lighting on a LIVE scene —
// out=raw vertex color (vcol0) when on. Affects subsequently-captured frames.
extern "C" void sb_ngx_set_nolight(int on) { g_nolight = on != 0; }
// DBG per-light breakdown for one floor (up-facing, reg-color, lit) vertex.
bool g_dbgL_done = false, g_dbgL_active = false; int g_dbgL_n = 0; u16 g_dbgL_cc = 0;
int g_dbgL_i[8] = {0}; float g_dbgL_attn[8]={0}, g_dbgL_ndl[8]={0}, g_dbgL_dist[8]={0}, g_dbgL_contrib[8]={0};
float g_dbgL_amb[3]={0}, g_dbgL_illum[3]={0};
// Per-material colour probe (SUNBRIGHT_NGX_CLRDBG=<tev_index>): latch the FIRST vertex of that
// material's RAW vcol0 (pre-lighting), the illum (ambient+lights), mat source, and final out — so
// a wrong on-screen colour can be split into DECODE (raw vcol0 wrong) vs LIGHTING (illum wrong).
extern int g_cur_tev_index;   // defined below (material index of the shape being captured)
const int g_clrdbg_ti = []{ const char* v=getenv("SUNBRIGHT_NGX_CLRDBG"); return v?atoi(v):-1; }();
bool g_clrdbg_have=false; u16 g_clrdbg_cc=0; int g_clrdbg_matvtx=0, g_clrdbg_ambvtx=0, g_clrdbg_nl=0;
float g_clrdbg_vcol[4]={0}, g_clrdbg_illum[3]={0}, g_clrdbg_mat[3]={0}, g_clrdbg_out[4]={0};
// Per-draw light/ambient breakdown for the CLRDBG material (the per-draw light-state probe):
// the ambient component (C.ambColor at draw) + the first masked light's color and s=attn·diff.
float g_clrdbg_amb[3]={0}; int g_clrdbg_l0i=-1; float g_clrdbg_l0col[3]={0}, g_clrdbg_l0s=0, g_clrdbg_l0ndl=0, g_clrdbg_l0attn=0;
// SKY latch: full per-draw lighting breakdown for the first reg-color LIT vertex whose
// channel-control == 0x0686 (the file-select sky material). Reset each frame so the static
// screen always shows fresh values. Captures the EXACT light/ambient state the sky used at
// its draw (not the end-of-frame g_light snapshot) to settle ambient-vs-light-colour.
struct SkyLatch {
    bool have=false; u16 cc=0, ca=0; float matc[4]={0}, ambc[4]={0}, en[3]={0}, illum[3]={0}, out[4]={0};
    bool hasNrm=false; int nl=0; int li[8]={0}; float lcol[8][3]={{0}}; float lndl[8]={0},
    lattn[8]={0}, ldiff[8]={0};
    u8 amb_reg_live[4]={0};   // live GXSetChanAmbColor register value AT sky draw
    float vcol0[4]={0};       // the sky vertex's raw CLR0 (is it the blue source?)
    u8 tgnum=0; u8 tgsrc[4]={0}; u8 tgtype[4]={0}; u8 tgmtx[4]={0};  // texgen per coord
    float tg0m[12]={0};   // tc0 texgen matrix (the sky tex0 sampling — wash suspect)
    // tc0/tc1 UV bbox over the sky verts (accumulated in the transform loop)
    float uv0min[2]={1e9f,1e9f}, uv0max[2]={-1e9f,-1e9f};
    float uv1min[2]={1e9f,1e9f}, uv1max[2]={-1e9f,-1e9f}; unsigned long uvn=0;
};
SkyLatch g_sky;       // latched this frame
SkyLatch g_sky_pub;   // published (kept across the reset for the probe)
void light_vertex(const float eye[3], const float en[3], const float vcol0[4], const float vcol1[4],
                  float out[4], float out1[4]) {
    const ChanInfo& C = g_cur_chan;
    if (g_nolight || !C.valid) { for (int k = 0; k < 4; k++) { out[k] = vcol0[k]; out1[k] = vcol1[k]; } return; }
    const u16 cc = C.color0, ca = C.alpha0;
    const bool matVtx = (cc >> 0) & 1;      // GXColorSrc: REG=0, VTX=1
    const bool enable = (cc >> 1) & 1;
    float mat[3];
    if (matVtx) { mat[0] = vcol0[0]; mat[1] = vcol0[1]; mat[2] = vcol0[2]; }
    else { mat[0] = C.matColor[0] / 255.f; mat[1] = C.matColor[1] / 255.f; mat[2] = C.matColor[2] / 255.f; }
    // Alpha channel source (no full alpha lighting in this slice).
    out[3] = ((ca >> 0) & 1) ? vcol0[3] : C.matColor[3] / 255.f;

    if (!enable) {
        out[0] = mat[0]; out[1] = mat[1]; out[2] = mat[2]; g_chan_flat++;
        if (matVtx) g_flat_vtx++; else g_flat_reg++;
        g_flat_lum_sum += (out[0] + out[1] + out[2]) / 3.0;
        return;
    }
    g_chan_lit++;

    const bool ambVtx = (cc >> 6) & 1;
    float illum[3];
    if (ambVtx) { illum[0] = vcol0[0]; illum[1] = vcol0[1]; illum[2] = vcol0[2]; }
    else { illum[0] = C.ambColor[0] / 255.f; illum[1] = C.ambColor[1] / 255.f; illum[2] = C.ambColor[2] / 255.f; }
    // DIAG: substitute Dolphin's authoritative ambient (xfmem) — confirms the 0-default is the bug.
    if (g_use_dolamb) {
        unsigned a=0,m=0; unsigned long c=0; sb_get_gx_ambient(&a,&m,&c);
        illum[0] = ((a>>24)&0xff)/255.f; illum[1] = ((a>>16)&0xff)/255.f; illum[2] = ((a>>8)&0xff)/255.f;
    }

    const int diffFn  = (cc >> 7) & 3;      // GXDiffuseFn: NONE=0 SIGN=1 CLAMP=2
    const int attnSel = (cc >> 9) & 3;      // 0/2 → NONE, 1 → SPEC, 3 → SPOT (J3DColorChan::getAttnFn)
    const u8  mask    = (u8)(((cc >> 2) & 0x0F) | (((cc >> 11) & 0x0F) << 4));
    // SKY latch: first reg-color lit 0x0686 vertex this frame.
    const bool sky_latch = (cc == 0x0686) && !matVtx && !g_sky.have;
    if (sky_latch) {
        g_sky.have = true; g_sky.cc = cc; g_sky.ca = ca; g_sky.nl = 0;
        for (int k=0;k<4;k++){ g_sky.matc[k]=C.matColor[k]; g_sky.ambc[k]=C.ambColor[k];
                               g_sky.amb_reg_live[k]=g_amb_reg[0][k]; }
        for (int k=0;k<3;k++){ g_sky.en[k]=en[k]; }
        for (int k=0;k<4;k++){ g_sky.vcol0[k]=vcol0[k]; }
        g_sky.tgnum = g_cur_texgen.num;
        for (int k=0;k<4 && k<g_cur_texgen.num;k++){ g_sky.tgsrc[k]=g_cur_texgen.tg[k].src;
            g_sky.tgtype[k]=g_cur_texgen.tg[k].type; g_sky.tgmtx[k]=g_cur_texgen.tg[k].has_mtx; }
        if (g_cur_texgen.num > 0) for (int k=0;k<12;k++) g_sky.tg0m[k]=g_cur_texgen.tg[0].m[k];
        g_sky.hasNrm = (en[0]*en[0]+en[1]*en[1]+en[2]*en[2]) > 1e-6f;
    }
    // DBG latch: dump the per-light breakdown of the FIRST up-facing reg-color lit vertex.
    if (!g_dbgL_done && !matVtx && !ambVtx && en[1] > 0.85f) {
        g_dbgL_active = true; g_dbgL_n = 0; g_dbgL_done = true; g_dbgL_cc = cc;
        g_dbgL_amb[0]=illum[0]; g_dbgL_amb[1]=illum[1]; g_dbgL_amb[2]=illum[2];
    }
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1 << i)) || !g_light[i].valid) continue;
        const LightObj& L = g_light[i];
        float ld[3] = { L.pos[0] - eye[0], L.pos[1] - eye[1], L.pos[2] - eye[2] };
        const float dist2 = ld[0]*ld[0] + ld[1]*ld[1] + ld[2]*ld[2];
        const float dist  = std::sqrt(dist2);
        if (dist > 1e-6f) { ld[0] /= dist; ld[1] /= dist; ld[2] /= dist; }
        float attn = 1.f;
        if (attnSel == 1) {                 // GX_AF_SPEC
            const float ndl = en[0]*ld[0] + en[1]*ld[1] + en[2]*ld[2];
            const float ang = ndl >= 0.f ? std::fmax(0.f, en[0]*L.dir[0] + en[1]*L.dir[1] + en[2]*L.dir[2]) : 0.f;
            const float a = std::fmax(0.f, L.cosA[0] + L.cosA[1]*ang + L.cosA[2]*ang*ang);
            const float k = L.distA[0] + L.distA[1]*ang + L.distA[2]*ang*ang;
            attn = (k > 1e-6f) ? a / k : 0.f;
        } else if (attnSel == 3) {          // GX_AF_SPOT
            const float c = std::fmax(0.f, ld[0]*L.dir[0] + ld[1]*L.dir[1] + ld[2]*L.dir[2]);
            const float a = std::fmax(0.f, L.cosA[0] + L.cosA[1]*c + L.cosA[2]*c*c);
            const float k = L.distA[0] + L.distA[1]*dist + L.distA[2]*dist2;
            attn = (k > 1e-6f) ? a / k : 0.f;
        }
        float diff = 1.f;
        const float ndl = en[0]*ld[0] + en[1]*ld[1] + en[2]*ld[2];
        if (diffFn == 1) diff = ndl;                    // SIGN
        else if (diffFn == 2) diff = std::fmax(0.f, ndl);  // CLAMP
        const float s = attn * diff;
        illum[0] += s*L.color[0]; illum[1] += s*L.color[1]; illum[2] += s*L.color[2];
        g_diff_sum += diff; g_diff_n++;
        // Per-draw light-state probe: record the FIRST masked light's contribution for the
        // CLRDBG material, captured at ITS draw (so it can't be the global last-writer snapshot).
        if (g_clrdbg_ti >= 0 && g_cur_tev_index == g_clrdbg_ti && !g_clrdbg_have && g_clrdbg_l0i < 0) {
            g_clrdbg_l0i = i; g_clrdbg_l0s = s; g_clrdbg_l0ndl = ndl; g_clrdbg_l0attn = attn;
            for (int k=0;k<3;k++) g_clrdbg_l0col[k] = L.color[k];
        }
        if (sky_latch && g_sky.nl < 8) {
            int j = g_sky.nl++;
            g_sky.li[j] = i; g_sky.lndl[j]=ndl; g_sky.lattn[j]=attn; g_sky.ldiff[j]=diff;
            g_sky.lcol[j][0]=L.color[0]; g_sky.lcol[j][1]=L.color[1]; g_sky.lcol[j][2]=L.color[2];
        }
        // DBG: capture per-light breakdown for ONE representative up-facing (floor)
        // vertex of a register-color lit material (matsrc=reg, ambsrc=reg) — wash suspect.
        if (g_dbgL_active && g_dbgL_n < 8) {
            g_dbgL_i[g_dbgL_n]=i; g_dbgL_attn[g_dbgL_n]=attn; g_dbgL_ndl[g_dbgL_n]=ndl;
            g_dbgL_dist[g_dbgL_n]=dist; g_dbgL_contrib[g_dbgL_n]=s*L.color[0]; g_dbgL_n++;
        }
        if (i == 0) {   // sun alignment probe
            g_sun_ndl_sum += ndl; g_sun_ndl_n++;
            if (ndl > 0.f) g_sun_ndl_pos++;
            if (ndl > g_sun_ndl_max) {
                g_sun_ndl_max = ndl;
                for (int k = 0; k < 3; k++) { g_dbg_en[k] = en[k]; g_dbg_ld0[k] = ld[k]; }
            }
        }
    }
    if (g_dbgL_active) { g_dbgL_active = false;
        g_dbgL_illum[0]=illum[0]; g_dbgL_illum[1]=illum[1]; g_dbgL_illum[2]=illum[2]; }
    // DBG: average + max illum for up-facing reg-color lit verts (the visible floor/ground).
    if (!matVtx && en[1] > 0.7f) {
        double il = (illum[0]+illum[1]+illum[2])/3.0;
        g_uplit_sum += il; g_uplit_n++;
        if (il > g_uplit_max) { g_uplit_max = il;
            g_uplit_amb = (C.ambColor[0]+C.ambColor[1]+C.ambColor[2])/3.0/255.0;
            g_uplit_ndl = en[0]*g_dbg_ld0[0]+en[1]*g_dbg_ld0[1]+en[2]*g_dbg_ld0[2]; }
    }
    // SHIPPING colour comes from the extracted, unit-tested ngx::light_color0 (render_test
    // test_lighting). The illum loop above is the DIAGNOSTIC mirror (per-light breakdown for
    // the /ngxshape probes); the returned out[] is authoritative from the tested unit so the
    // test validates the real path, not a fork. Capture-time (not the per-frame hot path).
    {
        ngx::ChanCtl CC = ngx::decode_chanctl(cc);
        ngx::LightSrc ls[8];
        for (int i = 0; i < 8; i++) {
            ls[i].valid = g_light[i].valid;
            for (int k = 0; k < 3; k++) { ls[i].color[k]=g_light[i].color[k]; ls[i].pos[k]=g_light[i].pos[k];
                ls[i].dir[k]=g_light[i].dir[k]; ls[i].cosA[k]=g_light[i].cosA[k]; ls[i].distA[k]=g_light[i].distA[k]; }
        }
        const float matC[3] = { C.matColor[0]/255.f, C.matColor[1]/255.f, C.matColor[2]/255.f };
        const float ambC[3] = { C.ambColor[0]/255.f, C.ambColor[1]/255.f, C.ambColor[2]/255.f };
        ngx::light_color0(CC, matC, ambC, ls, eye, en, vcol0, out);
    }
    // Per-material colour probe: latch raw vcol0 vs illum vs out for the target tev_index.
    if (g_clrdbg_ti >= 0 && g_cur_tev_index == g_clrdbg_ti && !g_clrdbg_have) {
        g_clrdbg_have = true; g_clrdbg_cc = cc; g_clrdbg_matvtx = matVtx; g_clrdbg_ambvtx = ambVtx;
        for (int k=0;k<4;k++){ g_clrdbg_vcol[k]=vcol0[k]; g_clrdbg_out[k]=out[k]; }
        for (int k=0;k<3;k++){ g_clrdbg_illum[k]=illum[k]; g_clrdbg_mat[k]=mat[k];
                               g_clrdbg_amb[k]=ambVtx?vcol0[k]:C.ambColor[k]/255.f; }
        int n=0; for (int i=0;i<8;i++) if ((mask&(1<<i)) && g_light[i].valid) n++;
        g_clrdbg_nl = n;
    }
    if (sky_latch) {
        for (int k=0;k<3;k++){ g_sky.illum[k]=illum[k]; g_sky.out[k]=out[k]; }
        g_sky.out[3]=out[3];
    }
    g_lit_lum_sum += (out[0] + out[1] + out[2]) / 3.0;
    g_dbg_cc = cc;
    for (int k = 0; k < 4; k++) { g_dbg_mat[k] = C.matColor[k]; g_dbg_amb[k] = C.ambColor[k]; g_dbg_out[k] = out[k]; }
    for (int k = 0; k < 3; k++) g_dbg_illum[k] = illum[k];

    // COLOR1A1: the 2nd raster channel, computed exactly like COLOR0 (same ngx::light_color0
    // unit) but with channel-1's control reg + its own material colour (mMatColor[1]) and ambient
    // (mAmbColor[1]). Channel-1 VTX sources read the vertex's CLR1 (vcol1). A TEV stage reading
    // rasChan=COLOR1/ALPHA1 uses this; materials with no distinct channel 1 still get a correct
    // value (≈ col0 when they share material/ambient/lights).
    {
        ngx::ChanCtl CC1 = ngx::decode_chanctl(C.color1);
        ngx::LightSrc ls[8];
        for (int i = 0; i < 8; i++) {
            ls[i].valid = g_light[i].valid;
            for (int k = 0; k < 3; k++) { ls[i].color[k]=g_light[i].color[k]; ls[i].pos[k]=g_light[i].pos[k];
                ls[i].dir[k]=g_light[i].dir[k]; ls[i].cosA[k]=g_light[i].cosA[k]; ls[i].distA[k]=g_light[i].distA[k]; }
        }
        const float matC1[3] = { C.matColor1[0]/255.f, C.matColor1[1]/255.f, C.matColor1[2]/255.f };
        const float ambC1[3] = { C.ambColor1[0]/255.f, C.ambColor1[1]/255.f, C.ambColor1[2]/255.f };
        ngx::light_color0(CC1, matC1, ambC1, ls, eye, en, vcol1, out1);
        out1[3] = ((C.alpha1 >> 0) & 1) ? vcol1[3] : C.matColor1[3]/255.f;   // ALPHA1 src: VTX/REG
        g_col1_verts++;
        if (std::fabs(out1[0]-out[0]) + std::fabs(out1[1]-out[1]) + std::fabs(out1[2]-out[2]) > 0.02f)
            g_col1_diff_verts++;
    }
}

// ── N5 per-material TEV capture (docs/native_port_plan.md §6) ───────────────────
// The current material is reachable at J3DShape::draw time via the j3dSys global:
// J3DMatPacket::draw sets j3dSys.mMatPacket (+0x3C) to the packet whose material
// it is about to load, then draws that packet's shape packets (→ J3DShape::draw).
// So mMatPacket+0x38 (J3DMatPacket::unk38) is the live J3DMaterial; +0x28 is its
// J3DTevBlock. The block's concrete variant (TVB1/2/4/16 — different field
// offsets) is identified by its vtable pointer (gmse01 addresses; the relative
// 0x9C spacing/order is from the JP symbol map __vt__*J3DTevBlock*, anchored to
// TVB1 = 0x803E0BE8 disassembled from __dt__12J3DTevBlock1). Verified at runtime
// by the /ngxshape vtable histogram.
constexpr u32 VT_TVB16 = 0x803E0A14u, VT_TVB4 = 0x803E0AB0u,
              VT_TVB2  = 0x803E0B4Cu, VT_TVB1 = 0x803E0BE8u;

// Per-variant field offsets (from reference/sms J3DTevBlocks.hpp). stage_off and
// order_off step by 8 / 4 bytes respectively; sentinel 0 = field absent.
struct TevLayout {
    u32 stagenum_off;   // mTevStageNum (0 ⇒ implicit 1 stage, TVB1)
    u32 order_off;      // mTevOrder[0]
    u32 stage_off;      // mTevStage[0]
    u32 tevcolor_off;   // mTevColor[0]  (0 ⇒ none, TVB1)
    u32 kcolor_off;     // mTevKColor[0] (0 ⇒ none)
    u32 kcsel_off;      // mTevKColorSel[0] (0 ⇒ none)
    u32 kasel_off;      // mTevKAlphaSel[0] (0 ⇒ none)
    u32 swaptable_off;  // mTevSwapModeTable[0] (0 ⇒ none ⇒ identity, TVB1)
    u32 indtev_off;     // mIndTevStage[0] (J3DIndTevStage, 0xC bytes each)
};
inline bool tev_layout(u32 vt, TevLayout& L) {
    switch (vt) {
    case VT_TVB1:  L = {0,      0x06, 0x0A, 0,     0,     0,     0,     0,     0x12 }; return true;
    case VT_TVB2:  L = {0x30,   0x08, 0x31, 0x10,  0x41,  0x51,  0x53,  0x55,  0x59 }; return true;
    case VT_TVB4:  L = {0x1C,   0x0C, 0x1D, 0x3E,  0x5E,  0x6E,  0x72,  0x76,  0x7A }; return true;
    case VT_TVB16: L = {0x54,   0x14, 0x55, 0xD6,  0xF6,  0x106, 0x116, 0x126, 0x12A}; return true;
    default: return false;
    }
}

// N6.7: read the material's per-texmap texture binding OBJECT-MODEL into g_mat_tex.
// The TEV block holds mTexNo[]@+0x04 (u16 per texmap, count by variant); j3dSys's
// J3DTexture (J3DSYS+0x54: mResourceCount u16@0x0, ResTIMG* mResources@0x4) is the
// model's texture table. ResTIMG (size 0x20): format@0x00, width@0x02, height@0x04,
// colorFormat(TLUT)@0x09, paletteOffset@0x0C, imageDataOffset@0x1C — both offsets
// self-relative to the (relocated) header. Image addr = timg + imageDataOffset.
void capture_textures(u32 tevblock, u32 vt) {
    for (int m = 0; m < 8; m++) g_mat_tex[m] = NgxTexBind{};
    const int ntm = vt == VT_TVB16 ? 8 : vt == VT_TVB4 ? 4 : vt == VT_TVB2 ? 2 : 1;
    const u32 jtex = r32(J3DSYS + 0x54);          // j3dSys.mTexture (J3DTexture*)
    if (!valid(jtex)) return;
    const u32 count = (r32(jtex + 0x00) >> 16) & 0xFFFF;   // mResourceCount (u16 @ 0x00)
    const u32 res = r32(jtex + 0x04);             // ResTIMG* mResources
    if (!valid(res) || count == 0) return;
    const u8* tb = sb_ram_fast(tevblock);
    if (!tb) return;
    for (int m = 0; m < ntm; m++) {
        const u16 texNo = (u16)((tb[0x04 + m * 2] << 8) | tb[0x04 + m * 2 + 1]);
        if (texNo == 0xFFFF || texNo >= count) continue;
        const u32 timg = res + (u32)texNo * 0x20;
        const u8* T = sb_ram_fast(timg);
        if (!T) continue;
        NgxTexBind& d = g_mat_tex[m];
        d.fmt = T[0x00];
        d.w = (u16)((T[0x02] << 8) | T[0x03]);
        d.h = (u16)((T[0x04] << 8) | T[0x05]);
        d.wrap_s = T[0x06]; d.wrap_t = T[0x07];     // GX wrap mode (0=CLAMP 1=REPEAT 2=MIRROR)
        d.mipmap = T[0x10];                        // mipmapEnabled
        d.min_filter = T[0x14]; d.mag_filter = T[0x15];   // GXTexFilter
        // ResTIMG mipmapCount @ 0x18: number of stored mip levels. Used ONLY when the GX min
        // filter is a mip variant (>=2 = *_MIP_*); else the game samples level 0 only. The
        // authored high mips can differ wildly from level 0 (e.g. the Delfino floor's distant
        // levels are dark blue), so honouring them is required for correct minified surfaces.
        d.mip_count = (d.min_filter >= 2 && T[0x18] > 1) ? T[0x18] : 1;
        d.tlut_fmt = T[0x09];                     // colorFormat (TLUT fmt for CI)
        const u32 imgOff = ((u32)T[0x1C] << 24) | ((u32)T[0x1D] << 16) | ((u32)T[0x1E] << 8) | T[0x1F];
        d.addr = timg + imgOff;
        if (d.fmt == 0x8 || d.fmt == 0x9 || d.fmt == 0xA) {   // CI → palette
            const u32 palOff = ((u32)T[0x0C] << 24) | ((u32)T[0x0D] << 16) | ((u32)T[0x0E] << 8) | T[0x0F];
            d.tlut_addr = timg + palOff;
        }
    }
}

// ── N7 PE block (alpha test + blend + zmode) ────────────────────────────────────
// The material's mPEBlock @ +0x30 decides framebuffer behaviour. Four variants,
// identified by the block's vtable getType() return (self-identifying — no region-
// specific vtable addresses hardcoded): 'PEOP' opaque, 'PEED' TexEdge cutout
// (alpha-tested foliage), 'PEXL' xlu (alpha blend) are PRESETS storing no fields;
// 'PEFL' full stores J3DAlphaComp/J3DBlend/J3DZMode. The preset GX state is ported
// verbatim from J3DPEBlock*::load (reference/sms J3DMaterial.cpp). The J3DAlphaComp
// / J3DZMode IDs decode as a plain bitfield — that IS what makeAlphaCmpTable /
// makeZModeTable build (J3DTevs.cpp): alphaID=(comp0<<5)|(op<<3)|comp1,
// zID=(cmpEn<<4)|(func<<1)|updEn.
// J3DIndBlock getType FourCCs: 'IBLF' = full (has indirect fields), 'IBLN' = null (none).
constexpr u32 IND_FULL = 0x49424C46u /*'IBLF'*/, IND_NULL = 0x49424C4Eu /*'IBLN'*/;
constexpr u32 PE_OP = 0x50454F50u /*'PEOP'*/, PE_ED = 0x50454544u /*'PEED'*/,
              PE_XL = 0x5045584Cu /*'PEXL'*/, PE_FL = 0x5045464Cu /*'PEFL'*/;

// Decode a J3D PE block's getType() tag from its vtable. Stored vtable ptr → slot 0
// at +8 (this codebase's CW/GC convention, see sms_drawsync_lossproof.cpp); getType
// is virtual slot 2 → vtable+0x10. Its body builds the 32-bit FourCC as
// `lis r3,HI; {ori|addi} r3,r3,LO; blr` (the compiler picks ori or addi). Returns
// 0 if it can't be decoded.
u32 pe_block_type(u32 vptr) {
    if (!valid(vptr)) return 0;
    const u32 fn = r32(vptr + 0x10);          // virtual slot 2 = getType
    if (!valid(fn)) return 0;
    const u32 i0 = r32(fn), i1 = r32(fn + 4);
    if ((i0 >> 26) != 15 || ((i0 >> 21) & 31) != 3) return 0;   // addis r3,0,HI (lis r3)
    const u32 hi = i0 & 0xFFFF;
    int lo = 0;
    const u32 op1 = i1 >> 26;
    if (op1 == 24)                                 lo = (int)(i1 & 0xFFFF);  // ori (zero-ext)
    else if (op1 == 14 && ((i1 >> 16) & 31) == 3)  lo = (int16_t)(i1 & 0xFFFF); // addi (sign-ext)
    return (u32)((int)(hi << 16) + lo);
}

// Diagnostics: distinct PE vtable → decoded tag + per-tag use count.
struct PeVtEntry { u32 vt = 0, tag = 0, fn = 0, i0 = 0, i1 = 0; unsigned cnt = 0; };
PeVtEntry g_pe_vt[8];
unsigned long g_pe_op = 0, g_pe_ed = 0, g_pe_xl = 0, g_pe_fl = 0, g_pe_unk = 0, g_pe_none = 0;
unsigned long g_pe_alpha = 0, g_pe_blend = 0, g_pe_nozwrite = 0;

inline bool alpha_always_pass(int comp0, int aop, int comp1) {
    const bool t0 = comp0 == 7, f0 = comp0 == 0;   // ALWAYS / NEVER
    const bool t1 = comp1 == 7, f1 = comp1 == 0;
    switch (aop) {
    case 0:  return t0 && t1;              // AND
    case 1:  return t0 || t1;              // OR
    case 2:  return (t0 && f1) || (f0 && t1);   // XOR
    default: return (t0 && t1) || (f0 && f1);   // XNOR
    }
}

// ── Indirect texturing capture (mIndBlock + mIndTevStage) ───────────────────────
// Read the material's indirect state into st.ind. mIndBlock @ material+0x2C holds the
// stage count, per-stage tex order (which texcoord/texmap feeds the indirect lookup),
// the 3 indirect offset matrices, and the per-stage coord scale; mIndTevStage[] (in the
// TevBlock, variant offset L.indtev_off) holds the per-regular-TEV-stage config. The
// float offset matrices are quantized to GX's S2.10 mantissas (ngx_ind_mtx_mantissa) so
// the generated shader can mirror Dolphin's integer math bit-for-bit.
// Tallied here for /ngxshape: how many materials actually emit an active indirect stage.
unsigned long g_ind_applied = 0;       // materials with >=1 enabled indirect TEV stage (Indirect mtx)
unsigned long g_ind_st_skipped = 0;    // enabled stages using an S/T matrix variant (not yet in shader)
void capture_indirect(u32 material, u32 tevblock, const TevLayout& L, NgxTevState& st) {
    st.ind = NgxIndirect{};
    const u32 indb = r32(material + 0x2C);                 // J3DMaterial::mIndBlock
    if (!valid(indb)) return;
    if (pe_block_type(r32(indb + 0x00)) != IND_FULL) return;   // IBLN / unknown → no indirect
    const u8* IB = sb_ram_fast(indb);
    if (!IB) return;
    const u8 nstg = IB[0x04];                             // mIndTexStageNum
    if (nstg == 0 || nstg > 4) return;
    st.ind.num_stages = nstg;
    for (int i = 0; i < nstg; i++) {
        st.ind.order_coord[i] = IB[0x05 + i * 4 + 0];     // J3DIndTexOrder.mCoord
        st.ind.order_map[i]   = IB[0x05 + i * 4 + 1];     // J3DIndTexOrder.mMap
        st.ind.scale_s[i]     = IB[0x6C + i * 4 + 0];     // IndTexCoordScale.mScaleS
        st.ind.scale_t[i]     = IB[0x6C + i * 4 + 1];     // IndTexCoordScale.mScaleT
    }
    for (int m = 0; m < 3; m++) {                         // 3 IndTexMtx (0x1C bytes each)
        const u32 mo = indb + 0x18 + (u32)m * 0x1C;
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 3; c++)
                st.ind.mtx[m][r][c] = ngx::ngx_ind_mtx_mantissa(rf(mo + (u32)(r * 3 + c) * 4));
        st.ind.mtx_exp[m] = (int8_t)IB[(0x18 + m * 0x1C) + 0x18];   // mScaleExp (s8)
    }
    // Per regular TEV stage indirect config (J3DIndTevStage, 0xC bytes).
    const u8* TB = sb_ram_fast(tevblock);
    if (!TB || !L.indtev_off) return;
    bool any = false, st_used = false;
    for (int s = 0; s < st.num_stages; s++) {
        const u32 io = L.indtev_off + (u32)s * 0xC;
        NgxIndTevStage& d = st.ind.stage[s];
        d.ind_stage = TB[io + 0]; d.format   = TB[io + 1]; d.bias_sel = TB[io + 2];
        d.mtx_sel   = TB[io + 3]; d.wrap_s   = TB[io + 4]; d.wrap_t   = TB[io + 5];
        d.add_prev  = TB[io + 6]; d.alpha_sel = TB[io + 8];
        int mi, kind;
        const bool on = ngx::ngx_ind_mtx_decode(d.mtx_sel, &mi, &kind) && d.ind_stage < nstg;
        d.enabled = on ? 1 : 0;
        if (on) { any = true; if (kind != ngx::NGX_ITM_INDIRECT) st_used = true; }
    }
    if (any) g_ind_applied++;
    if (st_used) g_ind_st_skipped++;
}

// Read the material's PE block into st.pe. Default (no/unknown block) = opaque:
// depth test+write LEQUAL, no blend, no alpha test.
void capture_pe(u32 material, NgxTevState& st) {
    st.pe = NgxPEState{};
    st.pe.z_test = 1; st.pe.z_func = 3 /*GX_LEQUAL*/; st.pe.z_write = 1;
    const u32 peb = r32(material + 0x30);              // J3DMaterial::mPEBlock
    if (!valid(peb)) { g_pe_none++; return; }
    const u32 vt = r32(peb + 0x00);
    const u32 tag = pe_block_type(vt);

    for (int i = 0; i < 8; i++) {                       // vtable→tag histogram
        if (g_pe_vt[i].vt == vt) { g_pe_vt[i].cnt++; break; }
        if (g_pe_vt[i].vt == 0) {
            g_pe_vt[i].vt = vt; g_pe_vt[i].tag = tag; g_pe_vt[i].cnt = 1;
            g_pe_vt[i].fn = r32(vt + 0x10);
            g_pe_vt[i].i0 = r32(g_pe_vt[i].fn); g_pe_vt[i].i1 = r32(g_pe_vt[i].fn + 4);
            break;
        }
    }

    if (tag == PE_OP) {            // opaque: ALWAYS alpha, no blend, z LEQUAL test+write
        g_pe_op++;                 // defaults already opaque
    } else if (tag == PE_ED) {     // TexEdge cutout (foliage): alpha >=0x80 AND <=0xff
        g_pe_ed++;
        st.pe.alpha_test = 1;
        st.pe.comp0 = 6 /*GEQUAL*/; st.pe.ref0 = 0x80; st.pe.aop = 0 /*AND*/;
        st.pe.comp1 = 3 /*LEQUAL*/; st.pe.ref1 = 0xFF;
        g_pe_alpha++;
    } else if (tag == PE_XL) {     // xlu: no alpha, SRCALPHA/INVSRCALPHA blend, z test, NO write
        g_pe_xl++;
        st.pe.blend_mode = 1 /*GX_BM_BLEND*/;
        st.pe.src_factor = 4 /*GX_BL_SRCALPHA*/; st.pe.dst_factor = 5 /*GX_BL_INVSRCALPHA*/;
        st.pe.logic_op = 3 /*GX_LO_COPY*/;
        st.pe.z_test = 1; st.pe.z_func = 3; st.pe.z_write = 0;
        g_pe_blend++; g_pe_nozwrite++;
    } else if (tag == PE_FL) {     // full — read the stored J3DAlphaComp/J3DBlend/J3DZMode
        g_pe_fl++;
        const u8* B = sb_ram_fast(peb);
        if (B) {
            const u16 acid = (u16)((B[0x08] << 8) | B[0x09]);    // mAlphaComp.mAlphaCmpID
            const u8 ref0 = B[0x0A], ref1 = B[0x0B];
            // NOTE: acid==0xFFFF means J3DAlphaComp::load emits NO GDSetAlphaCompare, so the
            // material INHERITS the prior GX alpha-compare state. Tried tracking+applying that
            // live state (the file-select cloud is PEFL id=0xFFFF) — but the cloud inherits
            // ALWAYS (verified: the material before it sets no discard), so it does NOT explain
            // the wash. Reverted. (Dead end: alpha-compare inheritance is not the cloud cause.)
            if (acid != 0xFFFF) {
                const int comp0 = (acid >> 5) & 7, aop = (acid >> 3) & 3, comp1 = acid & 7;
                if (!alpha_always_pass(comp0, aop, comp1)) {
                    st.pe.alpha_test = 1;
                    st.pe.comp0 = (u8)comp0; st.pe.ref0 = ref0; st.pe.aop = (u8)aop;
                    st.pe.comp1 = (u8)comp1; st.pe.ref1 = ref1;
                    g_pe_alpha++;
                }
            }
            st.pe.blend_mode = B[0x0C]; st.pe.src_factor = B[0x0D];   // J3DBlendInfo
            st.pe.dst_factor = B[0x0E]; st.pe.logic_op = B[0x0F];
            if (st.pe.blend_mode == 1 || st.pe.blend_mode == 3) g_pe_blend++;
            const u16 zid = (u16)((B[0x10] << 8) | B[0x11]);          // mZMode.mZModeID
            if (zid != 0xFFFF) {
                st.pe.z_test = (zid >> 4) & 1; st.pe.z_func = (zid >> 1) & 7; st.pe.z_write = zid & 1;
                if (!st.pe.z_write) g_pe_nozwrite++;
            }
        }
    } else {
        g_pe_unk++;                // unknown / undecodable block — keep opaque default
    }
}

// The TEV-state table: deduped by key, persistent across the frame (materials are
// bounded ~ hundreds). Batches reference a state by index. Reset only if it grows
// past the cap (defensive; not expected to be hit).
constexpr size_t TEVSTATE_CAP = 4096;
std::vector<NgxTevState>          g_tevstates;
std::unordered_map<uint64_t, int> g_tevkey_index;
u16 g_tev_cc[TEVSTATE_CAP] = {0};   // DBG: last colour-channel ctrl per tev index (matVtx/enable)
// DBG: per-tev colour-channel detail (matColor/ambColor RGBA + alpha0 ctrl) — for the raster
// source of the file-select sea-haze wash (/pixbatch ti dump). 0 = not yet seen.
struct TevChan { u16 alpha0; u8 mat[4], amb[4]; bool have; };
TevChan g_tev_chan[TEVSTATE_CAP] = {};
// Per-tev-index TevBlock address + vtable (for raw-byte inspection of a material's combiner).
u32 g_tev_tb[TEVSTATE_CAP] = {};
u32 g_tev_vt[TEVSTATE_CAP] = {};
int      g_cur_tev_index = -1;     // material index for the shape being captured
// CLR0 capture state for the shape being captured (for /ngxshapes per-input inspection):
// VCD class (0=none→white default, 1=direct, 2=idx8, 3=idx16), array base ngx samples, VAT format.
unsigned g_cur_clr0cls = 0, g_cur_clr0fmt = 0; u32 g_cur_clr0base = 0;
unsigned g_cur_nrmcls = 0;         // NRM VCD class for the shape being captured (lighting sanity)
// tc0/tc1 VCD class (0=absent) + array base + type/frac — verify the 2nd-octave (tc1=TEX1) decode
unsigned g_cur_tex0cls = 0, g_cur_tex1cls = 0; u32 g_cur_tex0base = 0, g_cur_tex1base = 0;
unsigned g_cur_tex0type = 0, g_cur_tex1type = 0, g_cur_tex0frac = 0, g_cur_tex1frac = 0;
u32      g_cur_shape = 0;          // address of the shape being captured (for the shred metric)
unsigned long g_mat_found = 0, g_mat_novt = 0, g_mat_none = 0;
u32      g_vt_hist_key[8] = {0};    // distinct vtable values seen
unsigned g_vt_hist_cnt[8] = {0};
unsigned g_stage_hist[17] = {0};    // num-stages histogram (0..16)

// ── Indirect-texturing reachability instrumentation (tooling-first gate) ─────────
// Before porting mIndTevStage, confirm reachable-scene materials actually USE it.
// J3DMaterial::mIndBlock @ +0x2C is J3DIndBlockFull ('IBLF', mIndTexStageNum @ +0x04)
// or J3DIndBlockNull ('IBLN', 0 stages). getType is virtual slot 2 (vtable+0x10),
// decoded by pe_block_type (a generic getType-FourCC reader). IND_FULL/IND_NULL above.
unsigned long g_ind_full = 0, g_ind_null = 0, g_ind_none = 0, g_ind_unk = 0;
unsigned g_ind_stage_hist[5] = {0};    // mIndTexStageNum histogram (0..4)
unsigned g_ind_stages_max = 0;

const unsigned char* resolve(unsigned addr, void*) { return sb_ram_fast(addr); }

// Build the CP descriptor for a shape from its J3D objects. Returns false if the
// descriptor/format objects are missing or malformed.
bool build_cp(u32 sh, NgxCP& cp) {
    const u32 desc  = r32(sh + 0x2C);          // GXVtxDescList*
    const u32 vdata = r32(sh + 0x44);          // J3DVertexData*
    if (!valid(desc) || !valid(vdata)) return false;
    const u32 fmt = r32(vdata + 0x0C);         // GXVtxAttrFmtList*
    if (!valid(fmt)) return false;
    const bool nbt = rb(sh + 0x30) & 1;

    // VCD from the descriptor list.
    for (u32 e = desc; e < desc + 0x200; e += 8) {
        const u32 attr = r32(e), type = r32(e + 4);
        if (attr == 0xFF) break;
        if (attr == 0)               cp.vcd_lo |= (type ? 1u : 0u) << 0;       // PNMTXIDX
        else if (attr <= 8)          cp.vcd_lo |= (type ? 1u : 0u) << attr;    // TEXiMTXIDX
        else if (attr == 9)          cp.vcd_lo |= (type & 3) << 9;             // POS
        else if (attr == 10)         cp.vcd_lo |= (type & 3) << 11;            // NRM
        else if (attr == 11)         cp.vcd_lo |= (type & 3) << 13;            // CLR0
        else if (attr == 12)         cp.vcd_lo |= (type & 3) << 15;            // CLR1
        else if (attr >= 13 && attr <= 20) cp.vcd_hi |= (type & 3) << (2 * (attr - 13));  // TEX
    }

    // VAT (+ array strides via attribute formats) from the format list.
    u32 pos_type = 4, nrm_type = 4, tex_type[8] = {4,4,4,4,4,4,4,4};
    u32& A = cp.vat[0][0]; u32& B = cp.vat[0][1]; u32& C = cp.vat[0][2];
    for (u32 e = fmt; e < fmt + 0x400; e += 16) {
        const u32 attr = r32(e), cnt = r32(e + 4), type = r32(e + 8), frac = rb(e + 12) & 0x1f;
        if (attr == 0xFF) break;
        switch (attr) {
        case 9:  A |= (cnt & 1) | ((type & 7) << 1) | (frac << 4); pos_type = type; break;
        case 10: A |= ((cnt ? 1u : 0u) << 9) | ((type & 7) << 10); if (cnt == 2) A |= 1u << 31;
                 nrm_type = type; break;
        case 11: A |= ((cnt & 1) << 13) | ((type & 7) << 14); break;
        case 12: A |= ((cnt & 1) << 17) | ((type & 7) << 18); break;
        case 13: A |= ((cnt & 1) << 21) | ((type & 7) << 22) | (frac << 25); tex_type[0] = type; break;
        case 14: B |= (cnt & 1) | ((type & 7) << 1) | (frac << 4);            tex_type[1] = type; break;
        case 15: B |= ((cnt & 1) << 9)  | ((type & 7) << 10) | (frac << 13);  tex_type[2] = type; break;
        case 16: B |= ((cnt & 1) << 18) | ((type & 7) << 19) | (frac << 22);  tex_type[3] = type; break;
        case 17: B |= ((cnt & 1) << 27) | ((type & 7) << 28); C |= frac;      tex_type[4] = type; break;
        case 18: C |= ((cnt & 1) << 5)  | ((type & 7) << 6)  | (frac << 9);   tex_type[5] = type; break;
        case 19: C |= ((cnt & 1) << 14) | ((type & 7) << 15) | (frac << 18);  tex_type[6] = type; break;
        case 20: C |= ((cnt & 1) << 23) | ((type & 7) << 24) | (frac << 27);  tex_type[7] = type; break;
        default: break;
        }
    }

    // Live vertex-array bases + strides. loadVtxArray() overrides pos/nrm/clr0 with j3dSys's
    // per-view buffers (unk10C/110/114), BUT only when those are set: for STATIC map geometry
    // the per-view CLR0 buffer (unk114) is NULL and GX keeps the static BMD colour array baked
    // by makeVtxArrayCmd (J3DVertexData::mVtxColorArray[0] @ vdata+0x1C; CLR1 = [1] @ +0x20).
    // Using unk114 unconditionally made indexed CLR0 resolve to null → white → washed-out floor/
    // buildings/sky (vertex-coloured map geometry). Fall back to the static array when null.
    cp.array_base[0] = r32(J3DSYS + 0x10C);  cp.array_stride[0] = (pos_type == 4) ? 12 : 6;
    cp.array_base[1] = nbt ? r32(vdata + 0x18) : r32(J3DSYS + 0x110);
    cp.array_stride[1] = ((nrm_type == 4) ? 12 : 6) * (nbt ? 3 : 1);
    // CLR0 array — read it from the J3D OBJECT MODEL, not Dolphin's emulated CP state. The
    // decomp's J3DShape::draw always runs loadVtxArray() → J3DLoadArrayBasePtr(GX_VA_CLR0,
    // j3dSys.unk114) AFTER GXCallDisplayList replays the static array, so the array GX actually
    // samples for the draw primitives is j3dSys.unk114 (J3DSYS+0x114) — exactly parallel to POS
    // (unk10C) and NRM (unk110) above. makeVtxArrayCmd bakes the static authored colours
    // (mVtxColorArray[0] @ vdata+0x1C, stride 4) as the fallback when unk114 is null.
    // (Reading g_main_cp_state[Color0] was a GameCube-emulation crutch and returned a STALE,
    // WRONG array — a texcoord-like base decoding to bright magenta where the real per-view
    // colours are neutral; verified via [magsrc]: unk114=80b9b600 neutral, cp=80d39c40 magenta.)
    const u32 clr0 = r32(J3DSYS + 0x114);
    cp.array_base[2] = valid(clr0) ? clr0 : r32(vdata + 0x1C);
    cp.array_stride[2] = 4;
    // Latch CLR0 capture state for /ngxshapes per-input inspection (the haze-wash probe):
    // class (0=none→white default), VAT format, and the array base ngx actually samples.
    g_cur_clr0cls  = (cp.vcd_lo >> 13) & 3;
    g_cur_clr0fmt  = (cp.vat[0][0] >> 14) & 7;
    g_cur_clr0base = cp.array_base[2];
    g_cur_nrmcls   = (cp.vcd_lo >> 11) & 3;
    cp.array_base[3] = r32(vdata + 0x20);    cp.array_stride[3] = 4;
    for (int i = 0; i < 8; i++) {
        cp.array_base[4 + i]   = r32(vdata + 0x24 + i * 4);
        cp.array_stride[4 + i] = (tex_type[i] == 4) ? 8 : 4;
    }
    g_cur_tex0cls = (cp.vcd_hi >> 0) & 3; g_cur_tex1cls = (cp.vcd_hi >> 2) & 3;
    g_cur_tex0base = cp.array_base[4]; g_cur_tex1base = cp.array_base[5];
    g_cur_tex0type = tex_type[0]; g_cur_tex1type = tex_type[1];
    g_cur_tex0frac = (cp.vat[0][0] >> 25) & 0x1f; g_cur_tex1frac = (cp.vat[0][1] >> 4) & 0x1f;
    return true;
}

// ── Published stats (read best-effort by /ngxshape from the HTTP thread; the
//    emu/render thread is the only writer, so torn reads at worst misreport a
//    counter by one — acceptable for a diagnostic) ─────────────────────────────
unsigned long g_calls = 0, g_meshes = 0, g_fail = 0, g_badcp = 0;
unsigned long g_total_verts = 0, g_total_tris = 0;
unsigned      g_last_verts = 0, g_last_tris = 0, g_max_verts = 0;
u32           g_last_vcdlo = 0, g_last_vcdhi = 0, g_last_vstride = 0;
float         g_last_pos[3] = {0, 0, 0};
// Native XF (vertex-transform) verification: model-space positions transformed by
// the game's modelview (j3dSys.mCurrentDrawMtx) → eye space. On-screen geometry
// faces the camera (GC camera looks down −Z), so eye.z<0 is "in front".
unsigned long g_xf_total = 0, g_xf_front = 0, g_xf_nomtx = 0;
float         g_last_eye[3] = {0, 0, 0};
float         g_eye_min[3] = {0, 0, 0}, g_eye_max[3] = {0, 0, 0};
// Native projection stage: the latest perspective projection matrix (4x4 row-
// major, as the game passes to GXSetProjection), published by the scene_render
// GXSetProjection tee. eye → clip = P·(eye,1) → NDC = clip.xyz/clip.w. On-screen
// geometry has clip.w>0 (in front of the near plane) and NDC x,y in [-1,1].
float         g_proj[16] = {0};
bool          g_have_proj = false;
unsigned      g_proj_type = 0;     // 0=perspective, >0=orthographic (the live projection class)
unsigned      g_proj_pass = 0;     // monotone per-frame projection-pass index (reset at publish; /ngxshapes)
// EFB-copy "epoch" — render-target awareness for the present. The game renders some passes to an
// OFFSCREEN texture (GXCopyTex) and others to the DISPLAY (GXCopyDisp). ngx currently composites
// EVERY captured J3D shape into the present regardless of where the game routed it → the file-select
// "floating Mario ghost" (Mario drawn in BOTH an offscreen pass and the display pass; GX presents
// only the display one). g_efb_epoch increments at each EFB copy; each shape/batch records the epoch
// it drew under; g_epoch_tex[buf][e]=true marks epoch e as closed by a GXCopyTex (offscreen). At
// present, a batch whose epoch is tex-closed is offscreen and must be discarded.
constexpr int EPOCH_CAP = 64;
unsigned      g_efb_epoch = 0;
bool          g_epoch_tex[2][EPOCH_CAP] = {{false}};   // per-frame: epoch closed by GXCopyTex?
// Per-published-buffer DISPLAY epoch = the highest tex-closed epoch (the main scene's offscreen
// render). Geometry in lower epochs is auxiliary (reflections/shadows/file-slot thumbnails) that
// the game samples as a texture, NOT composites directly — ngx must not draw it into the present
// (the file-select "floating Mario ghost"). Computed at publish, read by the present filter.
int           g_display_epoch[2] = {0, 0};
// Copy-event log (per frame, into g_cur buffer) for the /ngxshapes diagnostic: (kind, epoch, pass).
struct CopyEvt { unsigned char kind; unsigned epoch; unsigned pass; unsigned char clear; unsigned gen; };  // kind: 0=Tex 1=Disp
std::vector<CopyEvt> g_copyevt[2];
// ── CLEAR-AWARE GENERATION model (handoff gap #3) ───────────────────────────────────────────────
// The epoch model above splits the EFB at EVERY copy and displays only the highest tex-closed
// epoch — which DROPS layers GX accumulated. The GPU truth: the EFB accumulates draws and is only
// wiped by a clearing copy (GXCopyTex/Disp with clear=1). So draws between clears form ONE blend
// stack ("generation"); a non-clearing copy snapshots but KEEPS the EFB (accumulation continues).
// g_efb_gen = count of clearing copies so far this frame; shapes/batches record the gen they drew
// under. The DISPLAYED stack = the generation active at the GXCopyDisp (XFB) — NOT the highest
// tex-closed epoch. This is a DIAGNOSTIC first (surfaced in /efbcopies + /ngxshapes); the present
// filter still uses display_epoch until the data confirms the switch.
unsigned g_efb_gen = 0;          // current generation (reset per frame, ++ AFTER a clearing copy)
int      g_display_gen[2] = {-1, -1};   // per-published-buffer: gen active at the CopyDisp (XFB)
// Sky-shape (cc==0x0701, the vtx-color gradient) transform latch: the modelview + projection
// + eye/clip of its FIRST vertex, captured at its draw — to diagnose the mis-projection.
struct SkyXf { bool have=false; float M[12]={0}; float P[16]={0}; float pos0[3]={0};
               float eye0[3]={0}; float clip0[4]={0}; u32 mtxp=0; bool pnmtx=false; size_t nv=0; };
SkyXf g_skyxf, g_skyxf_pub;
struct PnmtxDbg { bool have=false; size_t nv=0; unsigned char mi0=0; bool mp_valid=false;
    float row[12]={0}; float hook[12]={0}; float pos0[3]={0}; int n_distinct=0; unsigned char mi_min=255,mi_max=0; };
PnmtxDbg g_pnmtxdbg, g_pnmtxdbg_pub;
unsigned long g_ndc_total = 0, g_ndc_wpos = 0, g_ndc_inbox = 0;
// Near-plane clip stats (per frame, reset at publish): tris in, tris dropped wholly-behind,
// tris clipped (straddling), and the min clip.w among EMITTED verts (spikes ⇒ tiny |min_w|).
unsigned long g_clip_in=0, g_clip_drop=0, g_clip_cut=0, g_clip_tiny=0; float g_clip_minw=1e30f;

// Clip-space triangle SNAPSHOT for the native Vulkan mesh render (vk_mesh.cpp /
// ngx_present.cpp): one frame's scene triangles + per-texture draw batches.
//
// DOUBLE-BUFFERED per frame. The game thread accumulates the current frame into
// the `g_cur` buffer; at each frame boundary (GXSetProjection, ngx_frame_begin)
// the just-completed buffer is published to `g_front` (atomic) and accumulation
// flips to the other buffer. The present (video thread) always reads the last
// COMPLETE frame from `g_front` — never a half-accumulated one. Before this, a
// single live buffer was read mid-accumulation/mid-wrap, so a present that landed
// just after the buffer wrapped showed a partial scene (the plaza floor missing,
// or near-empty) → intermittent black frames.
constexpr size_t SNAP_CAP  = 600000;   // vertices (200k tris) per buffer
constexpr size_t BATCH_CAP = 8192;     // draw batches per buffer
std::vector<NgxRenderVertex> g_snap[2];   // SNAP_CAP entries each, lazily sized
size_t                       g_snap_count[2] = {0, 0};
std::vector<NgxRenderBatch>  g_batches[2];
int                          g_cur = 0;       // accumulation buffer (game thread only)
std::atomic<int>             g_front{0};       // published buffer (present reads)
// interp60 (PC-native frame interpolation): the displaced frame (N-1) preserved at publish so the
// present can render an in-between (clip-space lerp N-1↔N). Double-buffered + atomic-indexed for the
// CPU(publish)→video(present) handoff; copied only when interp60 is on.
std::vector<NgxRenderVertex> g_snap_prev[2];
std::vector<NgxRenderBatch>  g_batches_prev[2];
size_t                       g_snap_prev_count[2] = {0, 0};
std::atomic<int>             g_prev_front{-1};       // published prev buffer (-1 = none yet)
int                          g_prev_cur = 0;         // prev accumulation index (publish thread)
int                          g_prev_read_front = -1; // latched by ngx_snap_verts_prev
std::atomic<int>             g_ngx_interp_mode{0};   // 0 = render front (real), 1 = render N-1↔N blend
std::atomic<int>             g_ngx_interp_alpha_milli{500};  // blend alpha ×1000 (default 0.5)
// Per-shape NDC-bbox record ring (double-buffered like g_snap) — for the /ngxshapes probe that
// localizes a MISPLACED shape (e.g. the file-select Mario rendered at screen-top): a shape whose
// NDC bbox sits where it shouldn't is an identified wrong-matrix shape, not a shred. Recorded at
// the end of transform_eye, cleared/flipped with the snapshot at the frame boundary.
struct ShapeRec {
    u32 sh; unsigned nv; u16 cc; u32 tex0; unsigned char pnmtx; unsigned single_idx;
    float nxmin, nxmax, nymin, nymax;   // NDC bbox over w>eps verts
    float wmin, wmax; unsigned nfront;  // clip-w range + count of w>eps verts
    float tx, ty, tz, det3;             // single-matrix (pnmtx=0) modelview translation + 3x3 det
    unsigned pass; unsigned char projtype;   // projection-pass index this shape drew under + ortho?
    unsigned epoch;                     // EFB-copy epoch this shape drew under (offscreen vs display)
    unsigned gen;                       // clear-aware generation (blend-stack) this shape drew under
    int ti;                             // tev_index (material) — filter haze layers by this
    unsigned clr0cls, clr0fmt; u32 clr0base;  // CLR0 VCD class / VAT fmt / array base ngx samples
    float nrm0[3]; unsigned nrmcls;     // first-vertex MODEL normal + NRM VCD class (lighting sanity)
    unsigned char clr0r, clr0g, clr0b, clr0a; // MEAN decoded per-vertex CLR0 (0..255) — the raster ngx feeds
    unsigned char clr0min, clr0max;     // min/max per-vertex luminance (0..255) — flat-white vs gradient
    float proj[16];                     // the projection matrix this shape drew under (per-pass depth-space)
    float zmin, zmax;                   // NDC-z (clip.z/clip.w) range over w>eps verts (depth ordering)
    float ezmin, ezmax;                 // eye-space Z range (camera distance; -ve = in front) — depth-source
    float mv[12];                       // full single-matrix modelview (drawMtx[single_idx], row-major 3x4)
    float mp0[3], mp1[3];               // model-space positions of vert0/vert1 (pre-transform) — foam vs sea cmp
    u32 material, cb_addr, cb_vt, matpkt;  // J3DMaterial / colorBlock addr+vtable / live MatPacket (baked DL)
    u16 alpha0;                          // captured ALPHA0 chanctrl
    unsigned char mat[4], amb[4];        // captured COLOR0 matColor / ambColor (the lighting inputs)
};
std::vector<ShapeRec> g_shaperec[2];
int                          g_read_front = 0; // latched by ngx_snap_verts for batches
unsigned long                g_frame_swaps = 0;
std::atomic<unsigned long>   g_ngx_front_frame{0};   // frame id of the published snapshot (abshot2 liveness)

// Reusable scratch (single emu/render thread serialized by nthr).
std::vector<NgxVertex> g_verts;
std::vector<unsigned>  g_indices;
std::vector<float>     g_clip;        // 4 floats/vertex (clip-space), scratch
std::vector<float>     g_litrgba;     // 4 floats/vertex (lit raster colour0), scratch
std::vector<float>     g_litrgba1;    // 4 floats/vertex (lit raster COLOR1A1), scratch
std::vector<float>     g_uvs;         // 16 floats/vertex (8 texgen'd UVs), scratch

// Read the current material's TEV block into an NgxTevState, dedupe it into the
// table, and return its index (-1 if no material / unknown block variant). Reads
// the guest object straight from RAM (big-endian bytes via sb_ram_fast), object-
// model — no GX byte-stream decode.
int capture_material() {
    g_cur_chan = ChanInfo{};                           // reset; stays invalid if no material
    g_cur_texgen = TexGenSet{};                        // reset texgen (num=0 → raw-attr fallback)
    for (int m = 0; m < 8; m++) g_mat_tex[m] = NgxTexBind{};   // reset textures
    const u32 matpacket = r32(J3DSYS + 0x3C);          // j3dSys.mMatPacket
    if (!valid(matpacket)) { g_mat_none++; return -1; }
    g_cur_matpkt = matpacket;                           // for /shapeat baked-DL walk (DL@+0x20, len@+0x24)
    const u32 material = r32(matpacket + 0x38);        // J3DMatPacket::unk38
    if (!valid(material)) { g_mat_none++; return -1; }
    capture_colorchan(material);                       // N6: colour-channel/lighting state
    capture_texgen(material);                          // diag: texgen src/type scope
    const u32 tevblock = r32(material + 0x28);         // J3DMaterial::mTevBlock
    if (!valid(tevblock)) { g_mat_none++; return -1; }
    const u32 vt = r32(tevblock + 0x00);               // J3DTevBlock vtable ptr

    // Vtable histogram (verification) — record up to 8 distinct values.
    for (int i = 0; i < 8; i++) {
        if (g_vt_hist_key[i] == vt) { g_vt_hist_cnt[i]++; break; }
        if (g_vt_hist_key[i] == 0) { g_vt_hist_key[i] = vt; g_vt_hist_cnt[i] = 1; break; }
    }

    capture_textures(tevblock, vt);                    // N6.7: object-model per-texmap textures

    // Indirect-texturing reachability probe (tooling-first gate before porting it).
    {
        const u32 indb = r32(material + 0x2C);         // J3DMaterial::mIndBlock
        if (!valid(indb)) { g_ind_none++; }
        else {
            const u32 itag = pe_block_type(r32(indb + 0x00));   // getType FourCC at vtable+0x10
            if (itag == IND_FULL) {
                const u8* IB = sb_ram_fast(indb);
                const u8 nstg = IB ? IB[0x04] : 0;     // mIndTexStageNum
                g_ind_full++;
                if (nstg <= 4) g_ind_stage_hist[nstg]++;
                if (nstg > g_ind_stages_max) g_ind_stages_max = nstg;
            } else if (itag == IND_NULL) { g_ind_null++; }
            else { g_ind_unk++; }
        }
    }

    TevLayout L;
    if (!tev_layout(vt, L)) { g_mat_novt++; return -1; }
    const u8* B = sb_ram_fast(tevblock);               // raw big-endian bytes
    if (!B) { g_mat_none++; return -1; }
    auto rb8  = [&](u32 o) -> u8  { return B[o]; };
    auto rs16 = [&](u32 o) -> int16_t { return (int16_t)((B[o] << 8) | B[o + 1]); };

    NgxTevState st{};
    u8 ns = L.stagenum_off ? rb8(L.stagenum_off) : 1;
    if (ns < 1) ns = 1; if (ns > 16) ns = 16;
    st.num_stages = ns;
    for (int s = 0; s < ns; s++) {
        const u32 so = L.stage_off + s * 8;            // J3DTevStage (8 bytes)
        // color_env = [mTevColorOp:mTevColorAB:mTevColorCD] (low 24 bits of the BP word)
        st.stage[s].color_env = ((u32)rb8(so + 1) << 16) | ((u32)rb8(so + 2) << 8) | rb8(so + 3);
        // alpha_env = [mTevAlphaOp:mTevAlphaAB:mTevSwapModeInfo]
        st.stage[s].alpha_env = ((u32)rb8(so + 5) << 16) | ((u32)rb8(so + 6) << 8) | rb8(so + 7);
        const u32 oo = L.order_off + s * 4;            // J3DTevOrder (4 bytes)
        st.stage[s].texcoord   = rb8(oo + 0);
        st.stage[s].texmap     = rb8(oo + 1);
        st.stage[s].color_chan = rb8(oo + 2);
        st.stage[s].kcsel = L.kcsel_off ? rb8(L.kcsel_off + s) : 0x0C;  // GX_TEV_KCSEL_1 default
        st.stage[s].kasel = L.kasel_off ? rb8(L.kasel_off + s) : 0x1C;  // GX_TEV_KASEL_1 default
    }
    // 4 TEV color registers (CPREV/C0/C1/C2), S10 RGBA — absent on TVB1 (defaults 0).
    if (L.tevcolor_off)
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++) st.tev_color[c][k] = rs16(L.tevcolor_off + c * 8 + k * 2);
    // 4 KONST color registers, u8 RGBA — absent on TVB1 (defaults 255 = white).
    if (L.kcolor_off) {
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++) st.kcolor[c][k] = rb8(L.kcolor_off + c * 4 + k);
    } else {
        std::memset(st.kcolor, 0xFF, sizeof st.kcolor);
    }
    // 4 TEV swap tables (J3DTevSwapModeTable::mIdx, 1 byte each) — absent on TVB1 (identity).
    if (L.swaptable_off)
        for (int t = 0; t < 4; t++) st.swap_table[t] = rb8(L.swaptable_off + t);
    else
        st.swap_table[0] = st.swap_table[1] = st.swap_table[2] = st.swap_table[3] = 0x1B;  // identity

    capture_pe(material, st);   // N7: PE block (alpha test → shader, blend/zmode → pipeline)
    st.pe.cull = g_cur_chan.cullMode;   // backface culling (color block) → pipeline cull state
    capture_indirect(material, tevblock, L, st);   // indirect texturing (texcoord warp)

    // FNV-1a key over the captured state (excluding the key field itself).
    uint64_t h = 1469598103934665603ull;
    const u8* p = (const u8*)&st;
    for (size_t i = 0; i < offsetof(NgxTevState, key); i++) { h ^= p[i]; h *= 1099511628211ull; }
    st.key = h;

    if (ns <= 16) g_stage_hist[ns]++;
    g_mat_found++;

    // /gxstate: when this material is the target, snapshot the GX-command-stream state (the
    // game's actual writes, live at this draw) alongside ngx's object-model reconstruction.
    auto gxstate_snap = [&](int ti){
        if (g_gxstate_ti.load() != ti || g_gxstate.have || !g_cur_chan.valid) return;
        GxStateRec& R = g_gxstate; R = GxStateRec{}; R.have = true; R.ti = ti; R.sh = g_cur_shape;
        R.obj_cc = g_cur_chan.color0; R.obj_ca = g_cur_chan.alpha0;
        for (int k=0;k<4;k++){ R.obj_mat[k]=g_cur_chan.matColor[k]; R.obj_amb[k]=g_cur_chan.ambColor[k]; }
        R.gx_cc = g_gx_cc[0]; R.gx_cc_have = g_gx_cc_have[0]; R.gx_cc_sets = g_gx_cc_sets[0];
        for (int k=0;k<4;k++){ R.gx_mat[k]=g_gx_matcol[0][k]; R.gx_amb[k]=g_amb_reg[0][k]; }
        R.gx_mat_have = g_gx_matcol_have[0]; R.gx_mat_sets = g_gx_matcol_sets[0];
        R.gx_amb_have = g_amb_have[0]; R.gx_amb_sets = g_amb_sets;
        for (int k=0;k<4;k++) R.gd_amb[k]=g_gd_amb[0][k]; R.gd_amb_have=g_gd_amb_have[0]; R.gd_amb_sets=g_gd_amb_sets;
        R.xf_cc = (u32)xfmem.color[0].hex; R.xf_mat = xfmem.matColor[0]; R.xf_amb = xfmem.ambColor[0]; R.xf_have = true;
        R.cb_addr = g_cur_cb_addr; R.cb_vt = g_cur_cb_vt; for (int k=0;k<0x20;k++) R.cb_raw[k]=g_cur_cb_raw[k];
        R.tev = st; R.tev_have = true;
        R.tg = g_cur_texgen; for (int m=0;m<8;m++) R.tex[m] = g_mat_tex[m];
        R.tc0cls=(u8)g_cur_tex0cls; R.tc1cls=(u8)g_cur_tex1cls; R.tc0base=g_cur_tex0base; R.tc1base=g_cur_tex1base;
        R.tc0type=(u8)g_cur_tex0type; R.tc1type=(u8)g_cur_tex1type; R.tc0frac=(u8)g_cur_tex0frac; R.tc1frac=(u8)g_cur_tex1frac;
        // Snapshot the GPU's actual texmtx (xfmem matrix memory) for each texgen coord AT this draw
        // (authoritative in oracle mode) → compare vs ngx's object-model mTotalMtx in /gxstate.
        for (int i=0;i<R.tg.num && i<8;i++) { const u8 sel = R.tg.tg[i].mtxsel;
            if (sel >= 30 && sel < 60) { const float* X = &xfmem.posMatrices[sel*4];
                for (int k=0;k<8;k++) R.tg_xf[i][k] = X[k]; R.tg_xf_have[i] = true; } }
        R.obj_mask = (u8)(((R.obj_cc >> 2) & 0x0F) | (((R.obj_cc >> 11) & 0x0F) << 4));
        R.gx_mask  = (u8)(((R.gx_cc  >> 2) & 0x0F) | (((R.gx_cc  >> 11) & 0x0F) << 4));
        for (int i=0;i<8;i++){ R.lights[i].valid=g_light[i].valid;
            for (int k=0;k<3;k++){ R.lights[i].col[k]=g_light[i].color[k]; R.lights[i].pos[k]=g_light[i].pos[k];
                R.lights[i].dir[k]=g_light[i].dir[k]; R.lights[i].cosA[k]=g_light[i].cosA[k]; R.lights[i].distA[k]=g_light[i].distA[k]; } }
    };

    const u16 dbgcc = g_cur_chan.valid ? g_cur_chan.color0 : 0xFFFF;  // 0xFFFF = no colour block
    auto stash_chan = [&](int i){ if (i<0||i>=(int)TEVSTATE_CAP) return;
        g_tev_tb[i]=tevblock; g_tev_vt[i]=vt;
        if (!g_cur_chan.valid) return;
        g_tev_chan[i].have=true; g_tev_chan[i].alpha0=g_cur_chan.alpha0;
        for (int k=0;k<4;k++){ g_tev_chan[i].mat[k]=g_cur_chan.matColor[k]; g_tev_chan[i].amb[k]=g_cur_chan.ambColor[k]; } };
    auto it = g_tevkey_index.find(h);
    if (it != g_tevkey_index.end()) { if (it->second < (int)TEVSTATE_CAP) { g_tev_cc[it->second] = dbgcc; stash_chan(it->second); } gxstate_snap(it->second); return it->second; }
    if (g_tevstates.size() >= TEVSTATE_CAP) { g_tevstates.clear(); g_tevkey_index.clear(); }
    const int idx = (int)g_tevstates.size();
    g_tevstates.push_back(st);
    g_tevkey_index[h] = idx;
    if (idx < (int)TEVSTATE_CAP) { g_tev_cc[idx] = dbgcc; stash_chan(idx); }
    gxstate_snap(idx);
    return idx;
}

// Transform this shape's extracted model-space positions by the live modelview
// matrix (Mtx 3x4 at *j3dSys.mCurrentDrawMtx) and fold into the eye-space stats.
// This is the native XF stage; the matrix is the same one the recompiled J3D
// computed (the interp60 pos-matrix seam, now consumed natively).
void transform_eye() {
    const u32 mp = r32(J3DSYS + 0x104);     // mCurrentDrawMtx = draw-matrix ARRAY base
    float m[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};   // identity fallback
    // Single-matrix shape: draw-matrix index = the shape's J3DShapeMtx.unk4 (g_single_idx), NOT 0.
    if (valid(mp)) for (int i = 0; i < 12; i++) m[i] = rf(mp + g_single_idx * 48u + i * 4);
    else if (!g_cur_pnmtx) { g_xf_nomtx++; return; }   // non-multi-matrix shapes need the draw mtx
    // Normal matrix (Mtx33, row-major 3x3) for the native lighting stage. Absent →
    // skip lighting (en stays 0 → only ambient contributes, matched by the math).
    const u32 nmp = r32(J3DSYS + 0x108);    // mCurrentNormMtx (Mtx33*)
    const bool have_nm = valid(nmp);
    float nm[9]; if (have_nm) for (int i = 0; i < 9; i++) nm[i] = rf(nmp + i * 4);
    bool first = (g_xf_total == 0);
    const size_t nv = g_verts.size();
    // DIAG: latch the FIRST multi-matrix (PNMTXIDX) shape that is NOT the big sky dome — the
    // title logo / sun-rays / clouds are sheared while the single-matrix sky is fine. Capture
    // vert0's selected matrix (from g_posmtx) so we can see if the per-vertex matrices are stale
    // (never loaded this draw → identity-ish / leftover) vs a sane modelview.
    if (!g_pnmtxdbg.have && g_cur_pnmtx && nv >= 12 && nv < 4000) {
        g_pnmtxdbg.have = true; g_pnmtxdbg.nv = nv; g_pnmtxdbg.mp_valid = valid(mp);
        const unsigned char mi = g_verts[0].matidx; g_pnmtxdbg.mi0 = mi;
        for (size_t vi=0; vi<nv; vi++){ unsigned char x=g_verts[vi].matidx; if(x<g_pnmtxdbg.mi_min)g_pnmtxdbg.mi_min=x; if(x>g_pnmtxdbg.mi_max)g_pnmtxdbg.mi_max=x; }
        if ((unsigned)mi + 2 < 64) for (int r=0;r<3;r++) for(int c=0;c<4;c++) {
            g_pnmtxdbg.row[r*4+c]=xfmem.posMatrices[(mi+r)*4+c];   // GPU ground truth (xfmem)
            g_pnmtxdbg.hook[r*4+c]=g_posmtx[mi+r][c];              // our indexed-hook capture
        }
        for (int k=0;k<3;k++) g_pnmtxdbg.pos0[k]=g_verts[0].pos[k];
    }
    // Latch the sky gradient shape's transform (cc==0x0701, the big vtx-color mesh).
    bool latch_skyxf = false;
    if (!g_skyxf.have && g_cur_chan.valid && g_cur_chan.color0 == 0x0701 && nv > 600) {
        g_skyxf.have = true; latch_skyxf = true; g_skyxf.nv = nv; g_skyxf.mtxp = mp; g_skyxf.pnmtx = g_cur_pnmtx;
        for (int i = 0; i < 12; i++) g_skyxf.M[i] = m[i];
        for (int i = 0; i < 16; i++) g_skyxf.P[i] = g_proj[i];
        if (nv) for (int k = 0; k < 3; k++) g_skyxf.pos0[k] = g_verts[0].pos[k];
    }
    if (g_have_proj) g_clip.assign(nv * 4, 0.0f);
    g_litrgba.assign(nv * 4, 0.0f);
    g_litrgba1.assign(nv * 4, 0.0f);
    g_uvs.assign(nv * 16, 0.0f);
    const bool vtxdump_this = (g_cur_tev_index == g_gxstate_ti.load());
    if (vtxdump_this) { g_vtxdump.clear(); g_vtxdump_projtype = (unsigned char)g_proj_type;
                        for (int i=0;i<12;i++) g_vtxdump_mv[i]=m[i]; }
    static std::vector<float> s_eye;          // per-vertex eye pos (x,y,z) for the shred metric
    s_eye.assign(nv * 3, 0.0f);

    // ngx-vs-Dolphin geometry differential: for every distinct PNMTXIDX slot this
    // multi-matrix shape references, compare ngx's applied matrix (g_posmtx[slot])
    // against Dolphin's authoritative XF matrix memory (xfmem.posMatrices[slot]).
    // Records the worst divergence per slot — this is the oracle the shear is judged
    // against, in numbers, not by eye.
    if (g_ngxdiff && g_cur_pnmtx) {
        g_gdiff_mm_shapes++;
        bool seen[64] = {false};
        for (size_t vi = 0; vi < nv; vi++) {
            const unsigned slot = g_verts[vi].matidx;
            if (slot + 2 >= 64 || seen[slot]) continue;
            seen[slot] = true;
            float ngx[12], dol[12], maxd = 0.0f;
            for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) {
                const float a = g_posmtx[slot + r][c];
                const float b = xfmem.posMatrices[(slot + r) * 4 + c];
                ngx[r*4+c] = a; dol[r*4+c] = b;
                float d = a - b; if (d < 0) d = -d; if (d > maxd) maxd = d;
            }
            GeomDiffSlot& s = g_gdiff[slot];
            s.n++; s.src = g_posmtx_src[slot];
            if (maxd > 1e-3f) s.nmiss++;
            if (maxd >= s.maxd) { s.maxd = maxd; for (int i = 0; i < 12; i++) { s.ngx[i]=ngx[i]; s.dol[i]=dol[i]; } }
        }
    }

    for (size_t vi = 0; vi < nv; vi++) {
        const NgxVertex& v = g_verts[vi];

        const float x = v.pos[0], y = v.pos[1], z = v.pos[2];
        // Multi-matrix (skinned) shapes: select the per-vertex position matrix by the vertex's
        // PNMTXIDX (XF row = slot*3) AND the packet that drew it. A skinned shape draws in N packets,
        // each reloading XF slots 0,3,… from its own useMtxIndexTable, so the correct matrix is
        // g_pkt_mtx[vertex.packet][slot] (resolved per packet in capture()). g_posmtx (global
        // last-writer) and live xfmem.posMatrices[mi] are BOTH wrong for any but the last packet
        // (proven 2026-06-17: using either shreds Mario / radiates garbage). Fall back to g_posmtx
        // only if a packet slot wasn't resolved (e.g. non-Multi PNMTXIDX shape).
        const float* M = m;
        float mm[12];
        if (g_cur_pnmtx && (unsigned)v.matidx + 2 < 64 && g_ngx_mtxsrc != 2) {
            const int slot = v.matidx / 3;
            const bool use_pkt = g_ngx_mtxsrc == 0 && v.packet < NGX_MAX_PKT &&
                                 slot < NGX_MAX_SLOT && g_pkt_have[v.packet][slot];
            if (use_pkt) { M = g_pkt_mtx[v.packet][slot]; g_pkt_applied++; }
            else {
                for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) mm[r*4+c] = g_posmtx[v.matidx + r][c];
                M = mm; g_pkt_fallback++;
            }
            g_pnmtx_applied++; if (v.matidx) g_pnmtx_nz++;   // DBG: per-vertex-matrix path firing?
        }
        const float ex = M[0]*x + M[1]*y + M[2]*z  + M[3];
        const float ey = M[4]*x + M[5]*y + M[6]*z  + M[7];
        const float ez = M[8]*x + M[9]*y + M[10]*z + M[11];

        // Native colour-channel lighting → per-vertex raster colour0.
        float en[3] = {0, 0, 0};
        if (have_nm) {
            en[0] = nm[0]*v.nrm[0] + nm[1]*v.nrm[1] + nm[2]*v.nrm[2];
            en[1] = nm[3]*v.nrm[0] + nm[4]*v.nrm[1] + nm[5]*v.nrm[2];
            en[2] = nm[6]*v.nrm[0] + nm[7]*v.nrm[1] + nm[8]*v.nrm[2];
            const float l = std::sqrt(en[0]*en[0] + en[1]*en[1] + en[2]*en[2]);
            if (l > 1e-6f) { en[0]/=l; en[1]/=l; en[2]/=l; }
            g_nrm_len_sum += l; g_nrm_n++;
            if (g_cur_chan.valid && ((g_cur_chan.color0 >> 1) & 1)) {
                g_lit_nrm_sum += l; g_lit_nrm_n++;
                if (l < 0.1f) g_lit_nrm_zero++;
            }
        }
        const float eye[3] = { ex, ey, ez };
        s_eye[vi*3+0]=ex; s_eye[vi*3+1]=ey; s_eye[vi*3+2]=ez;
        const float vcol0[4] = { v.clr[0][0]/255.f, v.clr[0][1]/255.f,
                                 v.clr[0][2]/255.f, v.clr[0][3]/255.f };
        const float vcol1[4] = { v.clr[1][0]/255.f, v.clr[1][1]/255.f,
                                 v.clr[1][2]/255.f, v.clr[1][3]/255.f };
        light_vertex(eye, en, vcol0, vcol1, &g_litrgba[vi * 4], &g_litrgba1[vi * 4]);
        // GX texgen: compute each texcoord's UV AFTER lighting so the GX_TG_SRTG sources
        // (texcoord = the lit colour, e.g. the sky's ramp index) read this vertex's col0.
        // Texcoords without a texgen def fall back to the raw vertex tex attribute.
        float* uvp = &g_uvs[vi * 16];
        for (int m = 0; m < 8; m++) {
            if (m < g_cur_texgen.num) texgen_uv(g_cur_texgen.tg[m], v, &g_litrgba[vi * 4], &uvp[m * 2]);
            else { uvp[m * 2 + 0] = v.tex[m][0]; uvp[m * 2 + 1] = v.tex[m][1]; }
        }
        // SKY UV bbox: accumulate tc0/tc1 UV extents for the sky material (0x0686) to check
        // whether ngx samples the deep-blue band of the sky texture (wash diagnosis).
        if (g_cur_chan.valid && g_cur_chan.color0 == 0x0686) {
            for (int a=0;a<2;a++){ if(uvp[a]<g_sky.uv0min[a])g_sky.uv0min[a]=uvp[a];
                                   if(uvp[a]>g_sky.uv0max[a])g_sky.uv0max[a]=uvp[a];
                                   if(uvp[2+a]<g_sky.uv1min[a])g_sky.uv1min[a]=uvp[2+a];
                                   if(uvp[2+a]>g_sky.uv1max[a])g_sky.uv1max[a]=uvp[2+a]; }
            g_sky.uvn++;
        }
        // gxstate target UV capture: pin the cloud over-tiling (raw tex + texgen'd UV range).
        if (g_cur_tev_index == g_gxstate_ti.load()) {
            if (!g_uvdbg.have) { g_uvdbg.have = true;
                g_uvdbg.tex0[0]=v.tex[0][0]; g_uvdbg.tex0[1]=v.tex[0][1];
                g_uvdbg.tex1[0]=v.tex[1][0]; g_uvdbg.tex1[1]=v.tex[1][1];
                g_uvdbg.uv0[0]=uvp[0]; g_uvdbg.uv0[1]=uvp[1]; g_uvdbg.uv1[0]=uvp[2]; g_uvdbg.uv1[1]=uvp[3]; }
            for (int a=0;a<2;a++){ if(uvp[a]<g_uvdbg.u0min[a])g_uvdbg.u0min[a]=uvp[a];
                                   if(uvp[a]>g_uvdbg.u0max[a])g_uvdbg.u0max[a]=uvp[a];
                                   if(uvp[2+a]<g_uvdbg.u1min[a])g_uvdbg.u1min[a]=uvp[2+a];
                                   if(uvp[2+a]>g_uvdbg.u1max[a])g_uvdbg.u1max[a]=uvp[2+a]; }
            const float ca=vcol0[3], cr=vcol0[0];
            if(ca<g_uvdbg.ca_min)g_uvdbg.ca_min=ca; if(ca>g_uvdbg.ca_max)g_uvdbg.ca_max=ca;
            if(cr<g_uvdbg.cr_min)g_uvdbg.cr_min=cr; if(cr>g_uvdbg.cr_max)g_uvdbg.cr_max=cr;
            g_uvdbg.nv++;
        }
        // DBG: bucket the resulting col0 luminance by category to localize the bright bulk.
        { const float* o = &g_litrgba[vi*4]; double lum=(o[0]+o[1]+o[2])/3.0;
          const bool valid=g_cur_chan.valid; const bool mv=valid&&((g_cur_chan.color0>>0)&1);
          const bool en2=valid&&((g_cur_chan.color0>>1)&1);
          int cat = !valid ? 4 : (mv?2:0)+(en2?1:0);   // 0 reg/flat,1 reg/lit,2 vtx/flat,3 vtx/lit,4 noblock
          g_colcat_sum[cat]+=lum; g_colcat_n[cat]++; }

        g_xf_total++;
        if (ez < 0.0f) g_xf_front++;
        if (first) { g_eye_min[0]=g_eye_max[0]=ex; g_eye_min[1]=g_eye_max[1]=ey;
                     g_eye_min[2]=g_eye_max[2]=ez; first=false; }
        else { if(ex<g_eye_min[0])g_eye_min[0]=ex; if(ex>g_eye_max[0])g_eye_max[0]=ex;
               if(ey<g_eye_min[1])g_eye_min[1]=ey; if(ey>g_eye_max[1])g_eye_max[1]=ey;
               if(ez<g_eye_min[2])g_eye_min[2]=ez; if(ez>g_eye_max[2])g_eye_max[2]=ez; }
        g_last_eye[0]=ex; g_last_eye[1]=ey; g_last_eye[2]=ez;

        // Native projection: clip = P·(eye,1); NDC = clip.xyz / clip.w.
        // Pure, unit-tested in sunbright-render-test (test_projection) — keep this
        // path a thin call so the tested math IS the shipping math.
        if (g_have_proj) {
            float* cp = &g_clip[vi * 4];
            ngx_project_eye(g_proj, ex, ey, ez, cp);
            const float cx = cp[0], cy = cp[1], cz = cp[2], cw = cp[3];
            if (latch_skyxf && vi == 0) { g_skyxf.eye0[0]=ex; g_skyxf.eye0[1]=ey; g_skyxf.eye0[2]=ez;
                g_skyxf.clip0[0]=cx; g_skyxf.clip0[1]=cy; g_skyxf.clip0[2]=cz; g_skyxf.clip0[3]=cw; }
            g_ndc_total++;
            float nx, ny;
            if (ngx_ndc_xy(cp, nx, ny)) {
                g_ndc_wpos++;
                if (nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f) g_ndc_inbox++;
            }
            if (vtxdump_this && g_vtxdump.size() < 4096) {
                const float* uvp = &g_uvs[vi * 16];
                g_vtxdump.push_back({cw>1e-6f?cx/cw:0.f, cw>1e-6f?cy/cw:0.f,
                                     g_litrgba[vi*4+3], uvp[0], uvp[1], uvp[2], uvp[3],
                                     ex, ey, ez, cw, cx, cy, cz});
            }
        }
    }

    // Per-shape NDC bbox record (for /ngxshapes — localize a misplaced shape by where it lands).
    if (g_have_proj && !g_clip.empty()) {
        ShapeRec rec{};
        rec.sh = g_cur_shape; rec.nv = (unsigned)nv;
        rec.cc = g_cur_chan.valid ? g_cur_chan.color0 : 0xFFFF;
        rec.tex0 = g_mat_tex[0].addr; rec.pnmtx = g_cur_pnmtx ? 1 : 0; rec.single_idx = g_single_idx;
        rec.tx = m[3]; rec.ty = m[7]; rec.tz = m[11];
        rec.det3 = m[0]*(m[5]*m[10]-m[6]*m[9]) - m[1]*(m[4]*m[10]-m[6]*m[8]) + m[2]*(m[4]*m[9]-m[5]*m[8]);
        for (int i = 0; i < 12; i++) rec.mv[i] = m[i];
        rec.mp0[0]=nv?g_verts[0].pos[0]:0; rec.mp0[1]=nv?g_verts[0].pos[1]:0; rec.mp0[2]=nv?g_verts[0].pos[2]:0;
        rec.mp1[0]=nv>1?g_verts[1].pos[0]:0; rec.mp1[1]=nv>1?g_verts[1].pos[1]:0; rec.mp1[2]=nv>1?g_verts[1].pos[2]:0;
        rec.pass = g_proj_pass; rec.projtype = (unsigned char)g_proj_type;
        for (int i = 0; i < 16; i++) rec.proj[i] = g_proj[i];
        { float zlo = 1e30f, zhi = -1e30f, ezlo = 1e30f, ezhi = -1e30f;
          for (size_t vi = 0; vi < nv; vi++) { const float* c = &g_clip[vi*4];
              if (c[3] > 1e-4f) { float z = c[2]/c[3]; if (z<zlo) zlo=z; if (z>zhi) zhi=z; }
              float ez = s_eye[vi*3+2]; if (ez<ezlo) ezlo=ez; if (ez>ezhi) ezhi=ez; }
          rec.zmin = zlo; rec.zmax = zhi; rec.ezmin = ezlo; rec.ezmax = ezhi; }
        rec.epoch = g_efb_epoch;
        rec.gen = g_efb_gen;
        rec.ti = g_cur_tev_index;
        rec.material = g_cur_material; rec.cb_addr = g_cur_cb_addr; rec.cb_vt = g_cur_cb_vt;
        rec.matpkt = g_cur_matpkt;
        rec.alpha0 = g_cur_chan.valid ? g_cur_chan.alpha0 : 0xFFFF;
        for (int k=0;k<4;k++){ rec.mat[k]=g_cur_chan.matColor[k]; rec.amb[k]=g_cur_chan.ambColor[k]; }
        rec.clr0cls = g_cur_clr0cls; rec.clr0fmt = g_cur_clr0fmt; rec.clr0base = g_cur_clr0base;
        rec.nrmcls = g_cur_nrmcls;
        rec.nrm0[0] = nv ? g_verts[0].nrm[0] : 0; rec.nrm0[1] = nv ? g_verts[0].nrm[1] : 0; rec.nrm0[2] = nv ? g_verts[0].nrm[2] : 0;
        // MEAN + min/max luminance of the per-vertex CLR0 ngx actually feeds the raster (after
        // decode/default). Flat-white haze (mean 255, min==max 255) vs a real gradient is the tell.
        { unsigned long sr=0,sg=0,sb=0,sa=0; unsigned lo=255,hi=0;
          for (size_t vi=0; vi<nv; vi++) { const unsigned char* c=g_verts[vi].clr[0];
              sr+=c[0]; sg+=c[1]; sb+=c[2]; sa+=c[3];
              unsigned l=(c[0]+c[1]+c[2])/3; if(l<lo)lo=l; if(l>hi)hi=l; }
          if (nv) { rec.clr0r=(unsigned char)(sr/nv); rec.clr0g=(unsigned char)(sg/nv);
                    rec.clr0b=(unsigned char)(sb/nv); rec.clr0a=(unsigned char)(sa/nv);
                    rec.clr0min=(unsigned char)lo; rec.clr0max=(unsigned char)hi; } }
        rec.nxmin = rec.nymin = 1e30f; rec.nxmax = rec.nymax = -1e30f;
        rec.wmin = 1e30f; rec.wmax = -1e30f; rec.nfront = 0;
        for (size_t vi = 0; vi < nv; vi++) {
            const float w = g_clip[vi*4+3]; if (w <= 1e-3f) continue;
            const float nx = g_clip[vi*4]/w, ny = g_clip[vi*4+1]/w;
            if (nx<rec.nxmin)rec.nxmin=nx; if(nx>rec.nxmax)rec.nxmax=nx;
            if (ny<rec.nymin)rec.nymin=ny; if(ny>rec.nymax)rec.nymax=ny;
            if (w<rec.wmin)rec.wmin=w; if(w>rec.wmax)rec.wmax=w; rec.nfront++;
        }
        if (rec.nfront > 0 && g_shaperec[g_cur].size() < 2048) g_shaperec[g_cur].push_back(rec);
    }

    // Skinned-shred metric: max eye-space edge over THIS skinned shape's own triangles.
    // A coherent character keeps every intra-shape edge ≲ its real size; a per-vertex matrix
    // mistake flings verts apart → a giant edge. Records the session worst (+ where) and a
    // per-shape bucket histogram. Skip the sky/single-matrix shapes (g_cur_pnmtx gate).
    if (g_cur_pnmtx && !g_indices.empty()) {
        float worst = 0.f; unsigned wp = 0; unsigned char wm0 = 0, wm1 = 0;
        for (size_t t = 0; t + 3 <= g_indices.size(); t += 3) {
            for (int e = 0; e < 3; e++) {
                const unsigned a = g_indices[t + e], b = g_indices[t + (e + 1) % 3];
                if (a >= nv || b >= nv) continue;
                const float dx = s_eye[a*3]-s_eye[b*3], dy = s_eye[a*3+1]-s_eye[b*3+1], dz = s_eye[a*3+2]-s_eye[b*3+2];
                const float d = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (d > worst) { worst = d; wp = g_verts[a].packet; wm0 = g_verts[a].matidx; wm1 = g_verts[b].matidx; }
            }
        }
        g_shred_last = worst;
        g_shred_n[worst < 500.f ? 0 : worst < 2000.f ? 1 : worst < 10000.f ? 2 : 3]++;
        if (worst > g_shred_max) { g_shred_max = worst; g_shred_shape = g_cur_shape;
                                   g_shred_pkt = wp; g_shred_mi0 = wm0; g_shred_mi1 = wm1; }
        // NDC (screen) edge over front-facing triangles (catches projection/clip-induced shred
        // that eye-space coherence hides). g_clip holds clip[4] per vertex; NDC = xy/w.
        if (g_have_proj && !g_clip.empty()) {
            float nworst = 0.f; unsigned nva = 0;
            for (size_t t = 0; t + 3 <= g_indices.size(); t += 3) {
                const unsigned i0=g_indices[t], i1=g_indices[t+1], i2=g_indices[t+2];
                if (i0>=nv||i1>=nv||i2>=nv) continue;
                const float w0=g_clip[i0*4+3], w1=g_clip[i1*4+3], w2=g_clip[i2*4+3];
                if (w0<1e-3f||w1<1e-3f||w2<1e-3f) continue;     // straddler → clipper handles it
                const unsigned ii[3]={i0,i1,i2};
                for (int e=0;e<3;e++){ const unsigned a=ii[e], b=ii[(e+1)%3];
                    const float dx=g_clip[a*4]/g_clip[a*4+3]-g_clip[b*4]/g_clip[b*4+3];
                    const float dy=g_clip[a*4+1]/g_clip[a*4+3]-g_clip[b*4+1]/g_clip[b*4+3];
                    const float d=std::sqrt(dx*dx+dy*dy);
                    if(d>nworst){ nworst=d; nva = (g_clip[a*4+3] < g_clip[b*4+3]) ? a : b; } }
            }
            if (nworst>0.f) g_shred_ndc_n[nworst<2.f?0:nworst<8.f?1:nworst<40.f?2:3]++;
            if (g_shred_freeze_thresh>0.f && nworst>g_shred_freeze_thresh)
                g_shred_pending_freeze.store(true, std::memory_order_release);
            if (nworst>g_shred_ndc_max){ g_shred_ndc_max=nworst; g_shred_ndc_shape=g_cur_shape;
                g_shred_ndc_w = g_clip[nva*4+3];
                g_shred_ndc_eye[0]=s_eye[nva*3]; g_shred_ndc_eye[1]=s_eye[nva*3+1]; g_shred_ndc_eye[2]=s_eye[nva*3+2];
                g_shred_ndc_mi = g_verts[nva].matidx; g_shred_ndc_pkt = g_verts[nva].packet;
                const int slot = g_verts[nva].matidx/3;
                g_shred_ndc_usepkt = (g_ngx_mtxsrc==0 && g_verts[nva].packet<NGX_MAX_PKT && slot<NGX_MAX_SLOT && g_pkt_have[g_verts[nva].packet][slot]);
                for (int k=0;k<3;k++) g_shred_ndc_pos[k]=g_verts[nva].pos[k];
                if (g_shred_ndc_usepkt) for (int k=0;k<12;k++) g_shred_ndc_M[k]=g_pkt_mtx[g_verts[nva].packet][slot][k];
                else if ((unsigned)g_verts[nva].matidx+2<64) for(int r=0;r<3;r++)for(int c=0;c<4;c++) g_shred_ndc_M[r*4+c]=g_posmtx[g_verts[nva].matidx+r][c];
                g_shred_ndc_nv = (unsigned)nv;
                float bb[6]={1e30f,-1e30f,1e30f,-1e30f,1e30f,-1e30f};
                for (size_t q=0;q<nv;q++){ for(int a=0;a<3;a++){ float e=s_eye[q*3+a];
                    if(e<bb[a*2])bb[a*2]=e; if(e>bb[a*2+1])bb[a*2+1]=e; } }
                for(int k=0;k<6;k++) g_shred_ndc_ebb[k]=bb[k];
                g_shred_ndc_projtype = g_proj_type;
                for(int k=0;k<16;k++) g_shred_ndc_P[k]=g_proj[k];
            }
        }
    }

    // Accumulate this shape's clip-space triangles into the render snapshot,
    // grouped into a per-texture batch (triangle-aligned roll-over so a wrap
    // never splits a triangle; on wrap the batch list resets with the buffer).
    if (g_have_proj && !g_indices.empty()) {
        std::vector<NgxRenderVertex>& snap = g_snap[g_cur];
        std::vector<NgxRenderBatch>&  batches = g_batches[g_cur];
        size_t& count = g_snap_count[g_cur];
        if (snap.size() < SNAP_CAP) snap.resize(SNAP_CAP);
        // Per-texmap bindings for this shape's material (object-model, g_mat_tex).
        // CI formats (C4/C8/C14X2) need a resolved TLUT; else render that texmap flat.
        NgxTexBind tb[8] = {};
        for (int m = 0; m < 8; m++) {
            const NgxTexBind& c = g_mat_tex[m];
            const bool is_ci = (c.fmt == 0x8 || c.fmt == 0x9 || c.fmt == 0xA);
            if (c.addr && c.w && c.h && (!is_ci || c.tlut_addr)) tb[m] = c;
        }
        // Near-plane CLIPPING (not culling). A triangle whose vertices straddle the camera/near
        // plane (some clip.w>0, some ≤0) cannot be drawn raw: the GPU's homogeneous clip lands the
        // boundary vertices at w≈0, so the perspective divide x/w → ∞ and the triangle explodes
        // into screen-spanning shears/spikes (the title-logo shear, the map-sun "white rays",
        // skinned-character blob — multi-matrix geometry that crosses the near plane). We clip in
        // CLIP SPACE against the GC near plane d = clip.z + clip.w ≥ 0 (= gl_Position.z ≥ 0 in
        // mesh.vert.glsl, the same plane Vulkan near-clips on), interpolating clip/rgba/uv[8], so
        // the boundary vertices land exactly on the near plane with a finite, projection-defined w.
        // Lossless: fully-in-front tris pass through unchanged; fully-behind tris drop; straddlers
        // become 1–2 properly-clipped tris. (SUNBRIGHT_NGX_NEARCULL still force-DROPS straddlers for
        // A/B; SUNBRIGHT_NGX_NOCLIP disables clipping to reproduce the raw shear.)
        static const bool noclip = getenv("SUNBRIGHT_NGX_NOCLIP") != nullptr;
        static const bool nearonly = getenv("SUNBRIGHT_NGX_NEARONLY") != nullptr;  // A/B: near-plane-only clip
        static const float nearcull_eps = []{ const char* v = getenv("SUNBRIGHT_NGX_NEARCULL");
            if (!v) return -1e30f; float e = atof(v); return e == 1.0f ? 1.0f : (e == 0.0f ? -1e30f : e); }();
        constexpr int VW = 28;   // floats per vertex: clip[4] + rgba[4] + rgba1[4] + uv[16]
        auto gather = [&](unsigned vidx, float* o) {
            const float* cp = &g_clip[vidx * 4]; const float* lit = &g_litrgba[vidx * 4];
            const float* lit1 = &g_litrgba1[vidx * 4]; const float* uvp = &g_uvs[vidx * 16];
            o[0]=cp[0]; o[1]=cp[1]; o[2]=cp[2]; o[3]=cp[3];
            o[4]=lit[0]; o[5]=lit[1]; o[6]=lit[2]; o[7]=lit[3];
            o[8]=lit1[0]; o[9]=lit1[1]; o[10]=lit1[2]; o[11]=lit1[3];   // COLOR1A1 (clip-interpolated)
            for (int m = 0; m < 16; m++) o[12+m] = uvp[m];
        };
        auto emit_v = [&](const float* v) {
            if (v[3] < g_clip_minw) g_clip_minw = v[3];
            if (v[3] > 0.0f && v[3] < 1.0f) g_clip_tiny++;
            NgxRenderVertex& d = snap[count];
            d.clip[0]=v[0]; d.clip[1]=v[1]; d.clip[2]=v[2]; d.clip[3]=v[3];
            d.rgba[0]=v[4]; d.rgba[1]=v[5]; d.rgba[2]=v[6]; d.rgba[3]=v[7];
            d.rgba1[0]=v[8]; d.rgba1[1]=v[9]; d.rgba1[2]=v[10]; d.rgba1[3]=v[11];
            for (int m = 0; m < 8; m++) { d.uv[m][0]=v[12+m*2]; d.uv[m][1]=v[12+m*2+1]; }
            count++;
        };
        for (size_t t = 0; t + 3 <= g_indices.size(); t += 3) {
            if (nearcull_eps > -1e29f) {
                const float w0 = g_clip[g_indices[t]*4+3], w1 = g_clip[g_indices[t+1]*4+3], w2 = g_clip[g_indices[t+2]*4+3];
                if (w0 <= nearcull_eps || w1 <= nearcull_eps || w2 <= nearcull_eps) continue;
            }
            // Gather + clip the triangle against the view frustum. The near-ONLY clip left
            // screen-spanning spikes for off-screen / behind-camera geometry during camera
            // transitions (it interpolated near-plane verts that land far off-axis). Full
            // 6-plane frustum clip matches the GPU and removes them. SUNBRIGHT_NGX_NEARONLY
            // restores the near-only path for A/B (proves the spikes are the side planes).
            float in[3][VW]; for (int e = 0; e < 3; e++) gather(g_indices[t + e], in[e]);
            float poly[9][VW]; int np = 0;
            if (noclip) { for (int e = 0; e < 3; e++) { for (int k=0;k<VW;k++) poly[e][k]=in[e][k]; } np = 3; }
            else if (nearonly) {
                int n_front;
                np = ngx_clip_near_tri(&in[0][0], VW, &poly[0][0], &n_front);
                g_clip_in++;
                if (n_front == 0)      g_clip_drop++;   // wholly behind near → dropped
                else if (n_front < 3)  g_clip_cut++;    // straddled → clipped
            } else {
                // Pure, unit-tested full-frustum clip (sunbright-render-test test_clip_frustum).
                np = ngx_clip_frustum_tri(&in[0][0], VW, &poly[0][0]);
                g_clip_in++;
                if (np == 0) g_clip_drop++; else if (np != 3) g_clip_cut++;
            }
            if (np < 3) continue;
            const int ntri = np - 2;                       // fan-triangulate the clipped polygon
            if (count + (size_t)ntri * 3 > SNAP_CAP) { count = 0; batches.clear(); }  // safety wrap
            // Open a new batch on material/binding change, after a wrap, or at start.
            const bool tex_diff = batches.empty() ||
                memcmp(batches.back().tex, tb, sizeof tb) != 0;
            if (batches.empty() || tex_diff ||
                batches.back().tev_index != g_cur_tev_index ||
                batches.back().vstart + batches.back().vcount != (uint32_t)count) {
                if (batches.size() >= BATCH_CAP) break;  // bounded
                NgxRenderBatch nb{}; memcpy(nb.tex, tb, sizeof tb);
                nb.vstart = (uint32_t)count; nb.vcount = 0; nb.tev_index = g_cur_tev_index;
                nb.epoch = (uint16_t)(g_efb_epoch < EPOCH_CAP ? g_efb_epoch : EPOCH_CAP - 1);
                batches.push_back(nb);
            }
            for (int f = 0; f < ntri; f++) { emit_v(poly[0]); emit_v(poly[f+1]); emit_v(poly[f+2]); }
            batches.back().vcount += ntri * 3;
            // POST-CLIP shred metric: NDC edge over the EMITTED (clipped) fan triangles, for
            // skinned shapes. This is the geometry the GPU rasterizes — the true visible shred.
            if (g_cur_pnmtx) {
                auto ndc=[&](const float* v,float&X,float&Y){ const float w=v[3]; X=(w>1e-6f)?v[0]/w:0.f; Y=(w>1e-6f)?v[1]/w:0.f; };
                for (int f=0; f<ntri; f++) {
                    const float* tri[3]={poly[0],poly[f+1],poly[f+2]};
                    bool ok=true; for(int e=0;e<3;e++) if(tri[e][3]<=1e-6f) ok=false;
                    if(!ok) continue;
                    for(int e=0;e<3;e++){ float x0,y0,x1,y1; ndc(tri[e],x0,y0); ndc(tri[(e+1)%3],x1,y1);
                        const float d=std::sqrt((x0-x1)*(x0-x1)+(y0-y1)*(y0-y1));
                        if(d>0.f) g_shred_post_n[d<2.f?0:d<8.f?1:d<40.f?2:3]++;
                        if(d>g_shred_post_max){ g_shred_post_max=d; g_shred_post_shape=g_cur_shape; } }
                }
            }
            // ALL-SHAPE post-clip shred metric: same, but for EVERY shape (single-matrix logo
            // too) — split by projection type. This is the truly-emitted, GPU-rasterized geometry,
            // so a big edge here = visible on-screen shred (unlike the pre-clip metric which counts
            // straddlers the clipper removes).
            {
                auto ndc=[&](const float* v,float&X,float&Y){ const float w=v[3]; X=(w>1e-6f)?v[0]/w:0.f; Y=(w>1e-6f)?v[1]/w:0.f; };
                for (int f=0; f<ntri; f++) {
                    const float* tri[3]={poly[0],poly[f+1],poly[f+2]};
                    bool ok=true; for(int e=0;e<3;e++) if(tri[e][3]<=1e-6f) ok=false;
                    if(!ok) continue;
                    float wmax=0.f;
                    for(int e=0;e<3;e++){ float x0,y0,x1,y1; ndc(tri[e],x0,y0); ndc(tri[(e+1)%3],x1,y1);
                        const float d=std::sqrt((x0-x1)*(x0-x1)+(y0-y1)*(y0-y1));
                        if(d>wmax)wmax=d;
                        if(d>0.f) g_shred_all_n[d<2.f?0:d<8.f?1:d<40.f?2:3]++;
                    }
                    if(wmax>=40.f){ if(g_proj_type)g_shred_all_northo++; else g_shred_all_npersp++; }
                    if(wmax>g_shred_all_max){ g_shred_all_max=wmax; g_shred_all_shape=g_cur_shape;
                        g_shred_all_projtype=(int)g_proj_type; g_shred_all_pnmtx=g_cur_pnmtx;
                        g_shred_all_cc=g_cur_chan.valid?g_cur_chan.color0:0xFFFF;
                        g_shred_all_tex0=g_mat_tex[0].addr;
                        for(int e=0;e<3;e++){ float X,Y; ndc(tri[e],X,Y);
                            g_shred_all_ndc[e*2]=X; g_shred_all_ndc[e*2+1]=Y; g_shred_all_triw[e]=tri[e][3]; }
                    }
                }
            }
        }
    }
}

void capture(u32 sh) {
    g_calls++;
    g_cur_shape = sh;
    NgxCP cp{};
    if (!build_cp(sh, cp)) { g_badcp++; return; }

    const u32 nelem = r32(sh + 4) & 0xFFFF;        // mElementCount @0x6
    const u32 draws = r32(sh + 0x38);              // J3DShapeDraw**
    if (!nelem || !valid(draws)) return;

    g_verts.clear();
    g_indices.clear();
    int tris = 0;
    bool any_fail = false;
    const bool skinned = (cp.vcd_lo & 1) != 0;     // PNMTXIDX → multi-matrix; needs per-packet matrices
    const u32 mtcs = r32(sh + 0x34);               // J3DShapeMtx** mMatrices (mElementCount entries)
    if (skinned) std::memset(g_pkt_have, 0, sizeof g_pkt_have);
    // big-endian u16 at byte address a (unkC is a u16 table in guest RAM).
    auto ru16 = [](u32 a) -> u16 {
        if (!valid(a)) return 0;
        const u32 w = mem_r32(a & ~3u);
        return (u16)((w >> ((a & 2) ? 0 : 16)) & 0xFFFF);
    };
    // Draw-matrix array base = j3dSys.mCurrentDrawMtx (J3DSYS+0x104). J3DSys::setModelDrawMtx stores
    // it there AND binds it as GX_POS_MTX_ARRAY (stride = sizeof(Mtx) = 48), synchronously during the
    // model's draw — so it is current and authoritative at capture() time (unlike the LoadIndexedXF
    // seam, which fires ASYNC after the super-call → seam_log empty here). GXLoadPosMtxIndx(m, dest)
    // reads matrix m at drawMtxBase + m*48.
    constexpr u32 J3DSYS = 0x804045DCu;
    const u32 drawMtxBase = skinned ? r32(J3DSYS + 0x104) : 0;
    // Non-skinned shape: the single matrix is drawMtx[mMatrices[0].unk4] (J3DShapeMtx.unk4 @ +0x04),
    // not drawMtx[0]. (transform_eye reads mCurrentDrawMtx as the array base + g_single_idx*48.)
    g_single_idx = 0;
    if (!skinned && valid(mtcs)) { const u32 s0 = r32(mtcs); if (valid(s0)) g_single_idx = ru16(s0 + 0x04); }
    // RUNNING per-slot matrix state across this shape's packets. GX XF pos-matrix slots PERSIST:
    // a packet (J3DShapeMtxMulti) reloads only its non-0xffff slots; verts that reference a slot a
    // LATER packet skipped keep the matrix an EARLIER packet loaded there. So accumulate, and
    // snapshot the running state into g_pkt_mtx[packet] AFTER each packet's loads. Initialise from
    // g_posmtx (the cross-shape global XF state) so slot 0 etc. is sane before the first load.
    float run[NGX_MAX_SLOT][12]; bool runhave[NGX_MAX_SLOT];
    if (skinned) for (int s = 0; s < NGX_MAX_SLOT; s++) {
        runhave[s] = (s * 3 + 2 < 64) && g_posmtx_src[s * 3] != 0;
        if (runhave[s]) for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) run[s][r * 4 + c] = g_posmtx[s * 3 + r][c];
    }
    for (u32 i = 0; i < nelem && i < NGX_MAX_PKT; i++) {
        const u32 dp = r32(draws + i * 4);
        if (!valid(dp)) continue;
        const u32 size = r32(dp + 4);              // mDisplayListSize (vtable@0)
        const u32 list = r32(dp + 8);              // mDisplayList
        const unsigned char* host = sb_ram_fast(list);
        if (!host || size == 0 || size > 0x200000) continue;
        const size_t v0 = g_verts.size();
        const int t = ngx_build_mesh(cp, host, size, resolve, nullptr, g_verts, g_indices);
        if (t < 0) any_fail = true; else tris += t;
        for (size_t v = v0; v < g_verts.size(); v++) g_verts[v].packet = (unsigned char)i;
        if (skinned && nelem >= 2 && i < 4 && getenv("SUNBRIGHT_DBG_PKT")) {
            static int kk = 0;
            if (kk < 240) { kk++;
                unsigned mn = 255, mx = 0;
                for (size_t v = v0; v < g_verts.size(); v++) { unsigned x = g_verts[v].matidx; if (x<mn)mn=x; if (x>mx)mx=x; }
                const u32 smtx = r32(mtcs + i * 4);
                const u32 num  = valid(smtx) ? ru16(smtx + 0x08) : 0;
                const u32 tbl  = valid(smtx) ? r32(smtx + 0x0C) : 0;
                fprintf(stderr, "[pktv] sh=%08x pkt%u verts=%zu matidx=[%u..%u] num=%u unkC=[%u %u %u %u %u %u]\n",
                        sh, i, g_verts.size()-v0, mn, mx, num,
                        ru16(tbl+0), ru16(tbl+2), ru16(tbl+4), ru16(tbl+6), ru16(tbl+8), ru16(tbl+10));
            }
        }
        // Resolve THIS packet's per-slot matrices from its J3DShapeMtxMulti useMtxIndexTable (unkC).
        // J3DShapeMtxMulti: unk8=useMtxNum@+0x08, unkC=useMtxIndexTable@+0x0C; slot j (skipped if
        // unkC[j]==0xffff) loads draw-matrix unkC[j] into XF row j*3 → g_pkt_mtx[packet][j].
        if (skinned && valid(mtcs) && valid(drawMtxBase)) {
            const u32 smtx = r32(mtcs + i * 4);    // J3DShapeMtxMulti*
            const u32 num  = valid(smtx) ? ru16(smtx + 0x08) : 0;
            const u32 tbl  = valid(smtx) ? r32(smtx + 0x0C) : 0;
            for (u32 j = 0; j < num && j < NGX_MAX_SLOT; j++) {   // UPDATE only this packet's loaded slots
                const u16 m = ru16(tbl + j * 2);
                if (m == 0xFFFF) continue;          // skipped slot → keep the running (earlier) matrix
                const u32 a = drawMtxBase + (u32)m * 48u;   // sizeof(Mtx) = 3x4 f32
                if (!valid(a)) continue;
                for (int k = 0; k < 12; k++) run[j][k] = rf(a + k * 4);
                runhave[j] = true;
            }
            for (int s = 0; s < NGX_MAX_SLOT; s++) {   // snapshot the accumulated state for this packet
                g_pkt_have[i][s] = runhave[s];
                if (runhave[s]) for (int k = 0; k < 12; k++) g_pkt_mtx[i][s][k] = run[s][k];
            }
            if (nelem >= 2 && i < 4 && getenv("SUNBRIGHT_DBG_PKT")) {
                static int kt = 0;
                if (kt < 40) { kt++;
                    char b[256]; int o = 0;
                    for (int s = 0; s < (int)num && s < NGX_MAX_SLOT; s++)
                        o += snprintf(b+o, sizeof b-o, " s%d=%u(%.0f,%.0f,%.0f)", s, ru16(tbl+s*2),
                                      run[s][3], run[s][7], run[s][11]);
                    fprintf(stderr, "[pktt] sh=%08x pkt%u drawBase=%08x%s\n", sh, i, drawMtxBase, b);
                }
            }
        }
    }

    if (skinned && nelem >= 2 && getenv("SUNBRIGHT_DBG_PKT")) {
        static int k = 0;
        if (k++ < 8) {
            fprintf(stderr, "[pkt] sh=%08x nelem=%u mMatrices=%08x seam_base=%08x stride=%u seam_log_n=%d\n",
                    sh, nelem, mtcs, g_seam_base, g_seam_stride, g_seam_log_n);
            for (u32 i = 0; i < nelem && i < 4; i++) {
                const u32 smtx = r32(mtcs + i * 4);
                const u32 num = valid(smtx) ? ru16(smtx + 0x08) : 0;
                const u32 tbl = valid(smtx) ? r32(smtx + 0x0C) : 0;
                fprintf(stderr, "   pkt%u smtx=%08x num=%u tbl=%08x unkC=[%u %u %u %u] m00[0..3]=%.3f %.3f %.3f %.2f have00=%d\n",
                        i, smtx, num, tbl, ru16(tbl+0), ru16(tbl+2), ru16(tbl+4), ru16(tbl+6),
                        g_pkt_mtx[i][0][0], g_pkt_mtx[i][0][1], g_pkt_mtx[i][0][2], g_pkt_mtx[i][0][3], (int)g_pkt_have[i][0]);
            }
        }
    }
    // DBG: dump the CLR0 source format + decoded bytes for the magenta-vertex backdrop shape(s),
    // to split a wrong colour into vertex-color DECODE (format misread) vs real game data.
    if (getenv("SUNBRIGHT_NGX_MAGDBG")) {
        static int kk = 0;
        for (auto& v : g_verts) {
            if (v.clr[0][0] > 200 && v.clr[0][1] < 70 && v.clr[0][2] > 200) {
                if (kk++ < 16) {
                    const u32 cbase = cp.array_base[2]; const u32 cstr = cp.array_stride[2];
                    const u32 b0 = valid(cbase) ? mem_r32(cbase) : 0, b1 = valid(cbase) ? mem_r32(cbase+4) : 0;
                    const u32 b2 = valid(cbase) ? mem_r32(cbase+8) : 0;
                    fprintf(stderr,
                    "[mag] sh=%08x clr0=(%u,%u,%u,%u) clr0cls=%u clr0fmt(vat0)=%u | CLR0 arr base=%08x stride=%u bytes[0..11]=%08x %08x %08x | vat0=%08x vat1=%08x vat2=%08x nv=%zu\n",
                    sh, v.clr[0][0], v.clr[0][1], v.clr[0][2], v.clr[0][3], (cp.vcd_lo >> 13) & 3, (cp.vat[0][0] >> 14) & 7,
                    cbase, cstr, b0, b1, b2, cp.vat[0][0], cp.vat[1][0], cp.vat[2][0], g_verts.size());
                    // Compare CLR0 array sources: object-model unk114 vs Dolphin CP vs static.
                    const u32 vd = r32(sh + 0x44);
                    const u32 u114 = r32(J3DSYS + 0x114), stat = valid(vd) ? r32(vd + 0x1C) : 0;
                    auto at = [&](u32 b, u32 i){ return valid(b) ? mem_r32(b + i*4) : 0; };
                    fprintf(stderr,
                    "[magsrc] unk114=%08x stat(vdata+1C)=%08x cp=%08x | unk114[95,265,898]=%08x %08x %08x | stat[95,265,898]=%08x %08x %08x\n",
                    u114, stat, cbase, at(u114,95), at(u114,265), at(u114,898), at(stat,95), at(stat,265), at(stat,898));
                }
                break;
            }
        }
    }
    // DBG: per-vertex position-matrix index (PNMTXIDX, vcd_lo bit0) usage. ngx applies a single
    // mCurrentDrawMtx to the whole shape; shapes with PNMTXIDX are multi-matrix (skinned/jointed,
    // e.g. the title logo) and TEAR if we don't select the per-vertex matrix.
    if (cp.vcd_lo & 1) { g_pnmtx_shapes++; g_pnmtx_verts += g_verts.size(); if (nelem > g_pnmtx_maxnelem) g_pnmtx_maxnelem = nelem; }
    if (nelem > g_max_nelem) g_max_nelem = nelem;

    g_meshes++;
    if (any_fail) g_fail++;
    g_total_verts += g_verts.size();
    g_total_tris  += (unsigned)tris;
    g_last_verts = (unsigned)g_verts.size();
    g_last_tris  = (unsigned)tris;
    g_last_vcdlo = cp.vcd_lo; g_last_vcdhi = cp.vcd_hi;
    g_last_vstride = ngx_vertex_size(cp, 0);
    if (g_last_verts > g_max_verts) g_max_verts = g_last_verts;
    // DBG: CLR0-class + matsrc histograms (vert-weighted) to localize the wash.
    { const unsigned cls = (cp.vcd_lo>>13)&3; g_clr0cls_hist[cls] += g_verts.size();
      const unsigned cfmt = (cp.vat[0][0]>>14)&7; if (cfmt<8) g_clr0fmt_hist[cfmt] += g_verts.size();
      const bool mv = g_cur_chan.valid && ((g_cur_chan.color0>>0)&1);
      const bool en = g_cur_chan.valid && ((g_cur_chan.color0>>1)&1);
      g_matsrc_hist[g_cur_chan.valid ? (mv?1:0) : 2] += g_verts.size();
      g_litcfg_hist[en?1:0] += g_verts.size();
      // capture the biggest no-normal (map) shape's CLR0 first-vertex color + class
      const bool has_nrm = ((cp.vcd_lo>>11)&3)!=0;
      if (!has_nrm && g_verts.size() > g_bigmap_verts) {
          g_bigmap_verts = g_verts.size(); g_bigmap_clr0cls = cls;
          g_bigmap_matvtx = mv; g_bigmap_en = en;
          g_bigmap_vcol[0]=g_verts[0].clr[0][0]; g_bigmap_vcol[1]=g_verts[0].clr[0][1];
          g_bigmap_vcol[2]=g_verts[0].clr[0][2]; g_bigmap_cc=g_cur_chan.color0;
      }
      // biggest shape OVERALL (likely the visible floor/building): cc + normals + vcol0
      if (g_verts.size() > g_bigany_verts) {
          g_bigany_verts = g_verts.size(); g_bigany_cc = g_cur_chan.color0;
          g_bigany_hasnrm = has_nrm; g_bigany_matvtx = mv; g_bigany_en = en;
          g_bigany_clr0cls = cls;
          g_bigany_vcol[0]=g_verts[0].clr[0][0]; g_bigany_vcol[1]=g_verts[0].clr[0][1]; g_bigany_vcol[2]=g_verts[0].clr[0][2];
          double s=0; for (auto&vv:g_verts) s+=(vv.clr[0][0]+vv.clr[0][1]+vv.clr[0][2])/3.0;
          g_bigany_vcolmean = s/g_verts.size();
          g_bigany_clr0base = cp.array_base[2];
          g_bigany_pnmtx = (cp.vcd_lo & 1) != 0;
          g_bigany_vcd = cp.vcd_lo; g_bigany_posbase = cp.array_base[0]; g_bigany_posstride = cp.array_stride[0];
          // raw model-space position bbox: wild ⇒ bad vertex data; sane ⇒ matrix is the culprit
          float pmn[3]={1e30f,1e30f,1e30f}, pmx[3]={-1e30f,-1e30f,-1e30f};
          for (auto&vv:g_verts) for(int k=0;k<3;k++){ if(vv.pos[k]<pmn[k])pmn[k]=vv.pos[k]; if(vv.pos[k]>pmx[k])pmx[k]=vv.pos[k]; }
          for(int k=0;k<3;k++){ g_bigany_pmin[k]=pmn[k]; g_bigany_pmax[k]=pmx[k]; g_bigany_pos0[k]=g_verts[0].pos[k]; }
      }
    }
    if (!g_verts.empty()) {
        g_last_pos[0] = g_verts[0].pos[0];
        g_last_pos[1] = g_verts[0].pos[1];
        g_last_pos[2] = g_verts[0].pos[2];
    }
    g_cur_tev_index = capture_material();   // N5: current J3DMaterial's TEV state
    // DBG: for a magenta-vertex shape, dump its material's texture binding to see WHY tex0=0.
    if (getenv("SUNBRIGHT_NGX_MAGDBG")) {
        bool mag = false; for (auto& v : g_verts) if (v.clr[0][0]>200 && v.clr[0][1]<70 && v.clr[0][2]>200) { mag=true; break; }
        if (mag) { static int kt=0; if (kt++ < 12) {
            const u32 matpacket = r32(0x804045DCu + 0x3C);
            const u32 material  = valid(matpacket) ? r32(matpacket + 0x38) : 0;
            const u32 tevblock  = valid(material) ? r32(material + 0x28) : 0;
            const u32 vt        = valid(tevblock) ? r32(tevblock + 0x00) : 0;
            const u8* tb = valid(tevblock) ? sb_ram_fast(tevblock) : nullptr;
            const u16 texNo0 = tb ? (u16)((tb[0x04]<<8)|tb[0x05]) : 0xFFFF;
            const u32 jtex = r32(0x804045DCu + 0x54);
            const u32 jcount = valid(jtex) ? ((r32(jtex)>>16)&0xFFFF) : 0;
            fprintf(stderr, "[magtex] sh=%08x ti=%d mat=%08x tev=%08x vt=%08x texNo0=%04x | jtex=%08x count=%u | g_mat_tex0 addr=%08x fmt=%u %ux%u\n",
                sh, g_cur_tev_index, material, tevblock, vt, texNo0, jtex, jcount,
                g_mat_tex[0].addr, g_mat_tex[0].fmt, g_mat_tex[0].w, g_mat_tex[0].h);
        }}
    }
    g_cur_pnmtx = (cp.vcd_lo & 1) != 0;     // multi-matrix shape → per-vertex matrix in transform_eye
    transform_eye();   // native XF stage (modelview) + eye-space verification
}

}  // namespace

// Is the ngx native capture/render active (SUNBRIGHT_NGX_SHAPE / NGX_PRESENT)? Render-ownership
// seams in other TUs (scene_render's GXSetProjection + J2DScreen capture) gate on THIS, not on
// sunbright_purejit_mode() — so with recomp eradicated (purejit always on) but ngx capture OFF, the
// guest GX/J2D draws still run under Dolphin's JIT and Dolphin's GX output is the on-screen image
// (the ngx-vs-Dolphin-GX baseline). With ngx on, those seams skip the original (ngx owns the frame).
// Computed from the env via a function-local static (NOT g_enabled) so it is immune to cross-TU
// static-initialization order — other TUs' static override-registrars call this before g_enabled's
// dynamic initializer may have run.
bool ngx_capture_active() {
    static const bool on = sb_env_on("SUNBRIGHT_NGX_SHAPE") || sb_env_on("SUNBRIGHT_NGX_PRESENT");
    return on;
}

// ── LOCKSTEP per-layer A/B (/ngxdrawlimit) ──────────────────────────────────────
// Render only the first N J3DShape draws THIS frame, on BOTH engines: the GX side
// (Dolphin) by skipping the real J3DShape::draw super-call past N (so its EFB/XFB
// holds exactly N shapes), and the ngx side by skipping capture past N (so its
// snapshot holds the same N shapes). /abshot2 then captures the IDENTICAL prefix on
// both → a true lockstep layer-by-layer walk: sweep N and the first N at which ngx
// and GX disagree is the shape that ngx renders wrong. (-1 = no limit = full scene.)
// g_frame_shape_idx counts shapes within the current frame; reset in ngx_frame_publish.
int      g_gx_draw_limit = -1;
unsigned g_frame_shape_idx = 0;
unsigned g_last_frame_shape_count = 0;   // shapes drawn in the last completed frame (probe)
extern "C" void sb_ngx_set_draw_limit(int n) { g_gx_draw_limit = n; }
extern "C" int  sb_ngx_get_draw_limit() { return g_gx_draw_limit; }
extern "C" unsigned sb_ngx_frame_shape_count() { return g_last_frame_shape_count; }
// Per-shape ti map (draw order → material tev_index) so the lockstep result maps a diverging
// shape# to a material for /gxstate. Filled in ov_j3dshape_draw; snapshotted at frame publish.
constexpr int SHAPETI_CAP = 512;
int g_frame_shape_ti[SHAPETI_CAP], g_pub_shape_ti[SHAPETI_CAP];
// /ngxverts — per-vertex dump of the /gxstate target ti's shape: NDC (x,y), RASA alpha, tc0/tc1
// UV. Answers whether the additive cloud's screen coverage is set by geometry (NDC spread), the
// per-vertex alpha gradient, or the noise UV. Set the target with /gxstate?ti=N first.
extern "C" int sb_ngx_verts_dump(char* out, int cap) {
    const auto& V = g_vtxdump_pub;
    const float* M = g_vtxdump_mv_pub;
    int w = snprintf(out, cap, "ngxverts: target_ti=%d  nverts=%zu  projtype=%u(0=PERSP,1+=ORTHO)\n"
                     "  modelview=[%.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f]\n"
                     "  (NDC x,y in [-1,1]; a=RASA; ez=eye-z (persp w=-ez); cw=clip.w; cw~0 => near-plane blowup)\n",
                     g_gxstate_ti.load(), V.size(), g_vtxdump_projtype_pub,
                     M[0],M[1],M[2],M[3], M[4],M[5],M[6],M[7], M[8],M[9],M[10],M[11]);
    float nxmin=1e9f,nxmax=-1e9f,nymin=1e9f,nymax=-1e9f,amin=1e9f,amax=-1e9f; double asum=0;
    for (auto& r : V) { nxmin=std::min(nxmin,r.ndcx); nxmax=std::max(nxmax,r.ndcx);
        nymin=std::min(nymin,r.ndcy); nymax=std::max(nymax,r.ndcy);
        amin=std::min(amin,r.a); amax=std::max(amax,r.a); asum+=r.a; }
    if (!V.empty()) w += snprintf(out+w, cap-w,
        "  NDC bbox x[%.3f,%.3f] y[%.3f,%.3f]  alpha[%.3f,%.3f] mean=%.3f\n",
        nxmin,nxmax,nymin,nymax,amin,amax,asum/V.size());
    for (size_t i=0;i<V.size() && w<cap-100;i++){ const auto& r=V[i];
        w += snprintf(out+w, cap-w, "  v%-3zu clip=(%.2f,%.2f,%.2f,%.2f) a=%.4f uv0=(%+.4f,%+.4f) uv1=(%+.4f,%+.4f)\n",
                      i, r.cx, r.cy, r.cz, r.cw, r.a, r.u0, r.v0, r.u1, r.v1); }
    return w;
}
extern "C" int sb_ngx_shapeti_dump(char* out, int cap) {
    unsigned nsh = g_last_frame_shape_count; if (nsh > SHAPETI_CAP) nsh = SHAPETI_CAP;
    int w = snprintf(out, cap, "shapeti: %u shapes (draw order -> material ti); cluster runs:\n", nsh);
    int run_ti = -999; unsigned run_lo = 0;
    for (unsigned i = 0; i <= nsh; i++) {
        int ti = (i < nsh) ? g_pub_shape_ti[i] : -998;
        if (ti != run_ti) {
            if (run_ti != -999 && w < cap - 64)
                w += snprintf(out+w, cap-w, "  #%u..#%u ti=%d (%u shapes)\n", run_lo, i-1, run_ti, i-run_lo);
            run_ti = ti; run_lo = i;
        }
    }
    return w;
}

// Published by the scene_render GXSetProjection tee (0x80362c34) with the authored
// projection matrix. g_proj is the CURRENTLY-ACTIVE projection (perspective OR
// orthographic) — whichever the game last set, exactly as GX uses it. Each shape's
// transform_eye consumes the live g_proj, so a J3D shape drawn under an orthographic
// projection (e.g. the title-logo models) is projected with ITS ortho matrix, not a
// stale perspective one (which would foreshorten/shear the flat logo). The full 4x4
// is applied (clip = P·eye incl. row 3 → w=1 for ortho, w=-ez for perspective).
unsigned long g_proj_persp = 0, g_proj_ortho = 0; float g_last_ortho[16] = {0}; unsigned g_last_ortho_type = 0;
float g_last_persp[16] = {0};   // ngx's last PERSPECTIVE projection — compared to Dolphin's actual
void ngx_set_projection(const float* m44, unsigned type) {
    if (type != 0) { g_proj_ortho++; g_last_ortho_type = type; for (int i=0;i<16;i++) g_last_ortho[i]=m44[i]; }
    else { g_proj_persp++; for (int i=0;i<16;i++) g_last_persp[i]=m44[i]; }
    g_proj_pass++;
    g_proj_type = type;
    for (int i = 0; i < 16; i++) g_proj[i] = m44[i];
    g_have_proj = true;
}

// Called from the GXCopyTex / GXCopyDisp overrides (gx_stream_own.cpp / efb_native.cpp) — both run
// on the emu/game thread, serialized with shape capture. Close the current EFB epoch: a GXCopyTex
// routed the just-drawn passes to an OFFSCREEN texture (mark the epoch tex-closed → discard at
// present); a GXCopyDisp routed them to the DISPLAY. Then advance the epoch so the next batch of
// passes is tracked separately.
// Global rolling copy log (NOT per-buffer, NOT reset at publish) — shows the TRUE per-frame
// copy sequence regardless of where ngx publishes (J2DScreen, mid-frame). Each entry records
// kind/dest/clear + how many shapes were captured since the previous copy (the epoch's geometry).
struct CopyLog { unsigned char kind; unsigned char clear; u32 dest; unsigned pass; unsigned long shapes_since; unsigned long frame; };
constexpr int COPYLOG_CAP = 64;
CopyLog g_copylog[COPYLOG_CAP];
std::atomic<unsigned> g_copylog_head{0};
unsigned long g_shapes_at_last_copy = 0;   // g_calls (anon-ns, defined above) is visible here in-TU

void ngx_note_efb_copy(bool is_disp, u32 dest, u32 clear) {
    if (g_efb_epoch < EPOCH_CAP) {
        if (!is_disp) g_epoch_tex[g_cur][g_efb_epoch] = true;
        if (g_copyevt[g_cur].size() < 256)
            g_copyevt[g_cur].push_back({(unsigned char)(is_disp ? 1 : 0), g_efb_epoch, g_proj_pass,
                                        (unsigned char)(clear & 1), g_efb_gen});
    }
    // Clear-aware generation: a GXCopyDisp (XFB) displays the CURRENT generation. Record it.
    if (is_disp) g_display_gen[g_cur] = (int)g_efb_gen;
    // A clearing copy wipes the EFB → subsequent draws are a NEW generation.
    if (clear & 1) g_efb_gen++;
    // Rolling global log (diagnostic).
    unsigned h = g_copylog_head.load(std::memory_order_relaxed) % COPYLOG_CAP;
    g_copylog[h] = { (unsigned char)(is_disp ? 1 : 0), (unsigned char)(clear & 1), dest,
                     g_proj_pass, g_calls - g_shapes_at_last_copy, g_frame_swaps };
    g_shapes_at_last_copy = g_calls;
    g_copylog_head.fetch_add(1, std::memory_order_relaxed);
    if (g_efb_epoch < EPOCH_CAP - 1) g_efb_epoch++;
}

// /efbcopies — dump the rolling copy log in order (oldest→newest), grouped by ngx frame.
int sb_ngx_efbcopies_dump(char* out, int cap) {
    unsigned head = g_copylog_head.load(std::memory_order_acquire);
    unsigned n = head < COPYLOG_CAP ? head : COPYLOG_CAP;
    int w = snprintf(out, cap, "efbcopies: total=%u (kind: DISP=display, tex=offscreen)\n", head);
    for (unsigned i = 0; i < n && w < cap - 128; i++) {
        unsigned idx = (head - n + i) % COPYLOG_CAP;
        const CopyLog& e = g_copylog[idx];
        w += snprintf(out + w, cap - w, "  f%-5lu pass=%-3u %s dest=%08x clear=%u shapes_in_epoch=%lu\n",
            e.frame, e.pass, e.kind ? "DISP" : "tex ", e.dest, e.clear, e.shapes_since);
    }
    return w;
}

// Explicit per-frame boundary, called from the J2DScreen::draw tee (scene_render.cpp):
// the HUD draws ONCE per frame AFTER all 3D drawing, so the accumulation buffer holds
// a complete 3D frame at that point. Publish it to the front buffer and flip
// accumulation to the other one — the present (video thread) then always reads a whole
// frame, never a half-accumulated one. The empty-guard makes repeat HUD draws within a
// frame (dialogue etc.) no-ops, and it aligns the 3D publish with the J2D HUD snapshot
// (both taken at the same tee → consistent composited frame). NOT GXSetProjection
// (fires ~5×/frame); NOT a GX HW function (GXCopyDisp can't be safely super-called).
// /ngxfreeze: when set, STOP advancing the published snapshot — every consumer (present,
// /abshot2, /pixbatch, the state dump) then reads ONE identical frozen frame, so I can flip
// debug modes / per-layer isolation / blend on the SAME geometry and the measurements agree
// (the live scene moves between probe calls, which made every reading incomparable). The game
// keeps running; only the rendered/analysed snapshot is latched.
std::atomic<bool> g_ngx_frozen{false};
extern "C" { extern volatile int g_sb_freeze_gx; }   // Present.cpp: latch the GX oracle XFB
extern "C" void sb_ngx_set_freeze(int on) {
    g_ngx_frozen.store(on != 0, std::memory_order_release);
    g_sb_freeze_gx = on ? 1 /*capture-pending*/ : 0 /*release held oracle*/;
}
extern "C" int  sb_ngx_get_freeze() { return g_ngx_frozen.load(std::memory_order_acquire) ? 1 : 0; }

void ngx_frame_publish() {
    if (g_frame_shape_idx > 0) {                     // snapshot the draw#→ti map for /ngxshapeti
        unsigned c = g_frame_shape_idx < SHAPETI_CAP ? g_frame_shape_idx : SHAPETI_CAP;
        for (unsigned i = 0; i < c; i++) g_pub_shape_ti[i] = g_frame_shape_ti[i];
    }
    g_last_frame_shape_count = g_frame_shape_idx;   // how many shapes this frame issued a draw for
    g_frame_shape_idx = 0;                           // reset the lockstep per-frame shape counter
    if (g_ngx_frozen.load(std::memory_order_acquire)) {
        // Frozen: don't swap/publish — keep the latched front buffer. But STILL reset the
        // back buffer so the live capture doesn't accumulate across frames into a giant mess.
        g_snap_count[g_cur] = 0;
        g_batches[g_cur].clear();
        g_shaperec[g_cur].clear();
        g_copyevt[g_cur].clear();
        for (int e = 0; e < EPOCH_CAP; e++) g_epoch_tex[g_cur][e] = false;
        g_efb_epoch = 0; g_efb_gen = 0; g_display_gen[g_cur] = -1;
        g_sky = SkyLatch{}; g_skyxf = SkyXf{}; g_pnmtxdbg = PnmtxDbg{};
        if (g_gxstate.have) g_gxstate_pub = g_gxstate; g_gxstate = GxStateRec{};
        if (g_uvdbg.have) g_uvdbg_pub = g_uvdbg; g_uvdbg = UvDbg{};
        g_pollution_pub = g_pollution; g_pollution.active = false;   // plaza pollution darkening params
        if (!g_vtxdump.empty()) { g_vtxdump_pub = g_vtxdump; g_vtxdump_projtype_pub = g_vtxdump_projtype;
                              for (int i=0;i<12;i++) g_vtxdump_mv_pub[i]=g_vtxdump_mv[i]; }
        g_clip_in=g_clip_drop=g_clip_cut=g_clip_tiny=0; g_clip_minw=1e30f;
        return;
    }
    if (g_snap_count[g_cur] == 0 && g_batches[g_cur].empty()) return;  // no 3D this frame: keep last
    // Display epoch = highest tex-closed epoch (the main scene's offscreen render). Lower-epoch
    // geometry is auxiliary offscreen (reflections/shadows/thumbnails) and must not be presented.
    // If NOTHING was tex-copied (a scene drawn straight to the EFB), keep everything (epoch 0).
    int de = 0;
    for (int e = EPOCH_CAP - 1; e >= 0; e--) if (g_epoch_tex[g_cur][e]) { de = e; break; }
    g_display_epoch[g_cur] = de;
    g_front.store(g_cur, std::memory_order_release);
    // Auto-freeze-on-shred: this just-published frame contains a skinned NDC spike → latch it
    // (ngx + GX oracle) so /abshot2 captures the actual spike frame for an A/B with the oracle.
    if (g_shred_pending_freeze.load(std::memory_order_acquire) && !g_ngx_frozen.load(std::memory_order_acquire)) {
        g_shred_pending_freeze.store(false, std::memory_order_release);
        sb_ngx_set_freeze(1);
        fprintf(stderr, "[shred] auto-froze on skinned NDC spike (max=%.0f shape=%08x)\n", g_shred_ndc_max, g_shred_ndc_shape);
    }
    g_cur ^= 1;
    // interp60: g_cur now indexes the just-displaced frame (N-1, about to be recycled for the next
    // accumulation). Preserve its used portion so the present can interpolate N-1↔N. Double-buffered
    // so the buffer the present is reading isn't overwritten for two more publishes (SPSC, no tear).
    if (ngx_interp60_enabled()) {
        const size_t pc = g_snap_count[g_cur];
        std::vector<NgxRenderVertex>& pv = g_snap_prev[g_prev_cur];
        if (pc > 0 && pc <= g_snap[g_cur].size()) {
            pv.assign(g_snap[g_cur].begin(), g_snap[g_cur].begin() + pc);
            g_batches_prev[g_prev_cur] = g_batches[g_cur];
            g_snap_prev_count[g_prev_cur] = pc;
            g_prev_front.store(g_prev_cur, std::memory_order_release);
            g_prev_cur ^= 1;
        }
    }
    if (g_snap[g_cur].size() < SNAP_CAP) g_snap[g_cur].resize(SNAP_CAP);
    g_snap_count[g_cur] = 0;
    g_batches[g_cur].clear();
    g_shaperec[g_cur].clear();
    g_copyevt[g_cur].clear();
    for (int e = 0; e < EPOCH_CAP; e++) g_epoch_tex[g_cur][e] = false;
    g_efb_epoch = 0; g_efb_gen = 0; g_display_gen[g_cur] = -1;
    g_proj_pass = 0;
    if (g_sky.have) g_sky_pub = g_sky;   // publish this frame's sky breakdown
    g_sky = SkyLatch{};                  // reset for the next frame
    if (g_skyxf.have) g_skyxf_pub = g_skyxf;
    g_skyxf = SkyXf{};
    if (g_pnmtxdbg.have) g_pnmtxdbg_pub = g_pnmtxdbg;
    g_pnmtxdbg = PnmtxDbg{};
    if (g_gxstate.have) g_gxstate_pub = g_gxstate;   // publish this frame's GX-vs-ngx state diff
    g_gxstate = GxStateRec{};                        // reset for the next frame
    g_pollution_pub = g_pollution; g_pollution.active = false;   // plaza pollution darkening params
    if (g_uvdbg.have) g_uvdbg_pub = g_uvdbg; g_uvdbg = UvDbg{};
    if (!g_vtxdump.empty()) { g_vtxdump_pub = g_vtxdump; g_vtxdump_projtype_pub = g_vtxdump_projtype;
                              for (int i=0;i<12;i++) g_vtxdump_mv_pub[i]=g_vtxdump_mv[i]; }
    g_clrdbg_have = false;   // re-latch the per-material colour probe each frame
    g_clrdbg_l0i = -1;
    g_clip_in=g_clip_drop=g_clip_cut=g_clip_tiny=0; g_clip_minw=1e30f;   // per-frame near-clip stats
    g_frame_swaps++;
    g_ngx_front_frame.store(g_frame_swaps, std::memory_order_release);   // stamp the published snapshot
    // SUNBRIGHT_NGX_FREEZE_AT=<N>: deterministically latch the published snapshot at content-frame N.
    // g_frame_swaps counts only frames WITH 3D content (the no-3D early-return above skips it), so a
    // fixed N reliably lands on the same scene every boot (boot is deterministic) — the robust way to
    // capture a TRANSIENT screen (e.g. the title logo ≈ frame 525, before the attract demo) without
    // racing a polled threshold. The just-published front buffer holds frame N's geometry.
    static long s_freeze_at = -2;
    if (s_freeze_at == -2) { const char* e = getenv("SUNBRIGHT_NGX_FREEZE_AT"); s_freeze_at = e ? atol(e) : -1; }
    if (s_freeze_at > 0 && (long)g_frame_swaps >= s_freeze_at && !g_ngx_frozen.load(std::memory_order_acquire)) {
        sb_ngx_set_freeze(1);
        fprintf(stderr, "[freeze-at] auto-froze at content frame_swaps=%lu (target %ld)\n", g_frame_swaps, s_freeze_at);
    }
}

// Frame-id of the currently PUBLISHED ngx snapshot (the one the present + /abshot2 render from).
// Lets /abshot2 self-certify same-state: the ngx side renders this frame; a stale snapshot (no 3D
// that frame → "keep last") shows as a frame id that does NOT advance between live captures.
extern "C" unsigned long sb_ngx_front_frame() { return g_ngx_front_frame.load(std::memory_order_acquire); }

// /ngxmtxsrc?m=N — LIVE skinned-matrix source: 0=per-packet object-model, 1=g_posmtx, 2=modelview.
// Takes effect on the next captured (live) frame. Returns the mode set.
extern "C" int sb_ngx_set_mtxsrc(int m) { if (m >= 0 && m <= 2) g_ngx_mtxsrc = m; return g_ngx_mtxsrc; }

// ── Immediate-mode GX geometry capture (GXDrawCube) ──────────────────────────────────────────────
// ngx renders from the J3D object model; the game's immediate-mode GX draws (GXDrawCube /
// GXDrawSphere, reference/sms GXDraw.c) have no J3D object, so the J3DShape path misses them. The
// Mario occlusion probe (MarioMain.cpp ~195) is a GXDrawCube at Mario writing ONLY a constant
// dst-alpha 0x10 (GXSetColorUpdate(FALSE)+GXSetDstAlpha(ENABLE,0x10)+z-test) — its result is read
// back by GXPeekARGB → MARIO_FLAG_OCCLUDED. Under ngx that cube was never drawn, so the EFB alpha
// is never 0x10. Here we capture it into the SAME clip-space batch pipeline as the J3D shapes, with
// the colour-write mask OFF and the const dst-alpha fed via the PASSCLR vertex alpha — so it writes
// alpha 0x10 (depth-tested against the live scene, so it occludes correctly) WITHOUT painting a
// visible box. Geometry from the pure, unit-tested ngx_imm (render_test `imm_cube`).
//
// The model→eye matrix is the current GX_PNMTX0 (boxDrawPrepare loads viewMtx×scale×translate into
// PNMTX0 via GXLoadPosMtxImm, captured synchronously in g_posmtx[0..2]) and GXSetCurrentMtx(PNMTX0)
// selects it for the cube's GX_DIRECT vertices.
// Counters for the deterministic verify (no eyeballing): cubes captured + emitted tris.
unsigned long g_imm_cube_calls = 0, g_imm_cube_tris = 0, g_imm_cube_onscreen = 0;
unsigned long g_imm_sphere_calls = 0, g_imm_sphere_tris = 0;   // GXDrawSphere skybox dome capture
// The Mario occlusion cube's nearest (front-face) Vulkan depth this frame, for the native
// GXPeekARGB occlusion query (efb_readback_native.cpp). 1e9 = unknown/far (not occluding).
std::atomic<float> g_imm_occ_front_depth{1e9f};
extern "C" int sb_ngx_imm_occ_depth(float* out) {
    float d = g_imm_occ_front_depth.load(std::memory_order_relaxed);
    if (d >= 1e8f) return 0;
    *out = d; return 1;
}
extern "C" void ngx_emit_imm_cube(int color_off, int alpha_off,
                                  int dst_alpha_en, int dst_alpha_val) {
    if (!g_enabled || !g_have_proj) return;
    g_imm_cube_calls++;
    float mtx[12]; for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) mtx[r*4+c] = g_posmtx[r][c];

    // 24 model-space corners → eye (mtx) → clip (g_proj). Reuse the unit-tested projection.
    float corners[24][3]; ngx_imm::cube_corners(corners);
    float clip[24][4];
    int onscreen = 0;
    for (int i = 0; i < 24; i++) {
        const float x = corners[i][0], y = corners[i][1], z = corners[i][2];
        const float ex = mtx[0]*x + mtx[1]*y + mtx[2]*z  + mtx[3];
        const float ey = mtx[4]*x + mtx[5]*y + mtx[6]*z  + mtx[7];
        const float ez = mtx[8]*x + mtx[9]*y + mtx[10]*z + mtx[11];
        ngx_project_eye(g_proj, ex, ey, ez, clip[i]);
        const float w = clip[i][3];
        if (w > 1e-4f) { const float nx = clip[i][0]/w, ny = clip[i][1]/w;
            if (nx >= -1.f && nx <= 1.f && ny >= -1.f && ny <= 1.f) onscreen++; }
    }
    if (onscreen > 0) g_imm_cube_onscreen++;
    // Record the cube's front-face depth for the native occlusion query (GXPeekARGB). All occlusion
    // cubes are at Mario's position → same depth; the colour-off probe (dst_alpha_en) is the one the
    // peek reads, so store on those. (ngx_imm::cube_front_depth_vk is the unit-tested shipping fn.)
    if (dst_alpha_en && onscreen > 0)
        g_imm_occ_front_depth.store(ngx_imm::cube_front_depth_vk(clip, 24), std::memory_order_relaxed);

    // Synthetic PASSCLR material: out colour = raster (white), out alpha = raster alpha. The const
    // dst-alpha rides the vertex alpha (GX forces the framebuffer alpha to the constant; with the
    // colour mask off, only alpha lands). PE: depth-test LEQUAL no-write (boxDrawPrepare), back-cull,
    // no blend, colour/alpha write masks from the live GXSetColorUpdate/AlphaUpdate state.
    NgxTevState st{}; st.num_stages = 1;
    // GX_PASSCLR: cc out = RASC(10) (a=b=c=ZERO(15), d=RASC), clamp, dest=prev.
    st.stage[0].color_env = (15u<<12)|(15u<<8)|(15u<<4)|10u | (1u<<19);
    // ac out = RASA(5) (a=b=c=ZERO(7), d=RASA), clamp, dest=prev.
    st.stage[0].alpha_env = (7u<<13)|(7u<<10)|(7u<<7)|(5u<<4) | (1u<<19);
    st.stage[0].texmap = 0xff; st.stage[0].texcoord = 0xff; st.stage[0].color_chan = 0;  // COLOR0A0
    // TEV swap tables MUST default to identity 0x1B ("rgba"); a zeroed table decodes to "rrrr"
    // (R broadcast) — which clobbers the raster ALPHA with R, so the const dst-alpha never lands.
    for (int i = 0; i < 4; i++) st.swap_table[i] = 0x1B;
    st.pe.z_test = 1; st.pe.z_func = 3 /*GX_LEQUAL*/; st.pe.z_write = 0;   // boxDrawPrepare's z-mode
    st.pe.blend_mode = 0; st.pe.cull = 2 /*GX_CULL_BACK*/;
    st.pe.color_mask_off = (uint8_t)(color_off ? 1 : 0);
    st.pe.alpha_mask_off = (uint8_t)(alpha_off ? 1 : 0);
    // DBG (SUNBRIGHT_IMM_SHOW=1): force the cube VISIBLE (write RGB+A, opaque red). The deterministic
    // check that immediate-mode geometry/transform/clip/cull render — with SHOW the cube paints a red
    // box on Mario (~13k px); without it the colour-masked occlusion box is invisible (scene
    // unchanged). This IS the falsifiable test for the port (vs eyeballing).
    const bool imm_show = getenv("SUNBRIGHT_IMM_SHOW") != nullptr;
    if (imm_show) { st.pe.color_mask_off = 0; st.pe.alpha_mask_off = 0; }
    // FNV-1a key (so identical cube materials dedupe into one tev_index / one pipeline).
    uint64_t h = 1469598103934665603ull; const u8* p = (const u8*)&st;
    for (size_t i = 0; i < offsetof(NgxTevState, key); i++) { h ^= p[i]; h *= 1099511628211ull; }
    st.key = h;
    int ti;
    auto it = g_tevkey_index.find(h);
    if (it != g_tevkey_index.end()) ti = it->second;
    else {
        if (g_tevstates.size() >= TEVSTATE_CAP) { g_tevstates.clear(); g_tevkey_index.clear(); }
        ti = (int)g_tevstates.size(); g_tevstates.push_back(st); g_tevkey_index[h] = ti;
    }

    // Raster colour: white (red under IMM_SHOW), alpha = the forced const dst-alpha (PASSCLR carries
    // it to the framebuffer alpha). GXSetDstAlpha replaces the EFB alpha with the constant; we feed
    // that constant as the vertex alpha through the PASSCLR combiner (so no shader-gen change).
    const float a = dst_alpha_en ? (float)(dst_alpha_val & 0xff) / 255.f : 1.f;
    const float cr = 1.f, cg = imm_show ? 0.f : 1.f, cb = imm_show ? 0.f : 1.f;  // red when shown

    std::vector<NgxRenderVertex>& snap = g_snap[g_cur];
    std::vector<NgxRenderBatch>&  batches = g_batches[g_cur];
    size_t& count = g_snap_count[g_cur];
    if (snap.size() < SNAP_CAP) snap.resize(SNAP_CAP);

    constexpr int VW = 24;   // clip[4] + rgba[4] + uv[16]
    auto gather = [&](int vi, float* o) {
        o[0]=clip[vi][0]; o[1]=clip[vi][1]; o[2]=clip[vi][2]; o[3]=clip[vi][3];
        o[4]=cr; o[5]=cg; o[6]=cb; o[7]=a;
        for (int m = 0; m < 16; m++) o[8+m] = 0.f;
    };
    auto emit_v = [&](const float* v) {
        NgxRenderVertex& d = snap[count];
        d.clip[0]=v[0]; d.clip[1]=v[1]; d.clip[2]=v[2]; d.clip[3]=v[3];
        d.rgba[0]=v[4]; d.rgba[1]=v[5]; d.rgba[2]=v[6]; d.rgba[3]=v[7];
        d.rgba1[0]=v[4]; d.rgba1[1]=v[5]; d.rgba1[2]=v[6]; d.rgba1[3]=v[7];   // imm geometry: col1 = col0
        for (int m = 0; m < 8; m++) { d.uv[m][0]=v[8+m*2]; d.uv[m][1]=v[8+m*2+1]; }
        count++;
    };
    // Open one batch for the whole cube (single material/binding).
    if (batches.size() >= BATCH_CAP) return;
    NgxRenderBatch nb{};   // no textures bound (1×1 white sampled), tev_index = synthetic cube
    nb.vstart = (uint32_t)count; nb.vcount = 0; nb.tev_index = ti;
    nb.epoch = (uint16_t)(g_efb_epoch < EPOCH_CAP ? g_efb_epoch : EPOCH_CAP - 1);
    batches.push_back(nb);
    unsigned idx[36]; ngx_imm::cube_tri_indices(idx);
    for (int t = 0; t < 12; t++) {
        float in[3][VW]; for (int e = 0; e < 3; e++) gather((int)idx[t*3+e], in[e]);
        float poly[9][VW]; int np = ngx_clip_frustum_tri(&in[0][0], VW, &poly[0][0]);
        if (np < 3) continue;
        const int ntri = np - 2;
        if (count + (size_t)ntri * 3 > SNAP_CAP) break;
        for (int f = 0; f < ntri; f++) { emit_v(poly[0]); emit_v(poly[f+1]); emit_v(poly[f+2]); }
        batches.back().vcount += ntri * 3;
        g_imm_cube_tris += ntri;
    }
    if (batches.back().vcount == 0) batches.pop_back();   // wholly off-screen → no empty batch
    if (getenv("SUNBRIGHT_DBG_EFB")) {
        static unsigned long n = 0;
        if ((n++ % 240) == 0) {
            float xn=1e9f,xx=-1e9f,yn=1e9f,yx=-1e9f;
            for (int i=0;i<24;i++){ const float w=clip[i][3]; if(w>1e-4f){ float nx=clip[i][0]/w,ny=clip[i][1]/w;
                if(nx<xn)xn=nx; if(nx>xx)xx=nx; if(ny<yn)yn=ny; if(ny>yx)yx=ny; } }
            fprintf(stderr, "[imm] cube #%lu onscreen=%d/24 tris=%lu NDC[x %.2f..%.2f y %.2f..%.2f] colorOff=%d dstA=%d/%#x\n",
                    n, onscreen, g_imm_cube_tris, xn, xx, yn, yx, color_off, dst_alpha_en, dst_alpha_val);
        }
    }
}

// ── GXDrawSphere capture (TSky::perform skybox dome, Map/Sky.cpp:88) ──────────
// The plaza sky draws a flat-matColor GXDrawSphere(8,0x10) as a huge camera-centred
// backdrop dome (PNMTX0 = scale(100000), CULL_FRONT = seen from inside). It has no
// J3D object, so the shape-capture path misses it → the sky region reads the clear
// colour instead of the dome's blue. We rebuild the unit-tested sphere geometry in
// clip space (current PNMTX0 + projection) and emit it with a flat PASSCLR material
// = the captured matColor (the channel is disabled → fragment == matColor), the sky's
// PE state (z-test LEQUAL + z-write, opaque ONE/ZERO, GX_CULL_FRONT, colour writes ON).
// matColor (r,g,b,a 0..255) is captured live from GXSetChanMatColor (imm_geom_native.cpp).
extern "C" void ngx_emit_imm_sphere(int numMajor, int numMinor, int r, int g, int b, int a) {
    if (!g_enabled || !g_have_proj) return;
    if (numMajor < 1 || numMinor < 2) return;
    const int vc = ngx_imm::sphere_vert_count(numMajor, numMinor);
    const int tc = ngx_imm::sphere_tri_count(numMajor, numMinor);
    if (vc > 512 || tc > 512) return;   // defensive cap; Sky uses (8,16) = 272 verts / 256 tris
    g_imm_sphere_calls++;

    float mtx[12]; for (int rr = 0; rr < 3; rr++) for (int cc = 0; cc < 4; cc++) mtx[rr*4+cc] = g_posmtx[rr][cc];

    static float verts[512][3];
    static float clip[512][4];
    ngx_imm::sphere_verts(numMajor, numMinor, verts);
    int onscreen = 0;
    for (int i = 0; i < vc; i++) {
        const float x = verts[i][0], y = verts[i][1], z = verts[i][2];
        const float ex = mtx[0]*x + mtx[1]*y + mtx[2]*z  + mtx[3];
        const float ey = mtx[4]*x + mtx[5]*y + mtx[6]*z  + mtx[7];
        const float ez = mtx[8]*x + mtx[9]*y + mtx[10]*z + mtx[11];
        ngx_project_eye(g_proj, ex, ey, ez, clip[i]);
        const float w = clip[i][3];
        if (w > 1e-4f) { const float nx = clip[i][0]/w, ny = clip[i][1]/w;
            if (nx >= -1.f && nx <= 1.f && ny >= -1.f && ny <= 1.f) onscreen++; }
    }

    // Flat PASSCLR material carrying the matColor (channel disabled ⇒ fragment = matColor).
    NgxTevState st{}; st.num_stages = 1;
    st.stage[0].color_env = (15u<<12)|(15u<<8)|(15u<<4)|10u | (1u<<19);   // cc out = RASC
    st.stage[0].alpha_env = (7u<<13)|(7u<<10)|(7u<<7)|(5u<<4) | (1u<<19);  // ac out = RASA
    st.stage[0].texmap = 0xff; st.stage[0].texcoord = 0xff; st.stage[0].color_chan = 0;  // COLOR0A0
    for (int i = 0; i < 4; i++) st.swap_table[i] = 0x1B;                   // identity (NOT 0 → "rrrr")
    // Sky.cpp PE: GXSetZMode(TRUE, LEQUAL, TRUE), GXSetBlendMode(BLEND, ONE, ZERO) = opaque,
    // GXSetCullMode(GX_CULL_FRONT). Colour + alpha writes ON (visible backdrop).
    st.pe.z_test = 1; st.pe.z_func = 3 /*GX_LEQUAL*/; st.pe.z_write = 1;
    st.pe.blend_mode = 0; st.pe.cull = 1 /*GX_CULL_FRONT*/;
    st.pe.color_mask_off = 0; st.pe.alpha_mask_off = 0;
    uint64_t h = 1469598103934665603ull; const u8* p = (const u8*)&st;
    for (size_t i = 0; i < offsetof(NgxTevState, key); i++) { h ^= p[i]; h *= 1099511628211ull; }
    st.key = h;
    int ti;
    auto it = g_tevkey_index.find(h);
    if (it != g_tevkey_index.end()) ti = it->second;
    else {
        if (g_tevstates.size() >= TEVSTATE_CAP) { g_tevstates.clear(); g_tevkey_index.clear(); }
        ti = (int)g_tevstates.size(); g_tevstates.push_back(st); g_tevkey_index[h] = ti;
    }

    const float cr = (float)(r & 0xff)/255.f, cg = (float)(g & 0xff)/255.f,
                cb = (float)(b & 0xff)/255.f, ca = (float)(a & 0xff)/255.f;

    std::vector<NgxRenderVertex>& snap = g_snap[g_cur];
    std::vector<NgxRenderBatch>&  batches = g_batches[g_cur];
    size_t& count = g_snap_count[g_cur];
    if (snap.size() < SNAP_CAP) snap.resize(SNAP_CAP);

    constexpr int VW = 24;   // clip[4] + rgba[4] + uv[16]
    auto gather = [&](int vi, float* o) {
        o[0]=clip[vi][0]; o[1]=clip[vi][1]; o[2]=clip[vi][2]; o[3]=clip[vi][3];
        o[4]=cr; o[5]=cg; o[6]=cb; o[7]=ca;
        for (int m = 0; m < 16; m++) o[8+m] = 0.f;
    };
    auto emit_v = [&](const float* v) {
        NgxRenderVertex& d = snap[count];
        d.clip[0]=v[0]; d.clip[1]=v[1]; d.clip[2]=v[2]; d.clip[3]=v[3];
        d.rgba[0]=v[4]; d.rgba[1]=v[5]; d.rgba[2]=v[6]; d.rgba[3]=v[7];
        d.rgba1[0]=v[4]; d.rgba1[1]=v[5]; d.rgba1[2]=v[6]; d.rgba1[3]=v[7];   // imm geometry: col1 = col0
        for (int m = 0; m < 8; m++) { d.uv[m][0]=v[8+m*2]; d.uv[m][1]=v[8+m*2+1]; }
        count++;
    };
    if (batches.size() >= BATCH_CAP) return;
    NgxRenderBatch nb{};
    nb.vstart = (uint32_t)count; nb.vcount = 0; nb.tev_index = ti;
    nb.epoch = (uint16_t)(g_efb_epoch < EPOCH_CAP ? g_efb_epoch : EPOCH_CAP - 1);
    batches.push_back(nb);
    static unsigned idx[512*3]; ngx_imm::sphere_tri_indices(numMajor, numMinor, idx);
    for (int t = 0; t < tc; t++) {
        float in[3][VW]; for (int e = 0; e < 3; e++) gather((int)idx[t*3+e], in[e]);
        float poly[9][VW]; int np = ngx_clip_frustum_tri(&in[0][0], VW, &poly[0][0]);
        if (np < 3) continue;
        const int ntri = np - 2;
        if (count + (size_t)ntri * 3 > SNAP_CAP) break;
        for (int f = 0; f < ntri; f++) { emit_v(poly[0]); emit_v(poly[f+1]); emit_v(poly[f+2]); }
        batches.back().vcount += ntri * 3;
        g_imm_sphere_tris += ntri;
    }
    if (batches.back().vcount == 0) batches.pop_back();
    if (getenv("SUNBRIGHT_DBG_EFB")) {
        static unsigned long n = 0;
        if ((n++ % 120) == 0)
            fprintf(stderr, "[imm] sphere #%lu (%d,%d) onscreen=%d/%d tris=%lu matColor=(%d,%d,%d,%d)\n",
                    n, numMajor, numMinor, onscreen, vc, g_imm_sphere_tris, r&0xff,g&0xff,b&0xff,a&0xff);
    }
}

// Best-effort snapshot accessors for the native Vulkan mesh render (copy promptly
// — the emu thread keeps writing; a torn read at worst yields a stray triangle).
const NgxRenderVertex* ngx_snap_verts(int* nverts) {
    // Latch the published buffer index so ngx_snap_batches reads the SAME frame
    // (the caller fetches verts then batches; a swap between them would mismatch).
    g_read_front = g_front.load(std::memory_order_acquire);
    *nverts = (int)g_snap_count[g_read_front];
    return g_snap[g_read_front].empty() ? nullptr : g_snap[g_read_front].data();
}
const NgxRenderBatch* ngx_snap_batches(int* nbatches) {
    const int f = g_read_front;
    *nbatches = (int)g_batches[f].size();
    return g_batches[f].empty() ? nullptr : g_batches[f].data();
}
// interp60: the preserved previous frame (N-1) for the in-between blend. Latch the prev buffer index
// in ngx_snap_verts_prev so ngx_snap_batches_prev reads the SAME prev frame.
const NgxRenderVertex* ngx_snap_verts_prev(int* nverts) {
    const int f = g_prev_front.load(std::memory_order_acquire);
    g_prev_read_front = f;
    if (f < 0) { *nverts = 0; return nullptr; }
    *nverts = (int)g_snap_prev_count[f];
    return g_snap_prev[f].empty() ? nullptr : g_snap_prev[f].data();
}
const NgxRenderBatch* ngx_snap_batches_prev(int* nbatches) {
    const int f = g_prev_read_front;
    if (f < 0) { *nbatches = 0; return nullptr; }
    *nbatches = (int)g_batches_prev[f].size();
    return g_batches_prev[f].empty() ? nullptr : g_batches_prev[f].data();
}
bool ngx_interp60_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("SUNBRIGHT_INTERP60");
        // Default ON when ngx capture is active (the PC-native engine showcase); opt out with =0.
        v = e ? (atoi(e) != 0) : (ngx_capture_active() ? 1 : 0);
    }
    return v != 0;
}
int  ngx_interp_mode() { return g_ngx_interp_mode.load(std::memory_order_acquire); }
extern "C" void  sb_ngx_set_interp_mode(int m) { g_ngx_interp_mode.store(m, std::memory_order_release); }
extern "C" int   sb_ngx_interp60_enabled() { return ngx_interp60_enabled() ? 1 : 0; }
extern "C" float sb_ngx_interp_alpha() { return g_ngx_interp_alpha_milli.load(std::memory_order_acquire) / 1000.0f; }
extern "C" void  sb_ngx_set_interp_alpha(float a) {
    if (a < 0.f) a = 0.f; if (a > 1.f) a = 1.f;
    g_ngx_interp_alpha_milli.store((int)(a * 1000.f + 0.5f), std::memory_order_release);
}
// Display epoch of the latched snapshot (read after ngx_snap_verts latches g_read_front). The
// present skips batches whose epoch < this (auxiliary offscreen renders — the ghost class).
int ngx_snap_display_epoch() { return g_display_epoch[g_read_front]; }
const NgxTevState* ngx_snap_tevstates(int* nstates) {
    *nstates = (int)g_tevstates.size();
    return g_tevstates.empty() ? nullptr : g_tevstates.data();
}
// ngx projection vs Dolphin's ACTUAL projection (the real rendering data, captured on the
// GPU thread in VertexShaderManager::LoadProjectionMatrix — authoritative, not lagged).
extern "C" const float* sb_get_dolphin_proj(int* type, unsigned long* count);
extern "C" const float* sb_get_dolphin_persp(unsigned long* count);
extern "C" void sb_get_gx_draw_counts(unsigned long*, unsigned long*, unsigned long*, unsigned long*);
extern "C" void sb_get_gx_ambient(unsigned* amb0, unsigned* mat0, unsigned long* count);
extern "C" int ngx_proj_diff_report(char* out, int cap) {
    // Compare the PERSPECTIVE projection (the 3D camera) — set once/frame and stable, so ngx's
    // (CPU) and Dolphin's (GPU) copies ARE comparable despite the async gap. The latest-of-any-type
    // compare below it is racy (HUD ortho churns); keep it only as a sanity line.
    unsigned long dpc = 0; const float* dp = sb_get_dolphin_persp(&dpc);
    const float* gp = g_last_persp;
    int p = snprintf(out, cap,
        "ngx PERSPECTIVE projection vs Dolphin's ACTUAL (VertexShaderManager, GPU-thread)\n"
        "  dolphin persp updates=%lu   ngx persp sets=%lu\n", dpc, g_proj_persp);
    float maxd = 0;
    for (int i = 0; i < 16; i++) { float e = gp[i] - dp[i]; if (e < 0) e = -e; if (e > maxd) maxd = e; }
    p += snprintf(out + p, cap - p, "  max abs element delta: %.5f  %s\n", maxd, maxd < 1e-3f ? "MATCH" : "<<< DIVERGES");
    for (int r = 0; r < 4; r++)
        p += snprintf(out + p, cap - p,
            "    ngx[%d] %10.5f %10.5f %10.5f %10.5f   dol %10.5f %10.5f %10.5f %10.5f\n", r,
            gp[r*4+0], gp[r*4+1], gp[r*4+2], gp[r*4+3], dp[r*4+0], dp[r*4+1], dp[r*4+2], dp[r*4+3]);

    // Draw coverage: is ngx DROPPING geometry? (black water/sky = missing draws). Dolphin's actual
    // perspective draws/indices (GPU thread) vs ngx's captured shapes/triangles. num_indices is an
    // expanded triangle-list, so dolphin-tris = persp_indices/3.
    unsigned long gd=0, gi=0, gpd=0, gpi=0; sb_get_gx_draw_counts(&gd, &gi, &gpd, &gpi);
    unsigned long dol_tris = gpi / 3;
    double cov = dol_tris ? 100.0 * (double)g_total_tris / (double)dol_tris : 0.0;
    p += snprintf(out + p, cap - p,
        "\nDRAW COVERAGE (is ngx dropping geometry?)\n"
        "  Dolphin GX: total_draws=%lu total_idx=%lu | PERSPECTIVE draws=%lu idx=%lu (~%lu tris)\n"
        "  ngx captured: shapes=%lu verts=%lu tris=%lu\n"
        "  ngx 3D-triangle coverage vs Dolphin perspective: %.1f%%  %s\n",
        gd, gi, gpd, gpi, dol_tris, g_calls, g_total_verts, g_total_tris, cov,
        cov < 80.0 ? "<<< ngx is MISSING geometry" : "(coverage ok — bug is shading/texture)");

    // AMBIENT: ngx defaults block-less ambient to 0 (→ black). Dolphin's actual xfmem ambient is
    // the authoritative value. If it's nonzero, that's the black bug confirmed.
    unsigned amb0=0, mat0=0; unsigned long ac=0; sb_get_gx_ambient(&amb0, &mat0, &ac);
    p += snprintf(out + p, cap - p,
        "\nAMBIENT (the black-materials bug)\n"
        "  Dolphin xfmem ambColor[0]=%08x matColor[0]=%08x (persp updates=%lu)\n"
        "  ngx last g_cur_chan.ambColor=(%u,%u,%u,%u)\n"
        "  %s\n",
        amb0, mat0, ac,
        g_cur_chan.ambColor[0], g_cur_chan.ambColor[1], g_cur_chan.ambColor[2], g_cur_chan.ambColor[3],
        (amb0 & 0x00ffffff) ? "<<< Dolphin ambient is NONZERO — ngx's 0 default is the black bug" : "ambient ~0 both");
    return p;
}

// ngx-vs-Dolphin geometry differential report (SUNBRIGHT_NGX_DIFF / probe /ngxgeomdiff).
// Per PNMTXIDX slot: how many times a shape used it, how many of those diverged from
// Dolphin's xfmem matrix, the worst Δ, and the two matrices side by side for the worst.
extern "C" int ngx_geom_diff_report(char* out, int cap) {
    int p = 0;
    // The decisive split: Imm-loaded slots are Dolphin-correct BY CONSTRUCTION (matrix is in the
    // call args). If THOSE diverge from xfmem, then xfmem is stale → not a valid oracle (and the
    // matrices ngx draws are right). If Imm slots MATCH but Indx slots diverge, xfmem is the oracle
    // and the indexed reconstruction is the bug. This is what tells me WHERE the bug is, no eyes.
    int imm_n=0, imm_div=0, indx_n=0, indx_div=0;
    for (int slot = 0; slot < 64; slot++) {
        const GeomDiffSlot& s = g_gdiff[slot];
        if (s.n == 0) continue;
        if (s.src == 1) { imm_n++; if (s.nmiss) imm_div++; }
        else if (s.src == 2) { indx_n++; if (s.nmiss) indx_div++; }
    }
    p += snprintf(out + p, cap - p,
        "ngx geometry diff vs Dolphin (xfmem.posMatrices)\n"
        "multi-matrix shapes seen: %lu   enabled: %s\n\n"
        "== POST-LOAD compare (read xfmem RIGHT AFTER the real load — no tee staleness) ==\n"
        "  Imm  (g_posmtx from ARGS vs xfmem): %lu/%lu diverge  maxD=%.4f\n"
        "  Indx (reconstruction     vs xfmem): %lu/%lu diverge  maxD=%.4f\n"
        "  READ: Imm diverge>0 => xfmem NOT synchronous at hook => not a CPU-side oracle (need pixels).\n"
        "        Imm==0 & Indx>0 => xfmem IS the oracle; indexed reconstruction (base+index*stride) is WRONG.\n"
        "        Imm==0 & Indx==0 => matrices are CORRECT; the bug is downstream (projection/decode/present).\n",
        g_gdiff_mm_shapes, g_ngxdiff ? "yes" : "no (set SUNBRIGHT_NGX_DIFF=1)",
        g_pl_imm_div, g_pl_imm_n, g_pl_imm_maxd, g_pl_indx_div, g_pl_indx_n, g_pl_indx_maxd);
    if (g_pl_indx_maxd > 1e-3f)
        p += snprintf(out + p, cap - p,
            "  worst Indx post-load: ngx=[%.3f %.3f %.3f %.2f /...] dol=[%.3f %.3f %.3f %.2f /...]\n",
            g_pl_indx_ngx[0],g_pl_indx_ngx[1],g_pl_indx_ngx[2],g_pl_indx_ngx[3],
            g_pl_indx_dol[0],g_pl_indx_dol[1],g_pl_indx_dol[2],g_pl_indx_dol[3]);
    p += snprintf(out + p, cap - p,
        "\n== TEE compare (xfmem read at transform_eye — may be stale) ==\n"
        "  Imm slots %d/%d diverge | Indx slots %d/%d diverge\n\n",
        imm_div, imm_n, indx_div, indx_n);
    int active = 0, diverging = 0;
    for (int slot = 0; slot < 64 && cap - p > 360; slot++) {
        const GeomDiffSlot& s = g_gdiff[slot];
        if (s.n == 0) continue;
        active++;
        const bool bad = s.nmiss > 0;
        if (bad) diverging++;
        const char* src = s.src == 1 ? "Imm" : s.src == 2 ? "Indx" : "?";
        p += snprintf(out + p, cap - p, "slot %2d [%-4s]: n=%lu miss=%lu maxD=%.4f %s\n",
            slot, src, s.n, s.nmiss, s.maxd, bad ? "<<< DIVERGES" : "ok");
        if (bad) {
            p += snprintf(out + p, cap - p,
                "   ngx: [%.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f]\n"
                "   dol: [%.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f]\n",
                s.ngx[0],s.ngx[1],s.ngx[2],s.ngx[3], s.ngx[4],s.ngx[5],s.ngx[6],s.ngx[7], s.ngx[8],s.ngx[9],s.ngx[10],s.ngx[11],
                s.dol[0],s.dol[1],s.dol[2],s.dol[3], s.dol[4],s.dol[5],s.dol[6],s.dol[7], s.dol[8],s.dol[9],s.dol[10],s.dol[11]);
        }
    }
    p += snprintf(out + p, cap - p, "\n%d/%d active slots DIVERGE from Dolphin\n", diverging, active);
    return p;
}

// DBG: colour-channel ctrl for a tev index (0xFFFF = no block) — used by the present's
// category-debug mode to tint each batch by its material category.
extern "C" unsigned ngx_tev_cc_dbg(int idx) {
    return (idx >= 0 && idx < (int)TEVSTATE_CAP) ? g_tev_cc[idx] : 0;
}

// /skyshader?ce=HEX — dump the generated TEV GLSL for the first captured material whose
// stage[0].color_env matches (default = the sky 0x09fae8), so we can audit the faithful
// combiner translation directly (no render, no drift).
extern "C" int sb_ngx_gen_shader(unsigned want_s0ce, char* out, int cap) {
    for (const NgxTevState& s : g_tevstates) {
        if (s.stage[0].color_env == want_s0ce) {
            std::string g = sb_tev_gen_fragment(s);
            int n = snprintf(out, cap, "TEV state s0ce=%06x stages=%u kc=%02x ka=%02x:\n",
                s.stage[0].color_env, s.num_stages, s.stage[0].kcsel, s.stage[0].kasel);
            n += snprintf(out+n, cap-n, "%.*s", cap-n-1, g.c_str());
            return n;
        }
    }
    return snprintf(out, cap, "no captured material with s0ce=%06x\n", want_s0ce);
}

// GXSetFog(type, startz, endz, nearz, farz, GXColor color) @ 0x80361b20 — SYNCHRONOUS ground
// truth of the fog state (bpmem.fog LAGS in the ngx process). type=GX_FOG_NONE(0) => fog OFF.
// If type!=0 the far additive cloud is fogged in GX (dimmed) but ngx skips fog = the wash suspect.
u32 g_fog_type = 0xffffffff, g_fog_color = 0; float g_fog_startz = 0, g_fog_endz = 0; unsigned long g_fog_calls = 0;
SUNBRIGHT_OVERRIDE(ov_gxsetfog, 0x80361b20u) {
    if (g_enabled) { g_fog_calls++; g_fog_type = cpu.gpr[3]; g_fog_color = cpu.gpr[4];
        g_fog_startz = (float)cpu.fpr[1].ps0; g_fog_endz = (float)cpu.fpr[2].ps0; }
    ngx_super(cpu, 0x80361b20u);
}

// J3DTexMtx::load(id) @ 0x802ee9d4 — SYNCHRONOUS ground truth of what texmtx J3D actually
// feeds GX (J3DGDLoadTexMtxImm of mTotalMtx into XF row id*3+30), unlike async-lagged xfmem.
// Captures per distinct J3DTexMtx* its loaded scale/translate → match against the cloud's
// mTexMtx ptr (/gxstate tcN mtxptr) to settle "ngx reads scale-2 but xfmem shows identity".
struct TexMtxLoad { u32 self; u32 id; float m0, m3, m5, m7; unsigned long cnt; };
TexMtxLoad g_tml[16]; int g_tml_n = 0; unsigned long g_tml_total = 0;
SUNBRIGHT_OVERRIDE(ov_j3dtexmtx_load, 0x802ee9d4u) {
    if (g_enabled) {
        const u32 self = cpu.gpr[3], id = cpu.gpr[4];
        g_tml_total++;
        const float m0 = rf(self + 0x64 + 0*4), m3 = rf(self + 0x64 + 3*4);
        const float m5 = rf(self + 0x64 + 5*4), m7 = rf(self + 0x64 + 7*4);
        int slot = -1;
        for (int i = 0; i < g_tml_n; i++) if (g_tml[i].self == self) { slot = i; break; }
        if (slot < 0 && g_tml_n < 16) { slot = g_tml_n++; g_tml[slot] = {self,id,0,0,0,0,0}; }
        if (slot >= 0) { g_tml[slot].id=id; g_tml[slot].m0=m0; g_tml[slot].m3=m3;
                         g_tml[slot].m5=m5; g_tml[slot].m7=m7; g_tml[slot].cnt++; }
    }
    ngx_super(cpu, 0x802ee9d4u);
}
extern "C" int sb_ngx_texmtxloads_dump(char* out, int cap) {
    int w = snprintf(out, cap, "texmtxloads: total=%lu distinct=%d (what J3D ACTUALLY loads to GX, sync)\n"
                     "  (match 'self' to /gxstate tcN mtxptr; m0=scale_s m5=scale_t m3/m7=translate)\n",
                     g_tml_total, g_tml_n);
    for (int i = 0; i < g_tml_n; i++) { const auto& t = g_tml[i];
        w += snprintf(out+w, cap-w, "  self=%08x id=%u xfrow=%u  scale=(%.4f,%.4f) trans=(%.3f,%.3f) cnt=%lu\n",
                      t.self, t.id, t.id*3+30, t.m0, t.m5, t.m3, t.m7, t.cnt); }
    return w;
}

// GXLoadTexObj(GXTexObj* obj, GXTexMapID id) @ 0x80360160 — track the texmap-0
// binding so shapes drawn after it can be textured. GXTexObj packed fields
// (reference/sms GXInitTexObj + docs/re_notes/efb_native_60fps.md): image0@0x08 =
// width-1[0:9] / height-1[10:19] / fmt&0xF[20:23]; image3@0x0C = (addr>>5)[0:20].
// GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name) @ 0x803601fc — record the palette
// for a TMEM tlut slot. __GXTlutObjInt: tlut@0x00 (fmt@bits10-11), loadTlut0@0x04
// (lut addr>>5 @bits0-20). CI texobjs reference the slot via texobj.tlutName@0x18.
SUNBRIGHT_OVERRIDE(ov_gxloadtlut, 0x803601fcu) {
    if (g_enabled) {
        const u32 obj = cpu.gpr[3], name = cpu.gpr[4] & 0xFF;
        if (valid(obj)) {
            const u32 tlut = r32(obj + 0x00), loadTlut0 = r32(obj + 0x04);
            g_tlut[name].fmt = (u8)((tlut >> 10) & 3);
            g_tlut[name].addr = 0x80000000u | ((loadTlut0 & 0x1FFFFFu) << 5);
            g_tlut[name].valid = true;
        }
    }
    ngx_super(cpu, 0x803601fcu);
}

// Per-texmap GXLoadTexObj + Preloaded histograms (diagnostic).
unsigned long g_texobj_hist[9] = {0}, g_preload_hist[9] = {0};
// Distinct texmap-0 texture addresses ever bound (bounded set) + last-batch stats.
u32 g_distinct_addr[512]; unsigned g_distinct_n = 0; bool g_distinct_overflow = false;
void note_addr(u32 a) {
    if (a == 0) return;
    for (unsigned i = 0; i < g_distinct_n; i++) if (g_distinct_addr[i] == a) return;
    if (g_distinct_n < 512) g_distinct_addr[g_distinct_n++] = a; else g_distinct_overflow = true;
}

// GXLoadTexObjPreLoaded(GXTexObj* obj, GXTexRegion* region, GXTexMapID id) @ 0x8035ffb8.
SUNBRIGHT_OVERRIDE(ov_gxloadtexobjpreloaded, 0x8035ffb8u) {
    if (g_enabled) {
        const u32 id = cpu.gpr[5];
        g_preload_hist[id < 8 ? id : 8]++;
        if (id < 8 && valid(cpu.gpr[3])) decode_texobj(cpu.gpr[3], g_curtex[id]);
    }
    ngx_super(cpu, 0x8035ffb8u);
}

// GXLoadTexObj(GXTexObj* obj, GXTexMapID id) @ 0x80360160 — capture the binding for
// EVERY texmap (0..7) so per-stage TEV texmap selection samples the right texture.
SUNBRIGHT_OVERRIDE(ov_gxloadtexobj, 0x80360160u) {
    if (g_enabled) {
        const u32 id = cpu.gpr[4];
        g_texobj_hist[id < 8 ? id : 8]++;
        if (id < 8 && valid(cpu.gpr[3])) { decode_texobj(cpu.gpr[3], g_curtex[id]); note_addr(g_curtex[id].addr); }
    }
    ngx_super(cpu, 0x80360160u);
}

// GXLoadLightObjImm(GXLightObj* lit, GXLightID id) @ 0x8035f26c — capture the live
// hardware light state for the native lighting stage. id is a GXLightID bit mask
// (GX_LIGHT0=0x01 .. GX_LIGHT7=0x80); a light may be loaded into several slots.
SUNBRIGHT_OVERRIDE(ov_gxloadlightobjimm, 0x8035f26cu) {
    if (g_enabled) {
        const u32 obj = cpu.gpr[3], id = cpu.gpr[4];
        if (valid(obj)) {
            LightObj L; L.valid = true;
            const u32 col = r32(obj + 0x0C);
            L.color[0] = ((col >> 24) & 0xFF) / 255.f;
            L.color[1] = ((col >> 16) & 0xFF) / 255.f;
            L.color[2] = ((col >>  8) & 0xFF) / 255.f;
            L.cosA[0]  = rf(obj + 0x10); L.cosA[1]  = rf(obj + 0x14); L.cosA[2]  = rf(obj + 0x18);
            L.distA[0] = rf(obj + 0x1C); L.distA[1] = rf(obj + 0x20); L.distA[2] = rf(obj + 0x24);
            L.pos[0]   = rf(obj + 0x28); L.pos[1]   = rf(obj + 0x2C); L.pos[2]   = rf(obj + 0x30);
            L.dir[0]   = rf(obj + 0x34); L.dir[1]   = rf(obj + 0x38); L.dir[2]   = rf(obj + 0x3C);
            for (int i = 0; i < 8; i++) if (id & (1u << i)) g_light[i] = L;
            g_light_loads++;
        }
    }
    ngx_super(cpu, 0x8035f26cu);
}

// GXSetCopyClear(GXColor clr, u32 clear_z) @ 0x8035ea40 — capture the EFB copy-clear colour
// the game requests. GXColor/JUtility::TColor is a 4-byte struct passed by value in gpr[3]
// as a big-endian packed word: r=MSB. The native present clears the 3D target to this (see
// g_copy_clear); a hardcoded clear washed the screen-blend sky (ti=11 dst=INVSRCCLR).
SUNBRIGHT_OVERRIDE(ov_gxsetcopyclear, 0x8035ea40u) {
    if (g_enabled) {
        // GXColor is passed BY POINTER in r3 (verified: r3 is a RAM ptr, [r3] = the packed
        // RGBA8888 colour = black here, matching the GPU's bpmem clear; r4 = clear_z 0xffffff).
        // If r3 isn't a valid pointer, fall back to the by-value packed-word interpretation.
        const u32 r3 = cpu.gpr[3];
        const u32 col = valid(r3) ? r32(r3) : r3;
        g_copy_clear[0] = ((col >> 24) & 0xFF) / 255.f;
        g_copy_clear[1] = ((col >> 16) & 0xFF) / 255.f;
        g_copy_clear[2] = ((col >>  8) & 0xFF) / 255.f;
        g_copy_clear[3] = 1.f;   // backdrop is opaque for the present (EFB clear alpha unused on screen)
        g_copy_clear_arg = r3; g_copy_clear_arg4 = cpu.gpr[4]; g_copy_clear_deref = col;
        g_copy_clear_sets++;
    }
    ngx_super(cpu, 0x8035ea40u);
}

// Accessor for the native present (runtime/render/ngx_present.cpp).
extern "C" void sb_ngx_get_clear(float out[4]) {
    out[0] = g_copy_clear[0]; out[1] = g_copy_clear[1];
    out[2] = g_copy_clear[2]; out[3] = g_copy_clear[3];
}

// Synchronous PNMTXIDX capture from the fork's LoadIndexedXF seam (interp_capture.cpp's
// sb_hook_xf_indexed). Records the EXACT pos-matrix Dolphin loads — source = guest RAM at
// array_base + stride*index (host-order floats), slot = XF dest word offset / 4. Correct base
// AND correct timing (decoded in GX-stream order on the guest thread, before capture(sh)), unlike
// the GXLoadPosMtxIndx function-hook reconstruction below (stale base → garbage). This is THE fix
// for multi-matrix (skinned: Mario/NPCs/logo) shapes collapsing into giant overdraw.
extern "C" void ngx_capture_indexed_posmtx(unsigned base, unsigned stride, unsigned index, unsigned address) {
    g_seam_base = base; g_seam_stride = stride;
    const unsigned slot = address / 4;
    if (slot + 2 >= 64) return;
    const unsigned a = ((base & 0x03FFFFFFu) | 0x80000000u) + stride * index;
    float m[12];
    for (int k = 0; k < 12; k++) m[k] = rf(a + k * 4);
    for (unsigned r = 0; r < 3; r++)
        for (unsigned c = 0; c < 4; c++) g_posmtx[slot + r][c] = m[r * 4 + c];
    for (unsigned r = 0; r < 3; r++) g_posmtx_src[slot + r] = 3;   // 3 = seam (Dolphin-exact)
    // Ordered per-shape log (capture() partitions by packet → true per-packet matrices).
    if (g_seam_log_n < (int)(sizeof g_seam_log / sizeof g_seam_log[0])) {
        SeamLoad& s = g_seam_log[g_seam_log_n++];
        s.slot = slot; for (int k = 0; k < 12; k++) s.m[k] = m[k];
    }
}

// GXLoadPosMtxImm(f32 mtx[3][4], u32 id) @ 0x80362e0c — capture the position-matrix memory for
// PNMTXIDX (multi-matrix/skinned) shapes. id = the matrix-memory ROW (id*4 = float address);
// it equals the per-vertex PNMTXIDX byte. mtx is a guest 3×4 row-major matrix (12 floats).
SUNBRIGHT_OVERRIDE(ov_gxloadposmtximm, 0x80362e0cu) {
    u32 cap_id = 0xFFFFFFFFu;   // captured for the post-load xfmem compare (GPRs clobbered by the call)
    if (g_enabled) {
        const u32 mp = cpu.gpr[3], id = cpu.gpr[4];
        if (valid(mp) && id + 2 < 64) {
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++) g_posmtx[id + r][c] = rf(mp + (r * 4 + c) * 4);
            for (int r = 0; r < 3; r++) g_posmtx_src[id + r] = 1;   // Imm = Dolphin-correct by construction
            g_posmtx_loads++; cap_id = id;
        }
    }
    ngx_super(cpu, 0x80362e0cu);
    if (cap_id != 0xFFFFFFFFu) ngx_postload_cmp(cap_id, true);   // synchrony validator
}

// GXLoadPosMtxIndx(u16 index, GXPosNrmMtx mtxIndex) @ 0x80362e48 — the INDEXED position-matrix
// load used by J3DShapeMtxMulti / billboards / skinned shapes (via J3DSys::loadPosMtxIndx →
// loadMtxIndx_*). UNLIKE the immediate form, the matrix is NOT in the call args — it lives in the
// guest position-matrix array (CPArray::XF_A, bound by GXSetArray) and is referenced by `index`.
// Hooking only the immediate form left g_posmtx ZERO for these shapes → they transformed by a null
// matrix (collapsed/sheared) — the title logo / sun-rays / clouds bug. Replicate the indexed read:
// matrix = XF_A_base + index*stride (12 big-endian floats, 3×4 row-major) → g_posmtx[slot].
u32 g_pmi_base=0, g_pmi_stride=0, g_pmi_index=0, g_pmi_slot=0, g_pmi_mp=0; unsigned long g_pmi_calls=0;
SUNBRIGHT_OVERRIDE(ov_gxloadposmtxindx, 0x80362e48u) {
    u32 cap_slot = 0xFFFFFFFFu;   // captured for the post-load xfmem compare
    if (g_enabled) {
        // Args (GXLoadPosMtxIndx(u16 mtx_indx, u32 id)): gpr[3]=mtx_indx = ARRAY index into the
        // pos-matrix array; gpr[4]=id = the XF destination slot (= the per-vertex PNMTXIDX byte;
        // J3DSys::loadPosMtxIndx passes id*3, the loop slot). The matrix is NOT in the args — it
        // lives at array_base + mtx_indx*stride. The correct array base (CPArray::XF_A) is set inside
        // the shape's pre-built display list (GXCallDisplayList), processed by Dolphin's CP via the
        // FIFO — so it lives in g_main_cp_state, which LAGS on the emu thread (reading it here gave a
        // stale/freed base → garbage matrices → giant overdraw, WORSE than the wrong-but-finite
        // J3DSYS+0x104 baseline). Synchronous indexed-matrix capture is handled at the fork's
        // LoadIndexedXF seam instead (the real Dolphin load); this baseline is left until that lands.
        const u32 index = cpu.gpr[3] & 0xFFFF, slot = cpu.gpr[4];
        constexpr u32 J3DSYS_ = 0x804045DCu;
        const u32 base = mem_r32(J3DSYS_ + 0x104);
        const u32 stride = 48;   // sizeof(Mtx) = 3×4 f32
        const u32 mp = base + index * stride;
        g_pmi_base=base; g_pmi_stride=stride; g_pmi_index=index; g_pmi_slot=slot; g_pmi_mp=mp; g_pmi_calls++;
        if (valid(mp) && slot + 2 < 64) {
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++) g_posmtx[slot + r][c] = rf(mp + (r * 4 + c) * 4);
            for (int r = 0; r < 3; r++) g_posmtx_src[slot + r] = 2;   // Indx = ngx reconstructs → suspect
            cap_slot = slot;
        }
        if (getenv("SUNBRIGHT_DBG_PMI")) {
            ngx_super(cpu, 0x80362e48u);
            static int k = 0;
            if (k++ < 24 && slot + 2 < 64)
                fprintf(stderr, "[pmi] slot=%u index=%u base=%08x tx=%.1f ty=%.1f tz=%.1f | rows=[%.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f]\n",
                        slot, index, base, rf(mp+12), rf(mp+28), rf(mp+44),
                        rf(mp+0), rf(mp+4), rf(mp+8), rf(mp+12),
                        rf(mp+16), rf(mp+20), rf(mp+24), rf(mp+28),
                        rf(mp+32), rf(mp+36), rf(mp+40), rf(mp+44));
            return;
        }
    }
    ngx_super(cpu, 0x80362e48u);
    if (cap_slot != 0xFFFFFFFFu) ngx_postload_cmp(cap_slot, false);   // reconstruction vs Dolphin's actual load
}

// GXSetChanCtrl(chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn) @ 0x8035f6d0 —
// capture the AUTHORITATIVE per-channel lighting control, reconstructed exactly as GX packs
// it into the XF channel register (reference dolphin/gx GXSetChanCtrl). This is the same
// LitChannel.hex Dolphin's renderer uses — captured SYNCHRONOUSLY on the emu thread (the J3D
// color-block byte parse was reading the wrong offsets → wrong enable/ambient/diffuse).
SUNBRIGHT_OVERRIDE(ov_gxsetchanctrl, 0x8035f6d0u) {
    if (g_enabled) {
        const u32 chan = cpu.gpr[3], enable = cpu.gpr[4], amb_src = cpu.gpr[5],
                  mat_src = cpu.gpr[6], lmask = cpu.gpr[7], diff_fn = cpu.gpr[8], attn_fn = cpu.gpr[9];
        u32 reg = 0;
        reg |= (enable & 1) << 1;
        reg |= (mat_src & 1) << 0;
        reg |= (amb_src & 1) << 6;
        if (lmask & 0x01) reg |= 1u << 2;  if (lmask & 0x02) reg |= 1u << 3;
        if (lmask & 0x04) reg |= 1u << 4;  if (lmask & 0x08) reg |= 1u << 5;
        if (lmask & 0x10) reg |= 1u << 11; if (lmask & 0x20) reg |= 1u << 12;
        if (lmask & 0x40) reg |= 1u << 13; if (lmask & 0x80) reg |= 1u << 14;
        reg |= ((attn_fn == 0) ? 0u : (diff_fn & 3)) << 7;   // diffuse (zeroed when SPEC)
        reg |= ((attn_fn != 2) ? 1u : 0u) << 9;              // attn bit 9
        reg |= ((attn_fn != 0) ? 1u : 0u) << 10;             // attn bit 10
        const int idx = (chan == 4) ? 0 : (chan == 5) ? 1 : (chan <= 1 ? (int)chan : -1);
        if (idx >= 0) { g_gx_cc[idx] = (u16)reg; g_gx_cc_have[idx] = true; g_gx_cc_sets[idx]++; }
    }
    ngx_super(cpu, 0x8035f6d0u);
}

// GXSetChanMatColor(GXChannelID chan, GXColor color) @ 0x8035f51c — material colour register.
SUNBRIGHT_OVERRIDE(ov_gxsetchanmatcolor, 0x8035f51cu) {
    if (g_enabled) {
        const u32 chan = cpu.gpr[3], c = cpu.gpr[4];
        const int idx = (chan == 0 || chan == 4) ? 0 : (chan == 1 || chan == 5) ? 1 : -1;
        if (idx >= 0) {
            g_gx_matcol[idx][0] = (u8)(c >> 24); g_gx_matcol[idx][1] = (u8)(c >> 16);
            g_gx_matcol[idx][2] = (u8)(c >> 8);  g_gx_matcol[idx][3] = (u8)c;
            g_gx_matcol_have[idx] = true; g_gx_matcol_sets[idx]++;
        }
    }
    ngx_super(cpu, 0x8035f51cu);
}

// GXSetChanAmbColor(GXChannelID chan, GXColor color) @ 0x8035f3b4 — capture the
// global hardware ambient register. GXChannelID: COLOR0=0, COLOR1=1, COLOR0A0=4,
// COLOR1A1=5. color is a 4-byte GXColor passed by value in gpr[4] (R in high byte).
SUNBRIGHT_OVERRIDE(ov_gxsetchanambcolor, 0x8035f3b4u) {
    if (g_enabled) {
        // GXColor is passed BY POINTER in gpr[4] (like GXSetCopyClear's r3) — gpr[4] reads as a
        // guest pointer (0x804263xx), so [gpr[4]] = the packed RGBA8888. The old by-value read
        // captured the pointer bytes (the "purple (128,66,99)" garbage). Deref when it's a pointer.
        const u32 chan = cpu.gpr[3], r4 = cpu.gpr[4];
        const u32 c = valid(r4) ? r32(r4) : r4;
        const int idx = (chan == 0 || chan == 4) ? 0 : (chan == 1 || chan == 5) ? 1 : -1;
        if (idx >= 0) {
            g_amb_reg[idx][0] = (u8)(c >> 24); g_amb_reg[idx][1] = (u8)(c >> 16);
            g_amb_reg[idx][2] = (u8)(c >> 8);  g_amb_reg[idx][3] = (u8)c;
            g_amb_have[idx] = true; g_amb_sets++;
            xf_amb_write(idx, (u8)(c>>24), (u8)(c>>16), (u8)(c>>8), (u8)c);
        }
    }
    ngx_super(cpu, 0x8035f3b4u);
}

// J3DGDSetChanAmbColor(GXChannelID chan, GXColor color) @ 0x802f33a8 — J3D's GD/XF-direct
// ambient write (the path materials actually use; GXSetChanAmbColor is mostly unused by J3D,
// which is why the GX-function tee read a stale/wrong value). color is a 4-byte GXColor by
// value in gpr[4] (R in the high byte). g_gd_amb is draw-order-current at capture time.
SUNBRIGHT_OVERRIDE(ov_j3dgdsetchanambcolor, 0x802f33a8u) {
    if (g_enabled) {
        const u32 chan = cpu.gpr[3], c = cpu.gpr[4];
        const int idx = (chan == 0 || chan == 4) ? 0 : (chan == 1 || chan == 5) ? 1 : -1;
        if (idx >= 0) {
            g_gd_amb[idx][0] = (u8)(c >> 24); g_gd_amb[idx][1] = (u8)(c >> 16);
            g_gd_amb[idx][2] = (u8)(c >> 8);  g_gd_amb[idx][3] = (u8)c;
            g_gd_amb_have[idx] = true; g_gd_amb_sets++;
            xf_amb_write(idx, (u8)(c>>24), (u8)(c>>16), (u8)(c>>8), (u8)c);
            for (int k = 0; k < 4; k++) g_gd_amb_last[k] = g_gd_amb[idx][k];
        }
    }
    ngx_super(cpu, 0x802f33a8u);
}

// Dump Dolphin's LIVE xfmem lighting/colour state (ground truth). In the pure-Dolphin
// oracle (NGX_SHAPE off, GX rendered synchronously) xfmem is CURRENT — this is the
// authoritative ambient/material/channel state the native renderer must reproduce. (In an
// ngx-capture process xfmem LAGS behind the GPU thread, so prefer the oracle for truth.)
extern "C" int sb_xfmem_dump(char* out, int cap) {
    int n = 0;
    auto A = [&](const char* fmt, ...) { if (n >= cap) return; va_list ap; va_start(ap, fmt);
        n += vsnprintf(out + n, cap - n, fmt, ap); va_end(ap); };
    A("xfmem (live): numChan=%u\n", (unsigned)xfmem.numChan.numColorChans);
    for (int c = 0; c < 2; c++) {
        u32 amb = xfmem.ambColor[c], mat = xfmem.matColor[c];
        A("  chan%d  amb=(%u,%u,%u,%u) mat=(%u,%u,%u,%u)\n", c,
          (amb>>24)&0xff,(amb>>16)&0xff,(amb>>8)&0xff,amb&0xff,
          (mat>>24)&0xff,(mat>>16)&0xff,(mat>>8)&0xff,mat&0xff);
        const LitChannel& lc = xfmem.color[c];
        A("    color  hex=%08x matsrc=%u enable=%u ambsrc=%u diff=%u attn=%u lightmask=%02x\n",
          (unsigned)lc.hex, (unsigned)(MatSource)lc.matsource, (unsigned)(bool)lc.enablelighting,
          (unsigned)(AmbSource)lc.ambsource, (unsigned)(DiffuseFunc)lc.diffusefunc,
          (unsigned)(AttenuationFunc)lc.attnfunc, (unsigned)(lc.GetFullLightMask()));
        const LitChannel& la = xfmem.alpha[c];
        A("    alpha  hex=%08x matsrc=%u enable=%u\n", (unsigned)la.hex,
          (unsigned)(MatSource)la.matsource, (unsigned)(bool)la.enablelighting);
    }
    // Last-loaded position matrices (XF slots 0/3/6/9) — GPU ground truth. On the oracle
    // (which processes its FIFO) these are the correct modelview the renderer uses; compare
    // vs ngx's synchronous guestRAM read (SUNBRIGHT_DBG_PMI) to see if a view-multiply lands
    // after the indexed-load is issued (→ ngx reads the matrix too early).
    for (int s = 0; s <= 9; s += 3) {
        const float* m = &xfmem.posMatrices[s * 4];
        A("  posMtx[%d] tz=%.1f rows=[%.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f]\n",
          s, m[11], m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11]);
    }
    return n;
}

// ── Always-on (oracle-usable) xfmem-state histogram at J3DShape::draw ────────────
// Records the DISTINCT (color0-channel-ctrl, ambColor0, light0-color) tuples the GPU
// actually sees across a frame. Runs in BOTH the ngx process and the pure-Dolphin
// oracle (NOT gated on g_enabled) so we get ground truth for which ambient/light the
// blue sky uses — the function tees miss J3D's inlined GD/XF writes; xfmem does not.
struct XfTuple { u32 cchex; u32 amb; u32 mat; u8 lcol[4]; unsigned long cnt; };
XfTuple g_xfhist[64]; int g_xfhist_n = 0;
void xfmem_draw_observe() {
    const u32 cchex = (u32)xfmem.color[0].hex;
    const u32 amb   = xfmem.ambColor[0];
    const u32 mat   = xfmem.matColor[0];
    const u8* lc    = xfmem.lights[0].color;
    for (int i = 0; i < g_xfhist_n; i++)
        if (g_xfhist[i].cchex == cchex && g_xfhist[i].amb == amb && g_xfhist[i].mat == mat &&
            g_xfhist[i].lcol[0]==lc[0] && g_xfhist[i].lcol[1]==lc[1] &&
            g_xfhist[i].lcol[2]==lc[2]) { g_xfhist[i].cnt++; return; }
    if (g_xfhist_n < 64) {
        XfTuple& t = g_xfhist[g_xfhist_n++];
        t.cchex = cchex; t.amb = amb; t.mat = mat;
        t.lcol[0]=lc[0]; t.lcol[1]=lc[1]; t.lcol[2]=lc[2]; t.lcol[3]=lc[3]; t.cnt = 1;
    }
}
extern "C" int sb_xfmem_hist(char* out, int cap) {
    int n = snprintf(out, cap, "xfmem draw-observe distinct tuples (n=%d):\n", g_xfhist_n);
    for (int i = 0; i < g_xfhist_n; i++) {
        const XfTuple& t = g_xfhist[i];
        n += snprintf(out+n, cap-n,
            "  cc=%08x amb=(%u,%u,%u,%u) mat=(%u,%u,%u,%u) light0=(%u,%u,%u) cnt=%lu\n",
            t.cchex, (t.amb>>24)&0xff,(t.amb>>16)&0xff,(t.amb>>8)&0xff,t.amb&0xff,
            (t.mat>>24)&0xff,(t.mat>>16)&0xff,(t.mat>>8)&0xff,t.mat&0xff,
            t.lcol[0],t.lcol[1],t.lcol[2], t.cnt);
    }
    return n;
}

SUNBRIGHT_OVERRIDE(ov_j3dshape_draw, 0x802e0390u) {
    const u32 sh = cpu.gpr[3];   // save before the super-call clobbers gpr
    // LOCKSTEP draw limit: skip this shape on BOTH engines once past the limit so the GX
    // EFB and the ngx snapshot hold the SAME first-N shapes (see /ngxdrawlimit above).
    const unsigned my_idx = g_frame_shape_idx++;
    if (g_gx_draw_limit >= 0 && (int)my_idx >= g_gx_draw_limit) return;  // draw nothing (GX) + no capture
    // Run the real draw FIRST: J3DShape::draw is what sets j3dSys's per-view vertex
    // arrays (loadVtxArray) AND the modelview (setModelDrawMtx) for THIS shape, so
    // we must capture after it for the arrays + matrix to be current.
    g_seam_log_n = 0;   // collect THIS shape's indexed pos-matrix loads in stream order
    // FULL-PC-ENGINE path (SUNBRIGHT_NGX_NATIVE_DRAW=1, or implied by SUNBRIGHT_NO_RECOMP when ngx
    // presents): ngx OWNS the shape draw — do NOT super-call the recompiled J3DShape::draw. The
    // original draw's only outputs are (a) GX commands (VCD/VAT load, mMatrices[i]->load to XF,
    // mDraws[i]->draw FIFO) which are DISCARDED when ngx is the presented frame, and (b) the
    // per-view j3dSys state ngx's capture() reads: mCurrentDrawMtx/mCurrentNormMtx (set by
    // setModelDrawMtx/setModelNrmMtx). We reproduce ONLY (b) natively, reading the shape's guest
    // fields (J3DShape: mDrawMatrices@0x50, mNormMatrices@0x54, mCurrentViewNo@0x58; J3DSys:
    // mCurrentDrawMtx@0x104, mCurrentNormMtx@0x108 — decomp J3DShape.cpp/J3DSys.hpp). The vertex
    // array bases (j3dSys.unk10C/110/114) are model-level state set BEFORE shape draws, and ngx
    // reads VCD/VAT + per-element matrices straight from the guest objects — so nothing else from
    // the original draw is needed. This removes the largest recomp consumer from the render path.
    if (g_native_draw) {
        const u32 viewp = r32(sh + 0x58);                 // J3DShape::mCurrentViewNo (u32*)
        const u32 view  = valid(viewp) ? r32(viewp) : 0;  // *mCurrentViewNo
        const u32 dmArr = r32(sh + 0x50);                 // mDrawMatrices (Mtx**)
        const u32 nmArr = r32(sh + 0x54);                 // mNormMatrices (Mtx33**)
        if (valid(dmArr)) { const u32 m = r32(dmArr + view * 4); mem_w32(J3DSYS + 0x104, m); }
        if (valid(nmArr)) { const u32 m = r32(nmArr + view * 4); mem_w32(J3DSYS + 0x108, m); }
    } else {
        ngx_super(cpu, 0x802e0390u);
    }
    xfmem_draw_observe();   // always-on (oracle too): ground-truth xfmem at draw
    if (g_enabled) capture(sh);
    if ((int)my_idx < SHAPETI_CAP) g_frame_shape_ti[my_idx] = g_cur_tev_index;   // draw#→material
}

// Pure-JIT no-recomp: the draw seam must fire PER-CALL. A pre-hook (return-false) would fire only
// once (Dolphin caches a passthrough block — see purejit_probe), so the capture seam is instead a
// purejit-safe RETURN-TRUE override: IsRecompiled returns true, Run() dispatches ov_j3dshape_draw
// every dispatch (g_native_draw is forced on above, so it never super-calls), then returns to lr so
// Dolphin JIT continues. Marked only when ngx capture is active.
static const bool ov_poll_dbg_mark = [] {
    // diag: force the shineshadow tee purejit-safe even when ngx capture is off (g_enabled false),
    // so SUNBRIGHT_DBG_POLL can observe whether the GAME calls drawShineShadowVolume in NGX_PRESENT=0.
    if (getenv("SUNBRIGHT_DBG_POLL")) mark_override_purejit_safe(0x8027c67cu);
    return true;
}();
static const bool ov_j3dshape_draw_pj = [] {
    if (!g_enabled) return true;
    // The draw seam + every GX/J3D capture tee must fire PER-CALL under purejit (return-true), not
    // once-at-compile (a pre-hook). Mark each tee'd address purejit-safe so IsRecompiled dispatches
    // it via Run() every dispatch; ngx_super() makes the body capture-only under purejit. Keep this
    // list in sync with the ngx_super(cpu, ADDR) call sites above.
    for (u32 a : { 0x802e0390u,   // J3DShape::draw
                   0x802f2d00u,   // J3DGDLoadTexMtx
                   0x80361b20u,   // GXSetFog
                   0x802ee9d4u,   // J3DTexMtx::load
                   0x803601fcu,   // GXLoadTlut
                   0x8035ffb8u,   // GXLoadTexObjPreLoaded
                   0x80360160u,   // GXLoadTexObj
                   0x8035f26cu,   // GXLoadLightObjImm
                   0x8035ea40u,   // GXSetCopyClear
                   0x80362e0cu,   // GXLoadPosMtxImm
                   0x80362e48u,   // GXLoadPosMtxIndx
                   0x8035f6d0u,   // GXSetChanCtrl
                   0x8035f51cu,   // GXSetChanMatColor
                   0x8035f3b4u,   // GXSetChanAmbColor
                   0x802f33a8u,   // J3DGDSetChanAmbColor
                   0x80361dd0u,   // GXSetBlendMode  (gx_stream_own.cpp /gxblend darkening-pass hunt)
                   0x8035ecd0u,   // GXSetDispCopyGamma (gx_stream_own.cpp /gxblend copy-gamma)
                   0x8027c67cu }) // TModelWaterManager::drawShineShadowVolume (plaza pollution capture)
        mark_override_purejit_safe(a);
    return true;
}();

// /gxstate?ti=N — RENDER-STATE DIFF: the GX command stream (the game's actual GX writes, ground
// truth) vs ngx's object-model reconstruction, for material tev_index=N, captured at its draw.
// Answers "at which pipeline step does the rendering differ" with a per-field PASS/**DIFF** verdict
// — no pixels involved. Set the target with /gxstate?ti=N (or SUNBRIGHT_NGX_GXSTATE); the record
// latches on the next frame's draw of that material and is published at the frame boundary.
int sb_ngx_gxstate_dump(char* out, int cap) {
    const GxStateRec& R = g_gxstate_pub;
    int n = snprintf(out, cap, "gxstate: target_ti=%d  have=%d  ti=%d sh=%08x\n",
                     g_gxstate_ti.load(), (int)R.have, R.ti, R.sh);
    if (!R.have) {
        n += snprintf(out+n, cap-n, "  (no snapshot — set /gxstate?ti=N and let one frame draw that material)\n");
        return n;
    }
    // Decode both channel-control words (COLOR0 cc bit layout, see decode_chanctl).
    auto dec = [](u16 cc, const char** ms, const char** as, const char** df, const char** af){
        static const char* MS[2]={"REG","VTX"}; static const char* AS[2]={"REG","VTX"};
        static const char* DF[4]={"NONE","SIGN","CLAMP","?"}; static const char* AF[4]={"NONE","SPEC","NONE2","SPOT"};
        *ms=MS[cc&1]; *as=AS[(cc>>6)&1]; *df=DF[(cc>>7)&3]; *af=AF[(cc>>9)&3]; };
    const char *oms,*oas,*odf,*oaf,*gms,*gas,*gdf,*gaf,*xms,*xas,*xdf,*xaf;
    dec(R.obj_cc,&oms,&oas,&odf,&oaf); dec(R.gx_cc,&gms,&gas,&gdf,&gaf);
    const u16 xfcc16 = (u16)R.xf_cc; const u8 xf_mask=(u8)(((xfcc16>>2)&0x0F)|(((xfcc16>>11)&0x0F)<<4));
    dec(xfcc16,&xms,&xas,&xdf,&xaf);
    auto eq4 = [](const u8 a[4], const u8 b[4]){ return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]; };
    // The authoritative reference for "what GX actually used" is xfmem (GPU-decoded), trustworthy
    // in the oracle. The DIFF verdict compares ngx(obj-block) against xfmem.
    const bool cc_eq = (R.obj_cc == xfcc16);
    n += snprintf(out+n, cap-n,
        "  ── COLOR-CHANNEL CONTROL (COLOR0)  [ngx-obj vs xfmem] ──  %s\n"
        "    ngx(obj-block)  cc=%04x  en=%d mat=%s amb=%s diff=%s attn=%s mask=%02x\n"
        "    xfmem(GPU auth) cc=%04x  en=%d mat=%s amb=%s diff=%s attn=%s mask=%02x  [have=%d]\n"
        "    GX (fn-tee*)    cc=%04x  en=%d mat=%s amb=%s diff=%s attn=%s mask=%02x  [have=%d sets=%lu] (*J3D bypasses fn → stale)\n",
        cc_eq ? "PASS" : "**DIFF**",
        R.obj_cc, (R.obj_cc>>1)&1, oms,oas,odf,oaf, R.obj_mask,
        xfcc16,   (xfcc16>>1)&1,   xms,xas,xdf,xaf, xf_mask, R.xf_have,
        R.gx_cc,  (R.gx_cc>>1)&1,  gms,gas,gdf,gaf, R.gx_mask, R.gx_cc_have, R.gx_cc_sets);
    if (!cc_eq) {
        n += snprintf(out+n, cap-n, "    FIELD DIFFS (ngx vs xfmem):");
        if (((R.obj_cc>>1)&1)!=((xfcc16>>1)&1)) n += snprintf(out+n,cap-n," enable");
        if ((R.obj_cc&1)!=(xfcc16&1))           n += snprintf(out+n,cap-n," matsrc");
        if (((R.obj_cc>>6)&1)!=((xfcc16>>6)&1)) n += snprintf(out+n,cap-n," ambsrc");
        if (((R.obj_cc>>7)&3)!=((xfcc16>>7)&3)) n += snprintf(out+n,cap-n," diffFn");
        if (((R.obj_cc>>9)&3)!=((xfcc16>>9)&3)) n += snprintf(out+n,cap-n," attnFn");
        if (R.obj_mask!=xf_mask)                n += snprintf(out+n,cap-n," lightmask");
        n += snprintf(out+n,cap-n,"\n");
    }
    n += snprintf(out+n, cap-n, "  ── ALPHA0 cc (ngx-block) = %04x ──\n", R.obj_ca);
    // Raw color-block bytes ngx parses — CLOF layout: matColor@0x04(8B) chanNum@0x0C mColorChan[4]@0x0E cull@0x16.
    n += snprintf(out+n, cap-n, "  ── RAW color block @%08x vt=%08x ──\n    bytes[00..1f]:", R.cb_addr, R.cb_vt);
    for (int k=0;k<0x20;k++) n += snprintf(out+n,cap-n," %02x%s", R.cb_raw[k], (k==0x0d)?"[":(k==0x15)?"]":"");
    n += snprintf(out+n,cap-n,"\n    (mColorChan[0..3]@0x0E = COLOR0,ALPHA0,COLOR1,ALPHA1 = %02x%02x %02x%02x %02x%02x %02x%02x)\n",
        R.cb_raw[0x0e],R.cb_raw[0x0f], R.cb_raw[0x10],R.cb_raw[0x11], R.cb_raw[0x12],R.cb_raw[0x13], R.cb_raw[0x14],R.cb_raw[0x15]);
    const u8 xfm[4]={(u8)(R.xf_mat>>24),(u8)(R.xf_mat>>16),(u8)(R.xf_mat>>8),(u8)R.xf_mat};
    const u8 xfa[4]={(u8)(R.xf_amb>>24),(u8)(R.xf_amb>>16),(u8)(R.xf_amb>>8),(u8)R.xf_amb};
    n += snprintf(out+n, cap-n,
        "  ── MATERIAL COLOR (COLOR0)  [ngx-obj vs xfmem] ──  %s\n"
        "    ngx(obj-block)  mat=(%u,%u,%u,%u)\n"
        "    xfmem(GPU auth) mat=(%u,%u,%u,%u)\n"
        "    GX (fn-tee*)    mat=(%u,%u,%u,%u)  [have=%d sets=%lu] (*pointer-bytes if J3D bypasses fn)\n",
        eq4(R.obj_mat,xfm) ? "PASS" : "**DIFF**",
        R.obj_mat[0],R.obj_mat[1],R.obj_mat[2],R.obj_mat[3],
        xfm[0],xfm[1],xfm[2],xfm[3],
        R.gx_mat[0],R.gx_mat[1],R.gx_mat[2],R.gx_mat[3], R.gx_mat_have, R.gx_mat_sets);
    n += snprintf(out+n, cap-n,
        "  ── AMBIENT COLOR (COLOR0)  [ngx-uses vs xfmem] ──  %s\n"
        "    ngx(uses reg)   amb=(%u,%u,%u,%u)\n"
        "    xfmem(GPU auth) amb=(%u,%u,%u,%u)\n"
        "    GX (fn-tee)     amb=(%u,%u,%u,%u)  [have=%d sets=%lu]\n"
        "    J3DGD (J3D path) amb=(%u,%u,%u,%u)  [have=%d sets=%lu]  (0 sets => J3D doesn't set amb here => global reg is the source)\n",
        eq4(R.obj_amb,xfa) ? "PASS" : "**DIFF**",
        R.obj_amb[0],R.obj_amb[1],R.obj_amb[2],R.obj_amb[3],
        xfa[0],xfa[1],xfa[2],xfa[3],
        R.gx_amb[0],R.gx_amb[1],R.gx_amb[2],R.gx_amb[3], R.gx_amb_have, R.gx_amb_sets,
        R.gd_amb[0],R.gd_amb[1],R.gd_amb[2],R.gd_amb[3], R.gd_amb_have, R.gd_amb_sets);
    n += snprintf(out+n, cap-n, "  ── LIGHTS (active per obj mask=%02x) ──\n", R.obj_mask);
    for (int i=0;i<8;i++) if ((R.obj_mask & (1<<i)) || R.lights[i].valid) {
        const auto& L = R.lights[i];
        n += snprintf(out+n, cap-n,
            "    light[%d]%s valid=%d col=(%.3f,%.3f,%.3f) pos=(%.0f,%.0f,%.0f) dir=(%.2f,%.2f,%.2f) cosA=(%.3f,%.3f,%.3f) distA=(%.3g,%.3g,%.3g)\n",
            i, (R.obj_mask&(1<<i))?"*":" ", L.valid, L.col[0],L.col[1],L.col[2],
            L.pos[0],L.pos[1],L.pos[2], L.dir[0],L.dir[1],L.dir[2],
            L.cosA[0],L.cosA[1],L.cosA[2], L.distA[0],L.distA[1],L.distA[2]);
    }
    // ── TEV combiner + PE/blend (coverage stages downstream of lighting) ──
    if (R.tev_have) {
        const NgxTevState& T = R.tev;
        static const char* CIN[16]={"CPREV","APREV","C0","A0","C1","A1","C2","A2","TEXC","TEXA","RASC","RASA","ONE","HALF","KONST","ZERO"};
        static const char* AIN[8] ={"APREV","A0","A1","A2","TEXA","RASA","KONST","ZERO"};
        static const char* DST[4] ={"PREV","C0","C1","C2"};
        n += snprintf(out+n, cap-n, "  ── TEV COMBINER (ngx obj-model) num_stages=%d ──\n", T.num_stages);
        for (int s=0;s<T.num_stages && s<16;s++) {
            const NgxTevStage& st = T.stage[s];
            const u32 ce=st.color_env, ae=st.alpha_env;
            const int ca=(ce>>12)&0xF, cb=(ce>>8)&0xF, cc=(ce>>4)&0xF, cd=ce&0xF;
            const int cbias=(ce>>16)&3, csub=(ce>>18)&1, cscl=(ce>>20)&3, cdst=(ce>>22)&3;
            const int aa=(ae>>13)&7, ab=(ae>>10)&7, ac=(ae>>7)&7, ad=(ae>>4)&7;
            const int abias=(ae>>16)&3, asub=(ae>>18)&1, ascl=(ae>>20)&3, adst=(ae>>22)&3;
            n += snprintf(out+n, cap-n,
                "    s%d tex=map%d/coord%d rasChan=%d kc=%02x ka=%02x\n"
                "       COLOR: %s(d=%s,a=%s,b=%s,c=%s) bias=%d scale<<%d clamp? -> %s\n"
                "       ALPHA: %s(d=%s,a=%s,b=%s,c=%s) bias=%d scale<<%d -> %s\n",
                s, (int8_t)st.texmap, (int8_t)st.texcoord, (int8_t)st.color_chan, st.kcsel, st.kasel,
                csub?"SUB":"ADD", CIN[cd],CIN[ca],CIN[cb],CIN[cc], cbias, cscl, DST[cdst],
                asub?"SUB":"ADD", AIN[ad],AIN[aa],AIN[ab],AIN[ac], abias, ascl, DST[adst]);
        }
        n += snprintf(out+n, cap-n, "    TEVreg CPREV/C0/C1/C2 (S10): ");
        for (int c=0;c<4;c++) n += snprintf(out+n,cap-n,"(%d,%d,%d,%d) ", T.tev_color[c][0],T.tev_color[c][1],T.tev_color[c][2],T.tev_color[c][3]);
        n += snprintf(out+n, cap-n, "\n    KONST0..3: ");
        for (int c=0;c<4;c++) n += snprintf(out+n,cap-n,"(%d,%d,%d,%d) ", T.kcolor[c][0],T.kcolor[c][1],T.kcolor[c][2],T.kcolor[c][3]);
        // ── GX-AUTHORITATIVE combiner (Dolphin bpmem) — the actual BP regs the game programmed via
        // GXSetTevColorIn/Op/etc. Compare ngx's obj-model color_env/alpha_env per stage against these
        // to find a COMBINER divergence (the floor-wash class) at the STATE level, not output pixels.
        n += snprintf(out+n, cap-n, "\n    ── GX (bpmem) combiner vs ngx ── (CAVEAT: bpmem is GP-thread"
                                    " state, ASYNC-LAGGED — a DIFF may be the wrong material's regs,"
                                    " not an ngx bug; corroborate with the rendered image)\n");
        for (int s=0;s<T.num_stages && s<16;s++) {
            const u32 gce = bpmem.combiners[s].colorC.hex & 0xFFFFFF, gae = bpmem.combiners[s].alphaC.hex & 0xFFFFFF;
            const u32 nce = T.stage[s].color_env & 0xFFFFFF, nae = T.stage[s].alpha_env & 0xFFFFFF;
            n += snprintf(out+n, cap-n, "    s%d color_env ngx=%06x gx=%06x %s   alpha_env ngx=%06x gx=%06x %s\n",
                s, nce, gce, nce==gce?"PASS":"**DIFF**", nae, gae, nae==gae?"PASS":"**DIFF**");
        }
        // GX TEV color/konst registers (S11 per channel; tevregs[0]=CPREV,1..3=C0..C2). The wash can
        // hide here: a too-bright TEV const/konst that the combiner adds/modulates.
        n += snprintf(out+n, cap-n, "    GX tevregs RA/BG raw: ");
        for (int c=0;c<4;c++) n += snprintf(out+n,cap-n,"[%08x %08x] ",
            bpmem.tevregs[c].ra.hex, bpmem.tevregs[c].bg.hex);
        // The ACTUAL fragment GLSL ngx runs for this material — verifies the combiner
        // TRANSLATION (scale/clamp/bias), not just the decoded fields. The hand-computed
        // spec is the oracle; this is what ngx really executes per pixel.
        { std::string g = sb_tev_gen_fragment(T);
          n += snprintf(out+n, cap-n, "\n    ── GENERATED FRAGMENT GLSL ──\n%.*s\n", cap-n-64, g.c_str()); }
        const NgxPEState& P = T.pe;
        static const char* BF[8]={"ZERO","ONE","SRCCLR","INVSRCCLR","SRCALPHA","INVSRCALPHA","DSTALPHA","INVDSTALPHA"};
        static const char* CMP[8]={"NEVER","LESS","EQ","LEQ","GREATER","NEQ","GEQ","ALWAYS"};
        n += snprintf(out+n, cap-n,
            "\n  ── PE / BLEND (ngx obj-model) ──\n"
            "    blend mode=%d src=%s dst=%s   (foam coverage: out = frag*src + dst*dst_factor)\n"
            "    alpha_test=%d comp0=%s ref0=%d aop=%d comp1=%s ref1=%d\n"
            "    zmode test=%d func=%s write=%d  cull=%d\n",
            P.blend_mode, (P.src_factor<8?BF[P.src_factor]:"?"), (P.dst_factor<8?BF[P.dst_factor]:"?"),
            P.alpha_test, (P.comp0<8?CMP[P.comp0]:"?"), P.ref0, P.aop, (P.comp1<8?CMP[P.comp1]:"?"), P.ref1,
            P.z_test, (P.z_func<8?CMP[P.z_func]:"?"), P.z_write, P.cull);
        // GX FOG (bpmem) — unmodeled by ngx; fsel!=0 fades far fragments toward fog color.
        // The additive cloud dome is FAR (w→148000); GX fog dims it while ngx draws it full = wash suspect.
        n += snprintf(out+n, cap-n,
            "  ── FOG (ngx does NOT model) ──\n"
            "    GXSetFog TEE (SYNC, authoritative): type=%u(0=GX_FOG_NONE) color=%08x startz=%.1f endz=%.1f calls=%lu\n"
            "    bpmem (LAGS):  fsel=%u proj=%u color=(%u,%u,%u) A=%.5f C=%.3f\n",
            g_fog_type, g_fog_color, g_fog_startz, g_fog_endz, g_fog_calls,
            (u32)bpmem.fog.c_proj_fsel.fsel.Value(), (u32)bpmem.fog.c_proj_fsel.proj.Value(),
            (u32)bpmem.fog.color.r, (u32)bpmem.fog.color.g, (u32)bpmem.fog.color.b,
            bpmem.fog.GetA(), bpmem.fog.GetC());
        // TexGen (UV generation) + bound textures — tiling/checkerboard diagnosis.
        n += snprintf(out+n, cap-n, "  ── TEXGEN (%d coords) + TEXTURES ──\n", R.tg.num);
        n += snprintf(out+n, cap-n,
            "    VTX-ARRAY src: tc0 cls=%u(0=ABSENT) base=%08x type=%u frac=%u | tc1 cls=%u base=%08x type=%u frac=%u\n"
            "      (tc1 src=TEX1 reads the 2nd texcoord array; cls=0 => mesh has NO TEX1 => ngx reads garbage)\n",
            R.tc0cls, R.tc0base, R.tc0type, R.tc0frac,
            R.tc1cls, R.tc1base, R.tc1type, R.tc1frac);
        for (int i=0;i<R.tg.num && i<8;i++) {
            const TexGen& g = R.tg.tg[i];
            n += snprintf(out+n, cap-n, "    tc%d: type=%s src=%d has_mtx=%d mtxsel=%d mtxptr=%08x", i,
                          g.type==0?"MTX3x4":"MTX2x4", g.src, g.has_mtx, g.mtxsel, g.mtxptr);
            if (g.has_mtx) n += snprintf(out+n, cap-n, "\n      ngx(obj mTotalMtx) row0=[%.4f %.4f %.4f %.3f] row1=[%.4f %.4f %.4f %.3f]",
                          g.obj_m[0],g.obj_m[1],g.obj_m[2],g.obj_m[3], g.obj_m[4],g.obj_m[5],g.obj_m[6],g.obj_m[7]);
            // GX ground truth: the texmtx Dolphin's GPU actually loaded (xfmem matrix memory at the
            // selector's XF row; authoritative in the ORACLE run). Scale mismatch here = ngx reads
            // the wrong/stale texmtx → wrong UV tiling = the cloud over-coverage suspect.
            if (R.tg_xf_have[i]) {
                const float* X = R.tg_xf[i];
                n += snprintf(out+n, cap-n, "\n      xfmem(GPU @draw)  row0=[%.4f %.4f %.4f %.3f] row1=[%.4f %.4f %.4f %.3f]",
                              X[0],X[1],X[2],X[3], X[4],X[5],X[6],X[7]);
            }
            // LIVE XF-row (what GX's GPU actually has loaded = last J3DGDLoadTexMtxImm into this
            // row, draw-ordered & SYNCHRONOUS). If self != mtxptr the material did NOT reload its
            // texmtx → GX uses the leaked matrix from src 'self', ngx uses mtxptr's = divergence.
            if (g.live_have) {
                // DIAGNOSTIC ONLY: last DIRECT J3DGDLoadTexMtxImm call into this XF row. NOT what
                // GX uses for DL-baked materials (per-frame DL replays bypass this tee). A mismatch
                // vs mtxptr here is a tee artifact, not the rendered matrix.
                const bool diff = (g.live_self != g.mtxptr + 0x64);
                n += snprintf(out+n, cap-n, "\n      direct-XF(diag, non-auth) scale=(%.4f,%.4f) trans=(%.3f,%.3f) src=%08x seq=%lu %s",
                              g.live_ss, g.live_st, g.live_ts, g.live_tt, g.live_self, g.live_seq,
                              diff ? "(differs from material — DL-baked, tee can't see replay)" : "(matches material)");
            }
            n += snprintf(out+n, cap-n, "\n");
        }
        static const char* WRAP[3]={"CLAMP","REPEAT","MIRROR"};
        for (int m=0;m<8;m++) if (R.tex[m].addr)
            n += snprintf(out+n, cap-n, "    texmap%d: addr=%08x %ux%u fmt=%u tlut=%08x wrapS=%s wrapT=%s minF=%u magF=%u mip=%u mipCnt=%u\n",
                          m, R.tex[m].addr, R.tex[m].w, R.tex[m].h, R.tex[m].fmt, R.tex[m].tlut_addr,
                          R.tex[m].wrap_s<3?WRAP[R.tex[m].wrap_s]:"?", R.tex[m].wrap_t<3?WRAP[R.tex[m].wrap_t]:"?",
                          R.tex[m].min_filter, R.tex[m].mag_filter, R.tex[m].mipmap, R.tex[m].mip_count);
        const UvDbg& U = g_uvdbg_pub;
        if (U.have) {
            n += snprintf(out+n, cap-n,
                "  ── PER-VERTEX UV (texgen output, nv=%lu) ──\n"
                "    v0 rawTEX0=(%.3f,%.3f) rawTEX1=(%.3f,%.3f) -> UV0=(%.3f,%.3f) UV1=(%.3f,%.3f)\n"
                "    UV0 range x[%.3f,%.3f] y[%.3f,%.3f]  (REPEAT wrap: range>1 => tex tiles that many times)\n"
                "    UV1 range x[%.3f,%.3f] y[%.3f,%.3f]\n"
                "    vtx CLR0: alpha(RASA src) range [%.3f,%.3f]  red(RASC if VTX) range [%.3f,%.3f]\n",
                U.nv, U.tex0[0],U.tex0[1], U.tex1[0],U.tex1[1], U.uv0[0],U.uv0[1], U.uv1[0],U.uv1[1],
                U.u0min[0],U.u0max[0],U.u0min[1],U.u0max[1], U.u1min[0],U.u1max[0],U.u1min[1],U.u1max[1],
                U.ca_min,U.ca_max, U.cr_min,U.cr_max);
        }
    }
    return n;
}

// /ngxorder — the displayed batches in DRAW order (the order the present emits them), with ti,
// vcount, epoch, and the material's cc/blend. Maps a /ngxprefix?n=N step to the exact layer it
// adds, so the prefix sweep's delta jump can be attributed to a specific pass/material.
int sb_ngx_order_dump(char* out, int cap) {
    const int f = g_front.load(std::memory_order_acquire);
    const std::vector<NgxRenderBatch>& bs = g_batches[f];
    const int de = g_display_epoch[f];
    int n = snprintf(out, cap, "ngxorder: front=%d total_batches=%zu display_epoch=%d (only drawn ones counted in prefix)\n", f, bs.size(), de);
    int drawn = 0;
    for (size_t b = 0; b < bs.size(); b++) {
        if ((int)bs[b].epoch < de) continue;   // matches the present's display filter
        const int ti = bs[b].tev_index;
        u16 cc = (ti >= 0 && ti < (int)TEVSTATE_CAP) ? g_tev_cc[ti] : 0xFFFF;
        u8 bm=0,sf=0,df=0,at=0; const NgxTevState* T=nullptr;
        { int nstate=0; const NgxTevState* ts=ngx_snap_tevstates(&nstate); if (ti>=0 && ti<nstate) T=&ts[ti]; }
        if (T) { bm=T->pe.blend_mode; sf=T->pe.src_factor; df=T->pe.dst_factor; at=T->pe.alpha_test; }
        if (n < cap - 160)
            n += snprintf(out+n, cap-n, "  [%3d] ti=%-3d nv=%-5u epoch=%u cc=%04x blend=%u src=%u dst=%u atest=%u\n",
                          drawn, ti, bs[b].vcount, bs[b].epoch, cc, bm, sf, df, at);
        drawn++;
    }
    n += snprintf(out+n, cap-n, "  (drawn=%d — /ngxprefix?n=K renders the first K of these)\n", drawn);
    return n;
}

// Probe report (/ngxshape).
// /ngxshapes — dump the published frame's per-shape NDC bboxes, sorted so the shapes nearest the
// TOP of the screen come first. Localizes a misplaced shape (e.g. file-select Mario at screen-top):
// note ngx NDC is Vulkan-style (y=-1 TOP, +1 BOTTOM), so "top" = small nymin/nymax.
int sb_ngx_shapes_dump(char* out, int cap) {
    const int f = g_front.load(std::memory_order_acquire);
    const std::vector<ShapeRec>& rs = g_shaperec[f];
    int n = snprintf(out, cap, "ngxshapes: front=%d shapes=%zu (ndc y: -1=top +1=bottom)\n", f, rs.size());
    // EFB-copy epoch map: which epochs went offscreen (GXCopyTex) vs display (GXCopyDisp).
    n += snprintf(out + n, cap - n, "  copy-events (epoch/gen routing):");
    for (const CopyEvt& e : g_copyevt[f])
        n += snprintf(out + n, cap - n, " [e%u/g%u@pass%u=%s%s]", e.epoch, e.gen, e.pass, e.kind ? "DISP" : "tex", e.clear ? ",CLR" : "");
    n += snprintf(out + n, cap - n, "\n  CLEAR-AWARE: display_gen=%d (the gen at CopyDisp/XFB = the blend stack GX shows)  [ngx present uses display_epoch=%d]",
                  g_display_gen[f], g_display_epoch[f]);
    {   // per-gen shape+vert histogram over batches — which generation holds the visible scene
        unsigned long gv[8] = {0}; unsigned gc[8] = {0};
        for (const NgxRenderBatch& b : g_batches[f]) { /* batch has epoch; map via shaperec gen below */ (void)b; }
        // gen is on ShapeRec; aggregate verts per gen from shaperecs
        for (const ShapeRec& r : g_shaperec[f]) { unsigned g = r.gen < 8 ? r.gen : 7; gv[g] += r.nv; gc[g]++; }
        n += snprintf(out + n, cap - n, "\n  shapes/gen:");
        for (int g = 0; g < 8; g++) if (gc[g]) n += snprintf(out + n, cap - n, " g%d=%luv/%us%s", g, gv[g], gc[g], (g==g_display_gen[f])?"(DISP)":"");
    }
    n += snprintf(out + n, cap - n, "\n  tex-closed epochs:");
    for (int e = 0; e < EPOCH_CAP; e++) if (g_epoch_tex[f][e]) n += snprintf(out + n, cap - n, " %d", e);
    // Per-epoch shape + vertex histogram over the BATCHES (the geometry the present actually draws) —
    // a race-free read of the frozen buffer: shows how the scene distributes across EFB-copy epochs.
    unsigned long ev[EPOCH_CAP] = {0}; unsigned ec[EPOCH_CAP] = {0};
    for (const NgxRenderBatch& b : g_batches[f]) { unsigned e = b.epoch < EPOCH_CAP ? b.epoch : EPOCH_CAP-1; ev[e]+=b.vcount; ec[e]++; }
    n += snprintf(out + n, cap - n, "\n  batch verts/epoch:");
    for (int e = 0; e < EPOCH_CAP; e++) if (ec[e]) n += snprintf(out + n, cap - n, " e%d=%luv/%ub%s", e, ev[e], ec[e], g_epoch_tex[f][e] ? "(tex)" : "(disp)");
    n += snprintf(out + n, cap - n, "\n");
    // index sort by nymin ascending (topmost first)
    std::vector<int> idx(rs.size());
    for (size_t i = 0; i < rs.size(); i++) idx[i] = (int)i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b){ return rs[a].nymin < rs[b].nymin; });
    int shown = 0;
    for (int i : idx) {
        if (n >= cap - 256 || shown >= 60) break;
        const ShapeRec& r = rs[i];
        n += snprintf(out + n, cap - n,
            "  e%u%s pass=%u%s ti=%-3d sh=%08x nv=%-5u cc=%04x tex0=%08x pnmtx=%u  clr0[cls=%u fmt=%u base=%08x mean=(%u,%u,%u,a%u) lum%u..%u] nrm[cls=%u v0=(%.2f,%.2f,%.2f)]  ndc x[%6.2f,%6.2f] y[%6.2f,%6.2f] w[%.1f,%.1f]\n",
            r.epoch, g_epoch_tex[f][r.epoch < EPOCH_CAP ? r.epoch : 0] ? "X" : "=",
            r.pass, r.projtype ? "o" : "p", r.ti, r.sh, r.nv, r.cc, r.tex0, r.pnmtx,
            r.clr0cls, r.clr0fmt, r.clr0base, r.clr0r, r.clr0g, r.clr0b, r.clr0a, r.clr0min, r.clr0max,
            r.nrmcls, r.nrm0[0], r.nrm0[1], r.nrm0[2],
            r.nxmin, r.nxmax, r.nymin, r.nymax, r.wmin, r.wmax);
        // For the sea (ti=12) and foam (ti=18): dump NDC-z range + the per-pass projection's
        // z-row, to compare their depth spaces (foam-over-sea ordering = the wash discriminator).
        if ((r.ti == 12 || r.ti == 18) && n < cap - 400) {
            n += snprintf(out + n, cap - n,
                "       ti=%d sh=%08x pnmtx=%u single_idx=%u tz=%.1f eyeZ[%.1f,%.1f] ndcZ[%.5f,%.5f] proj[10,11,14]=%.5f,%.4f,%.4f\n",
                r.ti, r.sh, r.pnmtx, r.single_idx, r.tz, r.ezmin, r.ezmax, r.zmin, r.zmax,
                r.proj[10], r.proj[11], r.proj[14]);
            n += snprintf(out + n, cap - n,
                "         mv row0=[%.3f %.3f %.3f %.2f] row1=[%.3f %.3f %.3f %.2f] row2=[%.3f %.3f %.3f %.2f]\n"
                "         model p0=(%.1f,%.1f,%.1f) p1=(%.1f,%.1f,%.1f)\n",
                r.mv[0],r.mv[1],r.mv[2],r.mv[3], r.mv[4],r.mv[5],r.mv[6],r.mv[7], r.mv[8],r.mv[9],r.mv[10],r.mv[11],
                r.mp0[0],r.mp0[1],r.mp0[2], r.mp1[0],r.mp1[1],r.mp1[2]);
        }
        shown++;
    }
    return n;
}

// /shapeat?x=&y= — for every published shape whose NDC bbox covers (x,y), front-to-back by NDC-z,
// report the captured COLOR0 chanctrl + matColor/ambColor AND RE-READ the guest J3DMaterial→colorBlock
// chanctrl FRESH (authoritative guest RAM, on the probe thread). This settles the wash paradox:
//   • fresh COLOR0 en bit = is the floor LIT (en=1) or UNLIT (en=0) in guest RAM? (lit ⇒ ngx must light)
//   • captured-cc vs fresh-cc mismatch = ngx's per-shape material capture races (wrong block) — lead (A)
//   • material→+0x20 re-read = did the colorBlock pointer change since capture?
// CLOF (vtable 0x803e0d38): chanNum@0x0C, mColorChan[0]=COLOR0@0x0E (u16 BE), matColor@0x04.
// CLON (0x803e0cd4): mColorChan[0]@0x16, matColor@0x04, ambColor@0x0C.
int sb_ngx_shapeat_dump(char* out, int cap, float qx, float qy) {
    const int f = g_front.load(std::memory_order_acquire);
    const std::vector<ShapeRec>& rs = g_shaperec[f];
    int n = snprintf(out, cap, "shapeat NDC=(%.3f,%.3f) front=%d shapes=%zu  (ndc y:-1=top +1=bottom)\n",
                     qx, qy, f, rs.size());
    std::vector<int> idx;
    for (size_t i = 0; i < rs.size(); i++) {
        const ShapeRec& r = rs[i];
        if (qx >= r.nxmin && qx <= r.nxmax && qy >= r.nymin && qy <= r.nymax) idx.push_back((int)i);
    }
    // front-to-back: smaller NDC-z = nearer (GC depth, LEQ test)
    std::sort(idx.begin(), idx.end(), [&](int a, int b){ return rs[a].zmin < rs[b].zmin; });
    n += snprintf(out + n, cap - n, "  %zu shapes cover the point (front->back):\n", idx.size());
    auto dec = [](u16 cc, char* b){ snprintf(b, 48, "en=%u mat=%s amb=%s mask=%02x",
        (cc>>1)&1, ((cc>>0)&1)?"VTX":"REG", ((cc>>6)&1)?"VTX":"REG",
        (unsigned)(((cc>>2)&0xF)|(((cc>>11)&0xF)<<4))); };
    int shown = 0;
    for (int i : idx) {
        if (n >= cap - 400 || shown >= 24) break;
        const ShapeRec& r = rs[i];
        // FRESH guest re-read: material@+0x20 → colorBlock; its vtable picks the chan offset.
        u32 fresh_cb = valid(r.material) ? r32(r.material + 0x20) : 0;
        u32 fresh_vt = valid(fresh_cb) ? r32(fresh_cb) : 0;
        u16 fresh_c0 = 0xFFFF; u8 fmat[4] = {0};
        if (const u8* B = valid(fresh_cb) ? sb_ram_fast(fresh_cb) : nullptr) {
            u32 chan_off = (fresh_vt == VT_CLON) ? 0x16 : 0x0E;   // CLON vs CLOF (default)
            fresh_c0 = (u16)((B[chan_off] << 8) | B[chan_off + 1]);
            for (int k = 0; k < 4; k++) fmat[k] = B[0x04 + k];
        }
        char capb[48], frb[48]; dec(r.cc, capb); dec(fresh_c0, frb);
        const bool race = (fresh_cb != r.cb_addr) || (fresh_c0 != r.cc);
        n += snprintf(out + n, cap - n,
            "  sh=%08x ti=%-3d nv=%-5u zmin=%.5f ezmax=%.1f nrmcls=%u clr0mean=(%u,%u,%u)\n"
            "    CAPTURED: cc=%04x [%s] matColor=(%u,%u,%u) ambColor=(%u,%u,%u) cb=%08x vt=%08x\n"
            "    FRESH(guest): material=%08x->cb=%08x vt=%08x  cc=%04x [%s] matColor=(%u,%u,%u)  %s\n",
            r.sh, r.ti, r.nv, r.zmin, r.ezmax, r.nrmcls, r.clr0r, r.clr0g, r.clr0b,
            r.cc, capb, r.mat[0],r.mat[1],r.mat[2], r.amb[0],r.amb[1],r.amb[2], r.cb_addr, r.cb_vt,
            r.material, fresh_cb, fresh_vt, fresh_c0, frb, fmat[0],fmat[1],fmat[2],
            race ? "*** CAPTURE RACE (fresh != captured) ***" : "(fresh == captured)");
        // Walk the material's BAKED display list (matpkt@+0x20=DL ptr, +0x24=len) and dump the channel
        // XF loads (numColors 0x1009, ambient 0x100A/B, matColor 0x100C/D, CHANCTRL 0x100E-0x1011) +
        // lights 0x0600-067F. This is the AUTHORITATIVE state GX replays. A DL chanctrl (0x100E) whose
        // en bit (bit1) differs from the block field above = the floor is LIT in GX though the block
        // reads unlit (stale-DL / DL-built-from-different-data bug) — the wash smoking gun.
        const u32 dlp = valid(r.matpkt) ? r32(r.matpkt + 0x20) : 0;
        const u32 dll = valid(r.matpkt) ? r32(r.matpkt + 0x24) : 0;
        n += snprintf(out + n, cap - n, "    BAKED DL @%08x len=%u (matpkt=%08x) XF:", dlp, dll, r.matpkt);
        if (const u8* D = (dll && dll < 0x4000 && valid(dlp)) ? sb_ram_fast(dlp) : nullptr) {
            u32 p = 0;
            while (p < dll && n < cap - 80) {
                const u8 op = D[p];
                if (op == 0x00) { p++; continue; }
                if (op == 0x10) {                                   // XF load: op, cnt-1 (u16), reg (u16), data
                    const u32 cnt = (((u32)D[p+1]<<8)|D[p+2]) + 1;
                    const u32 reg = ((u32)D[p+3]<<8)|D[p+4];
                    for (u32 rr = reg, j = 0; j < cnt && j < 16; j++, rr++) {
                        const bool want = (rr==0x1009)||(rr>=0x100A&&rr<=0x1011)||(rr>=0x0600&&rr<=0x067F);
                        if (want && n < cap - 40) { const u8* d = D + p + 5 + j*4;
                            n += snprintf(out+n, cap-n, " [%04x=%02x%02x%02x%02x]", rr, d[0],d[1],d[2],d[3]); }
                    }
                    p += 5 + cnt*4; continue;
                }
                if (op == 0x61) {                                   // BP load: op, reg(u8), value(u24)
                    const u8 reg = D[p+1];
                    const u32 val = ((u32)D[p+2]<<16)|((u32)D[p+3]<<8)|D[p+4];
                    // TEV color/alpha env (0xC0-0xDF), genMode/numTEV (0x00), TEV konst sel (0xF6-0xF9),
                    // TEV color regs (0xE0-0xE7), blend/PE cmode0 (0x41) — the per-fragment combine GX uses.
                    if ((reg >= 0xC0 && reg <= 0xDF) || reg == 0x00 || reg == 0x41 ||
                        (reg >= 0xE0 && reg <= 0xE7) || (reg >= 0xF6 && reg <= 0xF9)) {
                        if (n < cap - 24) n += snprintf(out+n, cap-n, " bp%02x=%06x", reg, val);
                    }
                    p += 5; continue;
                }
                if (op == 0x08) { p += 5; continue; }               // CP load
                break;                                              // primitive/unknown → stop
            }
        }
        n += snprintf(out + n, cap - n, "\n");
        shown++;
    }
    return n;
}

int sb_ngx_shape_dump(char* out, int cap) {
    int n = snprintf(out, cap,
        "ngx J3DShape capture: %s\n"
        "  calls=%lu  meshes_built=%lu  badcp=%lu  framing_fail=%lu\n"
        "  cumulative: verts=%lu tris=%lu\n"
        "  COLOR1A1: lit verts=%lu, col1!=col0 verts=%lu (2nd channel distinct ⇒ not an inert alias)\n"
        "  last shape: verts=%u tris=%u vstride=%u vcd_lo=%08x vcd_hi=%08x\n"
        "  last pos[0]=(%.3f, %.3f, %.3f)  max_verts/shape=%u\n"
        "  native XF (modelview): xf_verts=%lu  in_front(z<0)=%lu (%.1f%%)  no_mtx=%lu\n"
        "  eye bbox: x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]  last_eye=(%.2f, %.2f, %.2f)\n"
        "  native projection: have_proj=%d  clip.w>0=%lu/%lu  NDC xy in [-1,1]=%lu (%.1f%%)  frame_swaps=%lu\n"
        "  near-clip: tris_in=%lu drop_behind=%lu cut_straddle=%lu | emitted tiny_w(0..1)=%lu min_w=%.4f\n",
        g_enabled ? "ON" : "OFF (set SUNBRIGHT_NGX_SHAPE=1)",
        g_calls, g_meshes, g_badcp, g_fail,
        g_total_verts, g_total_tris,
        g_col1_verts, g_col1_diff_verts,
        g_last_verts, g_last_tris, g_last_vstride, g_last_vcdlo, g_last_vcdhi,
        g_last_pos[0], g_last_pos[1], g_last_pos[2], g_max_verts,
        g_xf_total, g_xf_front, g_xf_total ? 100.0 * (double)g_xf_front / (double)g_xf_total : 0.0,
        g_xf_nomtx,
        g_eye_min[0], g_eye_max[0], g_eye_min[1], g_eye_max[1], g_eye_min[2], g_eye_max[2],
        g_last_eye[0], g_last_eye[1], g_last_eye[2],
        g_have_proj ? 1 : 0, g_ndc_wpos, g_ndc_total,
        g_ndc_inbox, g_ndc_total ? 100.0 * (double)g_ndc_inbox / (double)g_ndc_total : 0.0,
        g_frame_swaps,
        g_clip_in, g_clip_drop, g_clip_cut, g_clip_tiny, g_clip_minw);

    // Projection-type accounting + the sky gradient shape's actual transform (mis-projection RE).
    n += snprintf(out + n, cap - n, "  PROJ sets: perspective=%lu orthographic=%lu (last_ortho_type=%u)\n",
        g_proj_persp, g_proj_ortho, g_last_ortho_type);
    if (g_last_ortho_type) n += snprintf(out + n, cap - n,
        "    last_ortho P=[%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n",
        g_last_ortho[0],g_last_ortho[1],g_last_ortho[2],g_last_ortho[3], g_last_ortho[4],g_last_ortho[5],g_last_ortho[6],g_last_ortho[7],
        g_last_ortho[8],g_last_ortho[9],g_last_ortho[10],g_last_ortho[11], g_last_ortho[12],g_last_ortho[13],g_last_ortho[14],g_last_ortho[15]);
    if (g_skyxf_pub.have) { const SkyXf& s = g_skyxf_pub;
        n += snprintf(out + n, cap - n,
            "  SKY-XF (cc=0701 gradient, nv=%zu pnmtx=%d mtxp=%08x):\n"
            "    pos0=(%.2f,%.2f,%.2f) eye0=(%.1f,%.1f,%.1f) clip0=(%.1f,%.1f,%.2f,%.4g)\n"
            "    M(modelview 3x4)=[%.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f]\n"
            "    P(proj 4x4)=[%.4f %.4f %.4f %.3f / %.4f %.4f %.4f %.3f / %.4f %.4f %.4f %.3f / %.4f %.4f %.4f %.3f]\n",
            s.nv, s.pnmtx, s.mtxp, s.pos0[0],s.pos0[1],s.pos0[2], s.eye0[0],s.eye0[1],s.eye0[2],
            s.clip0[0],s.clip0[1],s.clip0[2],s.clip0[3],
            s.M[0],s.M[1],s.M[2],s.M[3], s.M[4],s.M[5],s.M[6],s.M[7], s.M[8],s.M[9],s.M[10],s.M[11],
            s.P[0],s.P[1],s.P[2],s.P[3], s.P[4],s.P[5],s.P[6],s.P[7], s.P[8],s.P[9],s.P[10],s.P[11], s.P[12],s.P[13],s.P[14],s.P[15]);
    }
    if (g_pnmtxdbg_pub.have) { const PnmtxDbg& d = g_pnmtxdbg_pub;
        n += snprintf(out + n, cap - n,
            "  PNMTX-DBG (first multi-matrix non-sky shape, nv=%zu mp_valid=%d):\n"
            "    matidx vert0=%u  range=[%u..%u]  pos0=(%.2f,%.2f,%.2f)\n"
            "    xfmem[%u] 3x4=[%.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f]\n"
            "    hook [%u] 3x4=[%.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f]\n",
            d.nv, d.mp_valid, d.mi0, d.mi_min, d.mi_max, d.pos0[0],d.pos0[1],d.pos0[2], d.mi0,
            d.row[0],d.row[1],d.row[2],d.row[3], d.row[4],d.row[5],d.row[6],d.row[7], d.row[8],d.row[9],d.row[10],d.row[11],
            d.mi0,
            d.hook[0],d.hook[1],d.hook[2],d.hook[3], d.hook[4],d.hook[5],d.hook[6],d.hook[7], d.hook[8],d.hook[9],d.hook[10],d.hook[11]);
    }
    n += snprintf(out + n, cap - n,
        "  PosMtxIndx hook: calls=%lu  XF_A base=%08x stride=%u  last index=%u slot=%u mp=%08x\n",
        g_pmi_calls, g_pmi_base, g_pmi_stride, g_pmi_index, g_pmi_slot, g_pmi_mp);

    // N5 per-material TEV capture stats.
    n += snprintf(out + n, cap - n,
        "  TEV material: found=%lu none=%lu unknown_vt=%lu  unique_states=%zu\n"
        "  TevBlock vtable histogram:\n",
        g_mat_found, g_mat_none, g_mat_novt, g_tevstates.size());
    for (int i = 0; i < 8 && g_vt_hist_key[i]; i++) {
        const u32 vt = g_vt_hist_key[i];
        const char* nm = vt == VT_TVB1 ? "TVB1" : vt == VT_TVB2 ? "TVB2" :
                         vt == VT_TVB4 ? "TVB4" : vt == VT_TVB16 ? "TV16" : "????";
        n += snprintf(out + n, cap - n, "    %08x %-4s  %u\n", vt, nm, g_vt_hist_cnt[i]);
    }
    // Indirect-texturing reachability: how many captured materials carry an IBLF block
    // with >0 indirect stages (the feature gate before porting mIndTevStage).
    n += snprintf(out + n, cap - n,
        "  IndBlock: full(IBLF)=%lu null(IBLN)=%lu none=%lu unk=%lu  max_stages=%u\n"
        "    IndTexStageNum histogram [0..4]: %u %u %u %u %u\n"
        "    indirect applied (shader-active)=%lu  S/T-matrix stages skipped=%lu\n",
        g_ind_full, g_ind_null, g_ind_none, g_ind_unk, g_ind_stages_max,
        g_ind_stage_hist[0], g_ind_stage_hist[1], g_ind_stage_hist[2],
        g_ind_stage_hist[3], g_ind_stage_hist[4],
        g_ind_applied, g_ind_st_skipped);
    // N6 lighting capture stats.
    n += snprintf(out + n, cap - n,
        "  lighting: light_loads=%lu  lit_verts=%lu (mean_lum=%.3f, mean_diff=%.3f)\n"
        "    flat_verts=%lu (reg=%lu vtx=%lu, mean_lum=%.3f)\n"
        "    last lit: cc=%04x mat=(%u,%u,%u,%u) amb=(%u,%u,%u,%u) illum=(%.2f,%.2f,%.2f) out=(%.2f,%.2f,%.2f,%.2f)\n",
        g_light_loads, g_chan_lit,
        g_chan_lit ? g_lit_lum_sum / g_chan_lit : 0.0,
        g_diff_n ? g_diff_sum / g_diff_n : 0.0,
        g_chan_flat, g_flat_reg, g_flat_vtx,
        g_chan_flat ? g_flat_lum_sum / g_chan_flat : 0.0,
        g_dbg_cc, g_dbg_mat[0], g_dbg_mat[1], g_dbg_mat[2], g_dbg_mat[3],
        g_dbg_amb[0], g_dbg_amb[1], g_dbg_amb[2], g_dbg_amb[3],
        g_dbg_illum[0], g_dbg_illum[1], g_dbg_illum[2],
        g_dbg_out[0], g_dbg_out[1], g_dbg_out[2], g_dbg_out[3]);
    for (int i = 0; i < 8; i++) if (g_light[i].valid) {
        const LightObj& L = g_light[i];
        n += snprintf(out + n, cap - n,
            "    light[%d] col=(%.2f,%.2f,%.2f) pos=(%.0f,%.0f,%.0f) dir=(%.2f,%.2f,%.2f)\n"
            "             cosA=(%.3f,%.3f,%.3f) distA=(%.3g,%.3g,%.3g)\n",
            i, L.color[0], L.color[1], L.color[2], L.pos[0], L.pos[1], L.pos[2],
            L.dir[0], L.dir[1], L.dir[2],
            L.cosA[0], L.cosA[1], L.cosA[2], L.distA[0], L.distA[1], L.distA[2]);
    }
    n += snprintf(out + n, cap - n,
        "    mean |normal|=%.3f (n=%lu)  lit-vert |normal|=%.3f  lit-zero-normal=%lu/%lu (%.1f%%)\n",
        g_nrm_n ? g_nrm_len_sum / g_nrm_n : 0.0, g_nrm_n,
        g_lit_nrm_n ? g_lit_nrm_sum / g_lit_nrm_n : 0.0,
        g_lit_nrm_zero, g_lit_nrm_n, g_lit_nrm_n ? 100.0 * g_lit_nrm_zero / g_lit_nrm_n : 0.0);
    n += snprintf(out + n, cap - n,
        "    sun dot(en,ldir0): mean=%.3f max=%.3f pos_frac=%.1f%% (n=%lu)\n"
        "      @max: en=(%.2f,%.2f,%.2f) ld0=(%.2f,%.2f,%.2f)\n",
        g_sun_ndl_n ? g_sun_ndl_sum / g_sun_ndl_n : 0.0, g_sun_ndl_max,
        g_sun_ndl_n ? 100.0 * g_sun_ndl_pos / g_sun_ndl_n : 0.0, g_sun_ndl_n,
        g_dbg_en[0], g_dbg_en[1], g_dbg_en[2], g_dbg_ld0[0], g_dbg_ld0[1], g_dbg_ld0[2]);
    n += snprintf(out + n, cap - n,
        "    amb_reg[0]=(%u,%u,%u,%u) have=%d sets=%lu  |  GD_amb[0]=(%u,%u,%u,%u) have=%d sets=%lu last=(%u,%u,%u,%u)\n",
        g_amb_reg[0][0], g_amb_reg[0][1], g_amb_reg[0][2], g_amb_reg[0][3],
        g_amb_have[0], g_amb_sets,
        g_gd_amb[0][0], g_gd_amb[0][1], g_gd_amb[0][2], g_gd_amb[0][3], g_gd_amb_have[0], g_gd_amb_sets,
        g_gd_amb_last[0], g_gd_amb_last[1], g_gd_amb_last[2], g_gd_amb_last[3]);
    n += snprintf(out + n, cap - n,
        "    XF_AMB[0]=(%u,%u,%u,%u) have=%d writes=%lu | hist:",
        g_xf_amb[0][0], g_xf_amb[0][1], g_xf_amb[0][2], g_xf_amb[0][3], g_xf_amb_have[0], g_xf_amb_writes);
    for (int i=0;i<8 && g_xf_amb_hist_cnt[i];i++)
        n += snprintf(out+n, cap-n, " %08x×%lu", g_xf_amb_hist_key[i], g_xf_amb_hist_cnt[i]);
    n += snprintf(out + n, cap - n, "\n");
    {   // SKY latch: exact per-draw lighting breakdown of the sky material (cc=0x0686)
        const SkyLatch& S = g_sky_pub;
        n += snprintf(out + n, cap - n,
            "    SKY[cc=%04x have=%d hasNrm=%d] mat=(%.0f,%.0f,%.0f) amb=(%.0f,%.0f,%.0f) "
            "en=(%.2f,%.2f,%.2f) illum=(%.3f,%.3f,%.3f) out=(%.3f,%.3f,%.3f)\n",
            S.cc, S.have, S.hasNrm, S.matc[0], S.matc[1], S.matc[2], S.ambc[0], S.ambc[1], S.ambc[2],
            S.en[0], S.en[1], S.en[2], S.illum[0], S.illum[1], S.illum[2], S.out[0], S.out[1], S.out[2]);
        n += snprintf(out + n, cap - n, "      ca(alpha0)=%04x aVtx=%d matA=%.0f -> col0.a=%.3f  vcol0=(%.2f,%.2f,%.2f,%.2f) amb_reg_live=(%u,%u,%u,%u)\n",
            S.ca, S.ca & 1, S.matc[3], S.out[3],
            S.vcol0[0], S.vcol0[1], S.vcol0[2], S.vcol0[3],
            S.amb_reg_live[0], S.amb_reg_live[1], S.amb_reg_live[2], S.amb_reg_live[3]);
        n += snprintf(out + n, cap - n, "      texgen num=%u:", S.tgnum);
        for (int k=0;k<4 && k<S.tgnum;k++)
            n += snprintf(out + n, cap - n, " tc%d[src=%u type=%u mtx=%u]", k, S.tgsrc[k], S.tgtype[k], S.tgmtx[k]);
        n += snprintf(out + n, cap - n, "\n");
        n += snprintf(out + n, cap - n,
            "      tc0 mtx=[%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n"
            "      UV bbox (n=%lu): tc0 s[%.3f..%.3f] t[%.3f..%.3f]  tc1 s[%.3f..%.3f] t[%.3f..%.3f]\n",
            S.tg0m[0],S.tg0m[1],S.tg0m[2],S.tg0m[3], S.tg0m[4],S.tg0m[5],S.tg0m[6],S.tg0m[7], S.uvn,
            S.uv0min[0],S.uv0max[0],S.uv0min[1],S.uv0max[1],
            S.uv1min[0],S.uv1max[0],S.uv1min[1],S.uv1max[1]);
        for (int j = 0; j < S.nl; j++)
            n += snprintf(out + n, cap - n,
                "      light[%d] col=(%.2f,%.2f,%.2f) ndl=%.3f attn=%.3f diff=%.3f s=%.3f\n",
                S.li[j], S.lcol[j][0], S.lcol[j][1], S.lcol[j][2], S.lndl[j], S.lattn[j], S.ldiff[j],
                S.lattn[j]*S.ldiff[j]);
    }
    n += snprintf(out + n, cap - n, "    colour-block vtables:");
    for (int i = 0; i < 8 && g_cbvt_key[i]; i++)
        n += snprintf(out + n, cap - n, " %08x(%u)", g_cbvt_key[i], g_cbvt_cnt[i]);
    n += snprintf(out + n, cap - n, "\n");
    // Per-distinct-vtable RAW J3DColorBlock bytes — RE the real matColor/ambColor/chan-ctrl offsets.
    for (int i = 0; i < 8 && g_cbvt_key[i] && n < cap - 360; i++) {
        n += snprintf(out + n, cap - n, "    CB vt=%08x @%08x:", g_cbvt_key[i], g_cbvt_blkaddr[i]);
        for (int k = 0; k < 0x48; k++) {
            if ((k & 3) == 0) n += snprintf(out + n, cap - n, " ");
            n += snprintf(out + n, cap - n, "%02x", g_cbvt_raw[i][k]);
        }
        n += snprintf(out + n, cap - n, "\n      MatPkt @%08x:", g_cbvt_matpkt[i]);
        for (int k = 0; k < 0x40; k++) {
            if ((k & 3) == 0) n += snprintf(out + n, cap - n, " ");
            n += snprintf(out + n, cap - n, "%02x", g_cbvt_pktraw[i][k]);
        }
        // Walk the material's GX display list (ptr@+0x20, len@+0x24) and extract the XF register
        // loads — the FAITHFUL per-material ambient (XF 0x100A/B) / matColor (0x100C/D) / light
        // (0x0600+) the game programs. This is the game's OWN data, the native source of GX state.
        const u8* pk = g_cbvt_pktraw[i];
        const u32 dlp = ((u32)pk[0x20]<<24)|((u32)pk[0x21]<<16)|((u32)pk[0x22]<<8)|pk[0x23];
        const u32 dll = ((u32)pk[0x24]<<24)|((u32)pk[0x25]<<16)|((u32)pk[0x26]<<8)|pk[0x27];
        n += snprintf(out + n, cap - n, "\n      DL @%08x len=%u XFloads:", dlp, dll);
        if (const u8* D = (dll && dll < 0x4000) ? sb_ram_fast(dlp) : nullptr) {
            u32 p = 0;
            while (p < dll && n < cap - 120) {
                const u8 op = D[p];
                if (op == 0x00) { p++; continue; }                       // NOP
                if (op == 0x10) {                                         // XF load
                    const u32 cnt = (((u32)D[p+1]<<8)|D[p+2]) + 1;
                    const u32 reg = ((u32)D[p+3]<<8)|D[p+4];
                    // numColors(0x1009), ambient(0x100A/B), matColor(0x100C/D), CHANCTRL(0x100E-0x1011),
                    // and lights(0x0600-0x067F) — the FAITHFUL baked GX channel/lighting state GX replays.
                    // A DL chanctrl (0x100E) that differs from the block field ngx reads = stale-DL bug.
                    for (u32 r = reg, j = 0; j < cnt && j < 16; j++, r++) {
                        const bool want = (r == 0x1009) || (r >= 0x100A && r <= 0x1011) || (r >= 0x0600 && r <= 0x067F);
                        if (want) { const u8* d = D + p + 5 + j*4;
                            n += snprintf(out+n, cap-n, " [%04x=%02x%02x%02x%02x]", r, d[0],d[1],d[2],d[3]); }
                    }
                    p += 5 + cnt*4; continue;
                }
                if (op == 0x61) { p += 5; continue; }                     // BP load
                if (op == 0x08) { p += 5; continue; }                     // CP load
                break;   // primitive / unknown → stop
            }
        }
        n += snprintf(out + n, cap - n, "\n");
    }
    if (g_dbg_cb_have) {
        n += snprintf(out + n, cap - n, "    block vt=%08x raw[0x00..0x23]:", g_dbg_cb_vt);
        for (int k = 0; k < 0x24; k++) n += snprintf(out + n, cap - n, "%s%02x",
            (k % 4 == 0) ? " " : "", g_dbg_cb_raw[k]);
        n += snprintf(out + n, cap - n, "\n");
    }
    {   // batch texture stats (published frame): texmap0 presence + distinct addrs
        const std::vector<NgxRenderBatch>& fb = g_batches[g_front.load(std::memory_order_acquire)];
        unsigned tb_tex0 = 0, tb_none = 0, distinct = 0; u32 seen[64];
        for (const auto& B : fb) {
            if (B.tex[0].addr) {
                tb_tex0++;
                bool f = false; for (unsigned i = 0; i < distinct; i++) if (seen[i] == B.tex[0].addr) { f = true; break; }
                if (!f && distinct < 64) seen[distinct++] = B.tex[0].addr;
            }
            bool any = false; for (int m = 0; m < 8; m++) if (B.tex[m].addr) any = true;
            if (!any) tb_none++;
        }
        n += snprintf(out + n, cap - n,
            "    batches=%zu  with_tex0=%u  untextured=%u  distinct_tex0_addr=%u\n"
            "    distinct addrs bound to texmap0 (lifetime)=%u%s\n",
            fb.size(), tb_tex0, tb_none, distinct,
            g_distinct_n, g_distinct_overflow ? "+ (overflow)" : "");
    }
    n += snprintf(out + n, cap - n, "    GXLoadTexObj by texmap:");
    for (int i = 0; i < 9; i++) if (g_texobj_hist[i]) n += snprintf(out + n, cap - n, " [%d]=%lu", i, g_texobj_hist[i]);
    n += snprintf(out + n, cap - n, "   Preloaded:");
    for (int i = 0; i < 9; i++) if (g_preload_hist[i]) n += snprintf(out + n, cap - n, " [%d]=%lu", i, g_preload_hist[i]);
    n += snprintf(out + n, cap - n, "\n");
    n += snprintf(out + n, cap - n, "    texgen src hist:");
    for (int i = 0; i < 24; i++) if (g_tg_src_hist[i]) {
        const char* nm = i==0?"POS":i==1?"NRM":(i>=4&&i<=11)?"TEX":"?";
        n += snprintf(out + n, cap - n, " %s%d=%lu", nm, i, g_tg_src_hist[i]);
    }
    n += snprintf(out + n, cap - n, "  type:");
    for (int i = 0; i < 12; i++) if (g_tg_type_hist[i]) n += snprintf(out + n, cap - n, " t%d=%lu", i, g_tg_type_hist[i]);
    n += snprintf(out + n, cap - n, "  mtx: id=%lu set=%lu (n=%lu)\n", g_tg_mtx_id, g_tg_mtx_set, g_tg_n);
    // N7 PE block (alpha test + blend + zmode) capture stats + vtable→tag table.
    n += snprintf(out + n, cap - n,
        "  PE block: OP=%lu ED=%lu XL=%lu FL=%lu unknown=%lu none=%lu\n"
        "    alpha_tested=%lu  blended=%lu  no-zwrite=%lu\n",
        g_pe_op, g_pe_ed, g_pe_xl, g_pe_fl, g_pe_unk, g_pe_none,
        g_pe_alpha, g_pe_blend, g_pe_nozwrite);
    for (int i = 0; i < 8 && g_pe_vt[i].vt; i++) {
        const u32 t = g_pe_vt[i].tag;
        const char c[5] = { (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0 };
        n += snprintf(out + n, cap - n,
            "    vt=%08x tag=%08x '%s' getType=%08x [%08x %08x] cnt=%u\n",
            g_pe_vt[i].vt, t, (t >> 24) ? c : "????", g_pe_vt[i].fn,
            g_pe_vt[i].i0, g_pe_vt[i].i1, g_pe_vt[i].cnt);
    }
    n += snprintf(out + n, cap - n, "  num-stages histogram:");
    for (int s = 1; s <= 16; s++) if (g_stage_hist[s])
        n += snprintf(out + n, cap - n, " [%d]=%u", s, g_stage_hist[s]);
    n += snprintf(out + n, cap - n, "\n");
    // Dump the first few captured states' stage-0 combiner registers (sanity).
    int nst = 0; const NgxTevState* sts = ngx_snap_tevstates(&nst);
    for (int i = 0; i < nst && i < 4; i++) {
        n += snprintf(out + n, cap - n,
            "  state[%d]: stages=%u  s0 color_env=%06x alpha_env=%06x map=%u coord=%u chan=%u kc=%02x ka=%02x\n",
            i, sts[i].num_stages, sts[i].stage[0].color_env, sts[i].stage[0].alpha_env,
            sts[i].stage[0].texmap, sts[i].stage[0].texcoord, sts[i].stage[0].color_chan,
            sts[i].stage[0].kcsel, sts[i].stage[0].kasel);
    }
    // FULL dump of states (all stages + konst/tevreg) — combiner audit. Prefer 1-stage.
    int dumped = 0;
    for (int i = 0; i < nst && dumped < 10; i++) {
        const NgxTevState& s = sts[i];
        if (s.num_stages != 1) continue;   // simple materials only (the 46300-draw majority)
        n += snprintf(out + n, cap - n, "  FULL state[%d] stages=%u pe.alpha=%d blend=%d/%d/%d:\n",
                      i, s.num_stages, s.pe.alpha_test, s.pe.blend_mode, s.pe.src_factor, s.pe.dst_factor);
        for (int st = 0; st < s.num_stages; st++)
            n += snprintf(out + n, cap - n,
                "    s%d ce=%06x ae=%06x map=%u coord=%u chan=%u kc=%02x ka=%02x\n",
                st, s.stage[st].color_env, s.stage[st].alpha_env, s.stage[st].texmap,
                s.stage[st].texcoord, s.stage[st].color_chan, s.stage[st].kcsel, s.stage[st].kasel);
        for (int c = 0; c < 4; c++)
            n += snprintf(out + n, cap - n, "    kcolor[%d]=(%u,%u,%u,%u) tevreg[%d]=(%d,%d,%d,%d)\n",
                c, s.kcolor[c][0], s.kcolor[c][1], s.kcolor[c][2], s.kcolor[c][3],
                c, s.tev_color[c][0], s.tev_color[c][1], s.tev_color[c][2], s.tev_color[c][3]);
        dumped++;
    }
    // Ground-truth XF lighting state (what Dolphin's renderer actually uses) for A/B vs our capture.
    {
        auto cu=[&](u32 v){return v;};
        n += snprintf(out+n, cap-n, "  XFMEM lighting (Dolphin ground truth):\n");
        for (int c=0;c<2;c++)
            n += snprintf(out+n, cap-n, "    color[%d].hex=%08x amb=%08x mat=%08x | OURS cc=%04x have=%d amb=(%u,%u,%u) mat=(%u,%u,%u)\n",
                c, xfmem.color[c].hex, cu(xfmem.ambColor[c]), cu(xfmem.matColor[c]),
                g_gx_cc[c], g_gx_cc_have[c], g_amb_reg[c][0],g_amb_reg[c][1],g_amb_reg[c][2],
                g_gx_matcol[c][0],g_gx_matcol[c][1],g_gx_matcol[c][2]);
        for (int i=0;i<4;i++){ const Light&L=xfmem.lights[i];
            n += snprintf(out+n, cap-n, "    L%d col=(%u,%u,%u,%u) cosatt=(%.2f,%.2f,%.2f) distatt=(%.4f,%.4f,%.4f) pos=(%.0f,%.0f,%.0f)\n",
                i, L.color[0],L.color[1],L.color[2],L.color[3], L.cosatt[0],L.cosatt[1],L.cosatt[2],
                L.distatt[0],L.distatt[1],L.distatt[2], L.dpos[0],L.dpos[1],L.dpos[2]); }
    }
    n += snprintf(out+n, cap-n,
        "  CLR0 class hist (verts): notpresent=%lu direct=%lu idx8=%lu idx16=%lu | matsrc: reg=%lu vtx=%lu novalid=%lu | lighting: off=%lu on=%lu\n",
        g_clr0cls_hist[0], g_clr0cls_hist[1], g_clr0cls_hist[2], g_clr0cls_hist[3],
        g_matsrc_hist[0], g_matsrc_hist[1], g_matsrc_hist[2], g_litcfg_hist[0], g_litcfg_hist[1]);
    n += snprintf(out+n, cap-n,
        "  BIGGEST no-normal (map) shape: verts=%zu clr0cls=%u matVtx=%d enable=%d cc=%04x vcol0=(%u,%u,%u)\n",
        g_bigmap_verts, g_bigmap_clr0cls, g_bigmap_matvtx, g_bigmap_en, g_bigmap_cc,
        g_bigmap_vcol[0], g_bigmap_vcol[1], g_bigmap_vcol[2]);
    n += snprintf(out+n, cap-n,
        "  BIGGEST shape overall: verts=%zu cc=%04x (matVtx=%d en=%d) hasNrm=%d clr0cls=%u vcol0=(%u,%u,%u) vcolMean=%.1f\n",
        g_bigany_verts, g_bigany_cc, g_bigany_matvtx, g_bigany_en, g_bigany_hasnrm, g_bigany_clr0cls,
        g_bigany_vcol[0],g_bigany_vcol[1],g_bigany_vcol[2], g_bigany_vcolmean);
    n += snprintf(out+n, cap-n,
        "  CLR0 array base: ours(biggest)=%08x  Dolphin g_main_cp_state[Color0]=%08x stride=%u\n",
        g_bigany_clr0base, g_main_cp_state.array_bases[CPArray::Color0],
        g_main_cp_state.array_strides[CPArray::Color0]);
    n += snprintf(out+n, cap-n,
        "  CLR0 VAT fmt hist (verts): 565=%lu 888=%lu 888x=%lu 4444=%lu 6666=%lu 8888=%lu\n",
        g_clr0fmt_hist[0],g_clr0fmt_hist[1],g_clr0fmt_hist[2],g_clr0fmt_hist[3],g_clr0fmt_hist[4],g_clr0fmt_hist[5]);
    n += snprintf(out+n, cap-n,
        "  col0 lum by category (verts,avg): reg/flat=(%lu,%.2f) reg/lit=(%lu,%.2f) vtx/flat=(%lu,%.2f) vtx/lit=(%lu,%.2f) noblock=(%lu,%.2f)\n",
        g_colcat_n[0], g_colcat_n[0]?g_colcat_sum[0]/g_colcat_n[0]:0.0,
        g_colcat_n[1], g_colcat_n[1]?g_colcat_sum[1]/g_colcat_n[1]:0.0,
        g_colcat_n[2], g_colcat_n[2]?g_colcat_sum[2]/g_colcat_n[2]:0.0,
        g_colcat_n[3], g_colcat_n[3]?g_colcat_sum[3]/g_colcat_n[3]:0.0,
        g_colcat_n[4], g_colcat_n[4]?g_colcat_sum[4]/g_colcat_n[4]:0.0);
    n += snprintf(out+n, cap-n,
        "  PNMTXIDX shapes=%lu verts=%lu maxnelem=%u | mtxsrc=%d(0=pkt,1=posmtx,2=mv) pkt_applied=%lu fallback=%lu | posmtx_loads=%lu applied=%lu nz=%lu | bigShape verts=%zu pnmtx=%d vcd=%08x posbase=%08x stride=%u\n"
        "    bigShape model-pos bbox: x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]  pos0=(%.1f,%.1f,%.1f)\n",
        g_pnmtx_shapes, g_pnmtx_verts, g_pnmtx_maxnelem, g_ngx_mtxsrc, g_pkt_applied, g_pkt_fallback, g_posmtx_loads, g_pnmtx_applied, g_pnmtx_nz,
        g_bigany_verts, g_bigany_pnmtx, g_bigany_vcd, g_bigany_posbase, g_bigany_posstride,
        g_bigany_pmin[0],g_bigany_pmax[0],g_bigany_pmin[1],g_bigany_pmax[1],g_bigany_pmin[2],g_bigany_pmax[2],
        g_bigany_pos0[0],g_bigany_pos0[1],g_bigany_pos0[2]);
    if (g_clrdbg_ti >= 0)
        n += snprintf(out+n, cap-n,
            "  CLRDBG ti=%d have=%d cc=%04x matVtx=%d ambVtx=%d nlights=%d | RAW vcol0=(%.3f,%.3f,%.3f,%.3f) | illum=(%.3f,%.3f,%.3f) mat=(%.3f,%.3f,%.3f) | OUT=(%.3f,%.3f,%.3f,%.3f)\n",
            g_clrdbg_ti, (int)g_clrdbg_have, g_clrdbg_cc, g_clrdbg_matvtx, g_clrdbg_ambvtx, g_clrdbg_nl,
            g_clrdbg_vcol[0],g_clrdbg_vcol[1],g_clrdbg_vcol[2],g_clrdbg_vcol[3],
            g_clrdbg_illum[0],g_clrdbg_illum[1],g_clrdbg_illum[2], g_clrdbg_mat[0],g_clrdbg_mat[1],g_clrdbg_mat[2],
            g_clrdbg_out[0],g_clrdbg_out[1],g_clrdbg_out[2],g_clrdbg_out[3]);
    if (g_clrdbg_ti >= 0)
        n += snprintf(out+n, cap-n,
            "    CLRDBG light: amb=(%.3f,%.3f,%.3f) l0=#%d col=(%.3f,%.3f,%.3f) ndl=%.3f attn=%.3f s=%.3f  (illum = amb + s*col)\n",
            g_clrdbg_amb[0],g_clrdbg_amb[1],g_clrdbg_amb[2], g_clrdbg_l0i,
            g_clrdbg_l0col[0],g_clrdbg_l0col[1],g_clrdbg_l0col[2],
            g_clrdbg_l0ndl, g_clrdbg_l0attn, g_clrdbg_l0s);
    n += snprintf(out+n, cap-n,
        "  SHRED metric (eye-space max edge per skinned shape): max=%.1f @shape=%08x pkt=%u mi=%u,%u | last=%.1f | buckets <500=%lu <2k=%lu <10k=%lu >=10k=%lu\n"
        "  SHRED metric (NDC/screen max edge, front tris): max=%.2f @shape=%08x | buckets <2=%lu <8=%lu <40=%lu >=40=%lu\n"
        "    NDC-worst vert: w=%.5f eye=(%.2f,%.2f,%.2f) matidx=%u pkt=%u usepkt=%d pos=(%.2f,%.2f,%.2f)\n"
        "      M row0=[%.4f %.4f %.4f %.2f] row1=[%.4f %.4f %.4f %.2f] row2=[%.4f %.4f %.4f %.2f]\n"
        "      shape eye-bbox: x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f] nv=%u  (z near 0 = at camera)\n"
        "      ngx proj used: type=%d(0=persp) P=[%.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f]\n"
        "  SHRED metric (POST-CLIP emitted NDC edge): max=%.2f @shape=%08x | buckets <2=%lu <8=%lu <40=%lu >=40=%lu\n",
        g_shred_max, g_shred_shape, g_shred_pkt, g_shred_mi0, g_shred_mi1, g_shred_last,
        g_shred_n[0], g_shred_n[1], g_shred_n[2], g_shred_n[3],
        g_shred_ndc_max, g_shred_ndc_shape, g_shred_ndc_n[0], g_shred_ndc_n[1], g_shred_ndc_n[2], g_shred_ndc_n[3],
        g_shred_ndc_w, g_shred_ndc_eye[0], g_shred_ndc_eye[1], g_shred_ndc_eye[2], g_shred_ndc_mi, g_shred_ndc_pkt, (int)g_shred_ndc_usepkt,
        g_shred_ndc_pos[0], g_shred_ndc_pos[1], g_shred_ndc_pos[2],
        g_shred_ndc_M[0],g_shred_ndc_M[1],g_shred_ndc_M[2],g_shred_ndc_M[3],
        g_shred_ndc_M[4],g_shred_ndc_M[5],g_shred_ndc_M[6],g_shred_ndc_M[7],
        g_shred_ndc_M[8],g_shred_ndc_M[9],g_shred_ndc_M[10],g_shred_ndc_M[11],
        g_shred_ndc_ebb[0],g_shred_ndc_ebb[1],g_shred_ndc_ebb[2],g_shred_ndc_ebb[3],g_shred_ndc_ebb[4],g_shred_ndc_ebb[5],g_shred_ndc_nv,
        g_shred_ndc_projtype,
        g_shred_ndc_P[0],g_shred_ndc_P[1],g_shred_ndc_P[2],g_shred_ndc_P[3],g_shred_ndc_P[4],g_shred_ndc_P[5],g_shred_ndc_P[6],g_shred_ndc_P[7],
        g_shred_ndc_P[8],g_shred_ndc_P[9],g_shred_ndc_P[10],g_shred_ndc_P[11],g_shred_ndc_P[12],g_shred_ndc_P[13],g_shred_ndc_P[14],g_shred_ndc_P[15],
        g_shred_post_max, g_shred_post_shape, g_shred_post_n[0], g_shred_post_n[1], g_shred_post_n[2], g_shred_post_n[3]);
    n += snprintf(out+n, cap-n,
        "  SHRED metric (ALL shapes, POST-CLIP emitted NDC edge): max=%.2f @shape=%08x projtype=%d(0=persp) pnmtx=%d cc=%04x tex0=%08x | buckets <2=%lu <8=%lu <40=%lu >=40=%lu | >=40 by projtype: persp=%lu ortho=%lu\n"
        "    @max emitted tri: ndc0=(%.2f,%.2f) ndc1=(%.2f,%.2f) ndc2=(%.2f,%.2f)  w=(%.4f,%.4f,%.4f)\n",
        g_shred_all_max, g_shred_all_shape, g_shred_all_projtype, (int)g_shred_all_pnmtx, g_shred_all_cc, g_shred_all_tex0,
        g_shred_all_n[0], g_shred_all_n[1], g_shred_all_n[2], g_shred_all_n[3], g_shred_all_npersp, g_shred_all_northo,
        g_shred_all_ndc[0],g_shred_all_ndc[1],g_shred_all_ndc[2],g_shred_all_ndc[3],g_shred_all_ndc[4],g_shred_all_ndc[5],
        g_shred_all_triw[0],g_shred_all_triw[1],g_shred_all_triw[2]);
    n += snprintf(out+n, cap-n,
        "  up-facing reg-lit illum (visible floor/ground): avg=%.3f max=%.3f (n=%lu) @max amb=%.3f ndl=%.3f\n",
        g_uplit_n?g_uplit_sum/g_uplit_n:0.0, g_uplit_max, g_uplit_n, g_uplit_amb, g_uplit_ndl);
    // CLR0 vertex-color array (J3DSYS+0x114) — indexed map-geometry colours. If this base is
    // wrong/stale, indexed CLR0 lookups fail → white default → washed-out map geometry.
    {
        const u32 ca = r32(J3DSYS + 0x114);
        n += snprintf(out+n, cap-n, "  CLR0 array base=%08x:", ca);
        if (valid(ca)) for (int i = 0; i < 6; i++) n += snprintf(out+n, cap-n, " %08x", r32(ca + i*4));
        n += snprintf(out+n, cap-n, "\n");
    }
    // EFB copy-clear: ours (captured GXSetCopyClear, latest) vs Dolphin bpmem (GPU ground truth).
    n += snprintf(out+n, cap-n, "  COPY-CLEAR: ngx=(%.0f,%.0f,%.0f,%.0f) r3=%08x r4=%08x [r3]=%08x sets=%lu | bpmem AR=%08x GB=%08x\n",
        g_copy_clear[0]*255, g_copy_clear[1]*255, g_copy_clear[2]*255, g_copy_clear[3]*255,
        g_copy_clear_arg, g_copy_clear_arg4, g_copy_clear_deref, g_copy_clear_sets, bpmem.clearcolorAR, bpmem.clearcolorGB);
    // GX fog state (bpmem) — prime suspect for a global darkening ngx skips.
    n += snprintf(out+n, cap-n, "  FOG (bpmem): fsel=%u proj=%u color(rgb)=(%u,%u,%u) A=%.4f C=%.2f b_mag=%u b_shift=%u\n",
        (u32)bpmem.fog.c_proj_fsel.fsel.Value(), (u32)bpmem.fog.c_proj_fsel.proj.Value(),
        (u32)bpmem.fog.color.r, (u32)bpmem.fog.color.g, (u32)bpmem.fog.color.b,
        bpmem.fog.GetA(), bpmem.fog.GetC(), bpmem.fog.b_magnitude, bpmem.fog.b_shift);
    // EFB→XFB copy filter: the XFB-copy deflicker taps; gx renders through this, ngx bypasses it.
    // combined = prev*c[0..1] + cur*c[2..4] + next*c[5..6], then >>6 (÷64). Sum<64 ⇒ copy darkens
    // gx but not ngx (a candidate uniform wash factor). dispcopyyscale also affects vertical copy.
    { auto cf = bpmem.copyfilter.GetCoefficients(); int s=0; for(int i=0;i<7;i++) s+=cf[i];
      n += snprintf(out+n, cap-n, "  COPY filter coefs=[%u,%u,%u,%u,%u,%u,%u] sum=%d (/64=%.3f) yscale=%u gamma-not-checked-here\n",
        cf[0],cf[1],cf[2],cf[3],cf[4],cf[5],cf[6], s, s/64.0, bpmem.dispcopyyscale); }
    // DBG floor-vertex lighting breakdown (cc, ambient, per-light contribution, final illum).
    n += snprintf(out+n, cap-n, "  FLOOR-vert lighting: cc=%04x amb=(%.2f,%.2f,%.2f) lights=%d:\n",
        g_dbgL_cc, g_dbgL_amb[0], g_dbgL_amb[1], g_dbgL_amb[2], g_dbgL_n);
    for (int i = 0; i < g_dbgL_n; i++)
        n += snprintf(out+n, cap-n, "    light[%d] attn=%.3f ndl=%+.3f dist=%.0f contrib(r)=%.3f\n",
            g_dbgL_i[i], g_dbgL_attn[i], g_dbgL_ndl[i], g_dbgL_dist[i], g_dbgL_contrib[i]);
    n += snprintf(out+n, cap-n, "    final illum=(%.3f,%.3f,%.3f) → out=illum (mat=white)\n",
        g_dbgL_illum[0], g_dbgL_illum[1], g_dbgL_illum[2]);
    // Top batches by approximate screen area (NDC bbox) → identify the dominant on-screen
    // surfaces (floor/buildings) and the material they use. Helps localize the brightness gap.
    {
        const int fb = g_front.load(std::memory_order_acquire);
        const std::vector<NgxRenderVertex>& snap = g_snap[fb];
        const std::vector<NgxRenderBatch>&  bats = g_batches[fb];
        struct BR { float area; float cy; int ti; uint32_t vc; u32 tex0; };
        std::vector<BR> brs;
        for (const auto& B : bats) {
            float xmn=1e9f,xmx=-1e9f,ymn=1e9f,ymx=-1e9f; int cnt=0; double sy=0;
            for (uint32_t v = B.vstart; v < B.vstart + B.vcount && v < snap.size(); v++) {
                const float* c = snap[v].clip; if (c[3] <= 1e-4f) continue;
                float nx=c[0]/c[3], ny=c[1]/c[3];
                if(nx<xmn)xmn=nx; if(nx>xmx)xmx=nx; if(ny<ymn)ymn=ny; if(ny>ymx)ymx=ny; sy+=ny; cnt++;
            }
            if (cnt < 3) continue;
            float area = (xmx-xmn)*(ymx-ymn);
            brs.push_back({area, (float)(sy/cnt), B.tev_index, B.vcount, B.tex[0].addr});
        }
        std::sort(brs.begin(), brs.end(), [](const BR&a,const BR&b){return a.area>b.area;});
        // Decode the top batches' tex0 raw mean (skip degenerate brs[0]) — the visible floor/
        // buildings — to check whether ngx's DECODED texture is itself too bright vs GX.
        int td=0;
        for (size_t bi = 0; bi < brs.size() && td < 4; bi++) {
            const NgxRenderBatch* bp = nullptr;
            for (const auto& BB : bats) if (BB.tev_index == brs[bi].ti && BB.tex[0].addr) { bp = &BB; break; }
            if (!bp) continue;
            const NgxTexBind& t = bp->tex[0];
            if (!t.w || !t.h || t.w > 1024 || t.h > 1024) continue;
            const unsigned char* src = sb_ram_fast(t.addr);
            const unsigned char* tl = t.tlut_addr ? sb_ram_fast(t.tlut_addr) : nullptr;
            if (!src) continue;
            std::vector<uint32_t> px((size_t)t.w*t.h);
            sb_tex_decode(px.data(), src, t.w, t.h, t.fmt, tl, t.tlut_fmt);
            double r=0,g=0,b=0; for (uint32_t v:px){r+=v&0xFF;g+=(v>>8)&0xFF;b+=(v>>16)&0xFF;}
            size_t nn=px.size();
            n += snprintf(out+n, cap-n, "  TEX0[ti=%d area=%.2f] %08x fmt=%u %ux%u rawmean=(%.0f,%.0f,%.0f)=%.0f\n",
                brs[bi].ti, brs[bi].area, t.addr, t.fmt, t.w, t.h, r/nn, g/nn, b/nn, (r+g+b)/3/nn);
            td++;
        }
        n += snprintf(out+n, cap-n, "  TOP batches by screen area (ndc) [vmean=w>0 visible mean rgb, blend/atest from PE]:\n");
        for (size_t i = 0; i < brs.size() && i < 16; i++) {
            const BR& b = brs[i];
            const NgxTevState* s = (b.ti>=0 && b.ti<nst) ? &sts[b.ti] : nullptr;
            // visible (w>0) mean rgb across ALL batches of this tev_index
            double vr=0,vg=0,vb=0; unsigned vn=0;
            for (const auto& B2:bats) if (B2.tev_index==b.ti)
                for (uint32_t v=B2.vstart; v<B2.vstart+B2.vcount && v<snap.size(); v++)
                    if (snap[v].clip[3]>1e-4f){ vr+=snap[v].rgba[0]; vg+=snap[v].rgba[1]; vb+=snap[v].rgba[2]; vn++; }
            const u16 bcc = (b.ti>=0 && b.ti<(int)TEVSTATE_CAP) ? g_tev_cc[b.ti] : 0;
            const char* mcat = bcc==0xFFFF ? "noblk" : ((bcc&1)?((bcc&2)?"vtx/lit":"vtx/flat"):((bcc&2)?"reg/lit":"reg/flat"));
            n += snprintf(out+n, cap-n, "    area=%.3f cy=%+.2f ti=%d vc=%u tex0=%08x vmean=(%.2f,%.2f,%.2f) cc=%04x[%s] blend=%d atest=%d %s s0ce=%06x stg=%u\n",
                b.area, b.cy, b.ti, b.vc, b.tex0, vn?vr/vn:0,vn?vg/vn:0,vn?vb/vn:0, bcc, mcat,
                s?s->pe.blend_mode:0, s?s->pe.alpha_test:0,
                s?(s->num_stages==1?"1stage":"multi"):"?",
                s?s->stage[0].color_env:0, s?s->num_stages:0);
        }
        // FULL combiner dump of the BIGGEST batch (the sky) — all stages + konst/tevreg,
        // so we can audit the exact 2-stage combiner vs Dolphin's TEV for the wash.
        if (!brs.empty() && brs[0].ti >= 0 && brs[0].ti < nst) {
            const NgxTevState& s = sts[brs[0].ti];
            n += snprintf(out+n, cap-n, "  BIGGEST-BATCH TEV [ti=%d] stages=%u pe.alpha=%d blend=%d/%d/%d ztest=%d:\n",
                brs[0].ti, s.num_stages, s.pe.alpha_test, s.pe.blend_mode, s.pe.src_factor, s.pe.dst_factor, s.pe.z_test);
            n += snprintf(out+n, cap-n, "    alpha-test: comp0=%u ref0=%u aop=%u comp1=%u ref1=%u (GXCompare 0=NEVER 4=GT 6=GEQ 7=ALWAYS)\n",
                s.pe.comp0, s.pe.ref0, s.pe.aop, s.pe.comp1, s.pe.ref1);
            for (int st = 0; st < s.num_stages && st < 16; st++)
                n += snprintf(out+n, cap-n,
                    "    s%d ce=%06x ae=%06x map=%u coord=%u chan=%u kc=%02x ka=%02x\n",
                    st, s.stage[st].color_env, s.stage[st].alpha_env, s.stage[st].texmap,
                    s.stage[st].texcoord, s.stage[st].color_chan, s.stage[st].kcsel, s.stage[st].kasel);
            for (int c = 0; c < 4; c++)
                n += snprintf(out+n, cap-n, "    kcolor[%d]=(%u,%u,%u,%u) tevreg[%d]=(%d,%d,%d,%d)\n",
                    c, s.kcolor[c][0], s.kcolor[c][1], s.kcolor[c][2], s.kcolor[c][3],
                    c, s.tev_color[c][0], s.tev_color[c][1], s.tev_color[c][2], s.tev_color[c][3]);
            // All texmap bindings used by this batch (decoded mean) — the 2-stage sky
            // samples texmap0 AND texmap1; a mis-bound/mis-decoded texmap1 washes it out.
            const NgxRenderBatch* bp = nullptr;
            for (const auto& BB : bats) if (BB.tev_index == brs[0].ti) { bp = &BB; break; }
            if (bp) for (int tm = 0; tm < 8; tm++) {
                const NgxTexBind& t = bp->tex[tm];
                if (!t.addr) continue;
                double r=0,g=0,b=0,a=0; size_t nn=0; unsigned a_lt128=0, a_ge128=0;
                if (t.w && t.h && t.w<=1024 && t.h<=1024) {
                    const unsigned char* src = sb_ram_fast(t.addr);
                    const unsigned char* tl = t.tlut_addr ? sb_ram_fast(t.tlut_addr) : nullptr;
                    if (src) { std::vector<uint32_t> px((size_t)t.w*t.h);
                        sb_tex_decode(px.data(), src, t.w, t.h, t.fmt, tl, t.tlut_fmt);
                        for (uint32_t v:px){r+=v&0xFF;g+=(v>>8)&0xFF;b+=(v>>16)&0xFF;
                            unsigned av=(v>>24)&0xFF; a+=av; if(av<128)a_lt128++; else a_ge128++;}
                        nn=px.size(); }
                }
                n += snprintf(out+n, cap-n, "    texmap%d %08x fmt=%u %ux%u mean=(%.0f,%.0f,%.0f) a=%.0f  alpha<128:%u >=128:%u\n",
                    tm, t.addr, t.fmt, t.w, t.h, nn?r/nn:0, nn?g/nn:0, nn?b/nn:0, nn?a/nn:0, a_lt128, a_ge128);
                // Also write the decoded texel image to a PPM so we can SEE the gradient.
                if (nn && t.w && t.h && t.w<=1024 && t.h<=1024) {
                    const unsigned char* src = sb_ram_fast(t.addr);
                    const unsigned char* tl = t.tlut_addr ? sb_ram_fast(t.tlut_addr) : nullptr;
                    if (src) { std::vector<uint32_t> px((size_t)t.w*t.h);
                        sb_tex_decode(px.data(), src, t.w, t.h, t.fmt, tl, t.tlut_fmt);
                        char fn[128]; snprintf(fn, sizeof fn, "scratch/screenshots/skytex%d.ppm", tm);
                        if (FILE* f = fopen(fn, "wb")) {
                            fprintf(f, "P6\n%u %u\n255\n", t.w, t.h);
                            for (uint32_t v : px) { unsigned char rgb[3]={(unsigned char)(v&0xFF),
                                (unsigned char)((v>>8)&0xFF),(unsigned char)((v>>16)&0xFF)};
                                fwrite(rgb,1,3,f); }
                            fclose(f);
                        }
                    }
                }
            }
        }
    }
    return n;
}

// ── Pixel→batch probe (/pixbatch?x=NDC&y=NDC) ───────────────────────────────────
// Reliable, deterministic CPU rasterization of ONE pixel against the published
// clip-space batch list — no GPU readback, no AA, no cross-launch camera drift.
// Answers "which captured batch covers this screen point, and in what depth order"
// so we can tell whether the file-select sky pixel is covered by an ngx batch at all
// (hypothesis A: the blue sky shape is uncaptured) and what that batch's material is.
// x,y are GL/GX NDC in [-1,+1] (x right, y UP — sky is near y=+0.9; centre = 0,0).
// We replicate the present's depth contract exactly (mesh.vert.glsl): per-vertex
// Vulkan depth = clip.z/clip.w + 1 (GC NDC z∈[-1 near,0 far] → [0 near,1 far]),
// LEQUAL-style z-test from a 1.0 (far) clear, in batch DRAW ORDER.
// ── Per-pixel CPU full-pipeline blend-stack replay (/pixblend?x=NDC&y=NDC) ──────
// The doctrinal multi-layer no-oracle instrument (handoff #1 tooling gap): rasterize
// EVERY captured batch covering one NDC pixel in the present's EXACT draw order +
// epoch filter, run the GX integer TEV combiner per fragment (mirrors tev_shader.cpp
// write_regular/write_stage), bilinear-sample each stage's texmap at the texgen'd UV,
// z-test, then blend (captured src/dst factors) over the game's copy-clear. Prints the
// per-layer contribution + running accumulator + FINAL = ngx's predicted pixel, so a
// multi-layer wash is back-solved layer by layer vs the GX abshot2 pixel — deterministic,
// no GPU readback, no cross-launch drift. (This is a DIAGNOSTIC mirror of the GPU shader,
// not the shipping path; mismatch CPU-replay-vs-GPU-present localizes a shader/mip bug,
// mismatch CPU-replay-vs-GX localizes a captured-input bug.)
namespace {
inline int sb_clampi(int v, int lo, int hi){ return v<lo?lo:(v>hi?hi:v); }

// The GX combiner bitfield decode + per-stage integer math now live in the shared
// pure evaluator runtime/render/tev_eval.h (used by /pixblend AND render_test).
// Bilinear REPEAT sample of a texmap's base level → RGBA 0..255. (Base-level only; the
// GPU samples a trilinear mip chain, so for a heavily-minified surface compare the FINAL
// against the per-batch texture MEAN reported alongside — a large gap = mip-dependent.)
struct Tx { unsigned char r,g,b,a; };
Tx sample_tex(const NgxTexBind& t, float u, float v){
    Tx z{255,255,255,255};
    if (!t.addr || !t.w || !t.h || t.w>1024 || t.h>1024) return z;
    const unsigned char* host = sb_ram_fast(t.addr); if (!host) return z;
    const unsigned char* tl = t.tlut_addr ? sb_ram_fast(t.tlut_addr) : nullptr;
    static std::vector<uint32_t> px; px.assign((size_t)t.w*t.h, 0);
    sb_tex_decode(px.data(), host, t.w, t.h, t.fmt, tl, t.tlut_fmt);
    auto wrap=[](float c,int n){ float f=c-std::floor(c); int i=(int)(f*n); return (i%n+n)%n; };
    float fu=u*t.w-0.5f, fv=v*t.h-0.5f;
    int x0=(int)std::floor(fu), y0=(int)std::floor(fv);
    float tx=fu-x0, ty=fv-y0;
    auto at=[&](int x,int y)->uint32_t{ int xx=((x%t.w)+t.w)%t.w, yy=((y%t.h)+t.h)%t.h; return px[(size_t)yy*t.w+xx]; };
    (void)wrap;
    uint32_t c00=at(x0,y0),c10=at(x0+1,y0),c01=at(x0,y0+1),c11=at(x0+1,y0+1);
    auto lerp=[&](int sh)->unsigned char{
        float a=(float)((c00>>sh)&0xFF),b=(float)((c10>>sh)&0xFF),cc=(float)((c01>>sh)&0xFF),d=(float)((c11>>sh)&0xFF);
        float top=a+(b-a)*tx, bot=cc+(d-cc)*tx; return (unsigned char)sb_clampi((int)(top+(bot-top)*ty+0.5f),0,255); };
    return { lerp(0), lerp(8), lerp(16), lerp(24) };
}
}  // namespace

int sb_ngx_pixel_blend(float px, float py, char* out, int cap) {
    const int fb = g_front.load(std::memory_order_acquire);
    const std::vector<NgxRenderVertex>& snap = g_snap[fb];
    const std::vector<NgxRenderBatch>&  bats = g_batches[fb];
    int nst = 0; const NgxTevState* sts = ngx_snap_tevstates(&nst);
    const int disp = ngx_snap_display_epoch();
    int n = snprintf(out, cap, "pixblend NDC=(%.3f,%.3f) fb=%d batches=%zu disp_epoch=%d clear=(%.2f,%.2f,%.2f,%.2f)\n",
                     px, py, fb, bats.size(), disp, g_copy_clear[0],g_copy_clear[1],g_copy_clear[2],g_copy_clear[3]);
    auto edge=[](float ax,float ay,float bx,float by,float cx,float cy){ return (cx-ax)*(by-ay)-(cy-ay)*(bx-ax); };
    // Accumulator (framebuffer) in float 0..1, started at the game's copy-clear.
    float acc[4]={g_copy_clear[0],g_copy_clear[1],g_copy_clear[2],g_copy_clear[3]};
    float zbuf=1.0f; int layers=0;
    // GX blend factor → multiplier on (src=fragment, dst=accumulator), per the vk_src/vk_dst map.
    auto bfac=[&](int f,bool is_src,const float frag[4],float ch_idx)->float{
        // ch_idx selects the colour channel value for *_COLOR factors; alpha factors use frag/acc[3].
        switch(f&7){
            case 0: return 0.f; case 1: return 1.f;
            case 2: return is_src ? acc[(int)ch_idx] : frag[(int)ch_idx];        // SRC: DST_COLOR / DST: SRC_COLOR
            case 3: return 1.f-(is_src ? acc[(int)ch_idx] : frag[(int)ch_idx]);  // INV_*_COLOR
            case 4: return frag[3];        // SRC_ALPHA
            case 5: return 1.f-frag[3];    // INV_SRC_ALPHA
            case 6: return acc[3];         // DST_ALPHA
            default:return 1.f-acc[3];     // INV_DST_ALPHA
        }
    };
    for (size_t b=0; b<bats.size(); b++) {
        const NgxRenderBatch& B = bats[b];
        if ((int)B.epoch < disp) continue;                          // present's RT epoch filter
        const int ti = B.tev_index;
        const NgxTevState* S = (ti>=0 && ti<nst) ? &sts[ti] : nullptr;
        if (!S) continue;
        for (uint32_t vi=B.vstart; vi+2<B.vstart+B.vcount && vi+2<snap.size(); vi+=3) {
            const float* c0=snap[vi].clip; const float* c1=snap[vi+1].clip; const float* c2=snap[vi+2].clip;
            if (c0[3]<=1e-5f||c1[3]<=1e-5f||c2[3]<=1e-5f) continue;
            const float x0=c0[0]/c0[3],y0=c0[1]/c0[3], x1=c1[0]/c1[3],y1=c1[1]/c1[3], x2=c2[0]/c2[3],y2=c2[1]/c2[3];
            const float area=edge(x0,y0,x1,y1,x2,y2); if (area>-1e-12f && area<1e-12f) continue;
            float w0=edge(x1,y1,x2,y2,px,py), w1=edge(x2,y2,x0,y0,px,py), w2=edge(x0,y0,x1,y1,px,py);
            const bool inside=(w0<=0&&w1<=0&&w2<=0)||(w0>=0&&w1>=0&&w2>=0); if(!inside) continue;
            w0/=area; w1/=area; w2/=area;
            const float d0=c0[2]/c0[3]+1.f,d1=c1[2]/c1[3]+1.f,d2=c2[2]/c2[3]+1.f;
            const float depth=w0*d0+w1*d1+w2*d2;
            // z-test against running zbuf (LEQUAL-style, captured z_func).
            const int zf=S->pe.z_func&7;
            bool zp = !S->pe.z_test;
            if (S->pe.z_test) switch(zf){case 0:zp=false;break;case 1:zp=depth<zbuf;break;case 2:zp=depth==zbuf;break;
                case 3:zp=depth<=zbuf;break;case 4:zp=depth>zbuf;break;case 5:zp=depth!=zbuf;break;case 6:zp=depth>=zbuf;break;default:zp=true;}
            // interpolate the two raster channels (col0/col1)
            auto interp=[&](float a0,float a1,float a2){ return w0*a0+w1*a1+w2*a2; };
            int col0[4], col1[4];
            for(int k=0;k<4;k++){
                col0[k]=sb_clampi((int)(interp(snap[vi].rgba[k],snap[vi+1].rgba[k],snap[vi+2].rgba[k])*255.f+0.5f),0,255);
                col1[k]=sb_clampi((int)(interp(snap[vi].rgba1[k],snap[vi+1].rgba1[k],snap[vi+2].rgba1[k])*255.f+0.5f),0,255);
            }
            // Run the GX TEV combiner via the SHARED evaluator (tev_eval.h) — identical
            // register mapping (prev=CPREV, c0/c1/c2=tev_color[1..3]) + per-stage integer
            // math as the shipping shader. Texels sampled at the per-stage interpolated UV.
            struct Src : tev_eval::TexelSource {
                const std::vector<NgxRenderVertex>* snap; const NgxRenderBatch* B;
                uint32_t vi; float w0,w1,w2;
                mutable Tx s0tx{255,255,255,255}; mutable float s0u=0,s0v=0; mutable uint32_t s0addr=0;
                bool sample(int stage,int tm,int tc,int out[4]) const override {
                    const auto& sn=*snap;
                    const float u=w0*sn[vi].uv[tc][0]+w1*sn[vi+1].uv[tc][0]+w2*sn[vi+2].uv[tc][0];
                    const float v=w0*sn[vi].uv[tc][1]+w1*sn[vi+1].uv[tc][1]+w2*sn[vi+2].uv[tc][1];
                    Tx tx=sample_tex(B->tex[tm],u,v);
                    out[0]=tx.r;out[1]=tx.g;out[2]=tx.b;out[3]=tx.a;
                    if(stage==0){ s0tx=tx; s0u=u; s0v=v; s0addr=B->tex[tm].addr; }
                    return true;
                }
            } src; src.snap=&snap; src.B=&B; src.vi=vi; src.w0=w0; src.w1=w1; src.w2=w2;
            tev_eval::StageTrace tr[16];
            int outc[4]; tev_eval::eval(*S, col0, col1, src, outc, tr);
            char texinfo[96];
            snprintf(texinfo,sizeof texinfo,"tx%08x(%u,%u,%u,a%u)@uv(%.2f,%.2f)",
                     src.s0addr,src.s0tx.r,src.s0tx.g,src.s0tx.b,src.s0tx.a,src.s0u,src.s0v);
            // Per-stage prev+textemp trace (the alpha-divergence probe — prev.a feeds the
            // later stages; a wrong texture alpha or a register mismap shows up here).
            char stgtr[512]={0}; int stgn=0;
            stgn += snprintf(stgtr+stgn,sizeof stgtr-stgn," | RASC(col0)=(%d,%d,%d,a%d)",col0[0],col0[1],col0[2],col0[3]);
            { int nstg=S->num_stages; if(nstg<1)nstg=1; if(nstg>16)nstg=16;
              for(int sgi=0;sgi<nstg && stgn<(int)sizeof stgtr-72;sgi++)
                stgn += snprintf(stgtr+stgn,sizeof stgtr-stgn," s%d:prev=(%d,%d,%d,a%d)tex=(%d,%d,%d,a%d)",
                    sgi, tr[sgi].prev[0],tr[sgi].prev[1],tr[sgi].prev[2],tr[sgi].prev[3],
                    tr[sgi].textemp[0],tr[sgi].textemp[1],tr[sgi].textemp[2],tr[sgi].textemp[3]); }
            float frag[4]={outc[0]/255.f,outc[1]/255.f,outc[2]/255.f,outc[3]/255.f};
            // texture mean (for the mip-gap check) of the stage-0 texmap.
            double tmean=0; { const NgxTexBind& t=B.tex[S->stage[0].texmap<8?S->stage[0].texmap:0];
                if(t.addr&&t.w&&t.h&&t.w<=1024&&t.h<=1024){const unsigned char* h=sb_ram_fast(t.addr);
                    if(h){static std::vector<uint32_t> pp; pp.assign((size_t)t.w*t.h,0);
                        const unsigned char* tl=t.tlut_addr?sb_ram_fast(t.tlut_addr):nullptr;
                        sb_tex_decode(pp.data(),h,t.w,t.h,t.fmt,tl,t.tlut_fmt);
                        double s=0; for(uint32_t q:pp) s+=((q&0xFF)+((q>>8)&0xFF)+((q>>16)&0xFF))/3.0; tmean=s/pp.size();}}}
            if (n < cap-300) n += snprintf(out+n,cap-n,
                "  L%-2d ti=%-3d ep=%u z=%s(d=%.3f) bm=%u s/d=%u/%u frag=(%.2f,%.2f,%.2f,a%.2f) %s tmean=%.0f",
                layers, ti, B.epoch, zp?"PASS":"fail", depth, S->pe.blend_mode, S->pe.src_factor, S->pe.dst_factor,
                frag[0],frag[1],frag[2],frag[3], texinfo, tmean);
            if (n < cap-560 && S->num_stages>1) n += snprintf(out+n,cap-n,"%s",stgtr);
            if (!zp) { if(n<cap-8) n+=snprintf(out+n,cap-n," (z-culled)\n"); continue; }
            // blend over acc (mode 1 = BLEND with factors; else opaque src=ONE dst=ZERO)
            if (S->pe.blend_mode==1) {
                for(int k=0;k<3;k++){ float sf=bfac(S->pe.src_factor,true,frag,(float)k), df=bfac(S->pe.dst_factor,false,frag,(float)k);
                    acc[k]=sb_clampi((int)((frag[k]*sf+acc[k]*df)*255.f+0.5f),0,255)/255.f; }
                float saf=bfac(S->pe.src_factor,true,frag,3.f), daf=bfac(S->pe.dst_factor,false,frag,3.f);
                acc[3]=sb_clampi((int)((frag[3]*saf+acc[3]*daf)*255.f+0.5f),0,255)/255.f;
            } else if (S->pe.blend_mode==3) { for(int k=0;k<4;k++) acc[k]=sb_clampi((int)((acc[k]-frag[k])*255.f+0.5f),0,255)/255.f; }
            else { for(int k=0;k<4;k++) acc[k]=frag[k]; }                      // opaque overwrite
            if (S->pe.z_test && S->pe.z_write) zbuf=depth;
            layers++;
            if (n<cap-64) n+=snprintf(out+n,cap-n," -> acc=(%.0f,%.0f,%.0f)\n",acc[0]*255,acc[1]*255,acc[2]*255);
        }
    }
    n += snprintf(out+n,cap-n,"FINAL ngx-predicted = (%.0f,%.0f,%.0f)  layers=%d\n",acc[0]*255,acc[1]*255,acc[2]*255,layers);
    return n;
}

int sb_ngx_pixel_batch(float px, float py, char* out, int cap) {
    const int fb = g_front.load(std::memory_order_acquire);
    const std::vector<NgxRenderVertex>& snap = g_snap[fb];
    const std::vector<NgxRenderBatch>&  bats = g_batches[fb];
    int nst = 0; const NgxTevState* sts = ngx_snap_tevstates(&nst);
    int n = snprintf(out, cap, "pixbatch NDC=(%.3f,%.3f) fb=%d batches=%zu verts=%zu\n",
                     px, py, fb, bats.size(), snap.size());

    // Diagnostic mode: px<-900 = "dump a batch's raw clip verts" — py encodes the tev_index
    // (cast to int). Shows clip[4] + NDC + w-sign breakdown so a mis-projected skybox (huge
    // NDC from a near-0 / negative w) is visible directly.
    if (px < -900.f) {
        const int want_ti = (int)py;
        n += snprintf(out + n, cap - n, "BATCH CLIP DUMP for ti=%d:\n", want_ti);
        if (want_ti >= 0 && want_ti < (int)TEVSTATE_CAP && g_tev_tb[want_ti]) {
            const u32 tb = g_tev_tb[want_ti], vt = g_tev_vt[want_ti];
            n += snprintf(out + n, cap - n, "  TevBlock=%08x vt=%08x raw[0..0x80]:\n", tb, vt);
            const u8* B = sb_ram_fast(tb);
            if (B) for (int row = 0; row < 0x80; row += 16) {
                n += snprintf(out + n, cap - n, "    %03x:", row);
                for (int c = 0; c < 16; c++) n += snprintf(out + n, cap - n, " %02x", B[row + c]);
                n += snprintf(out + n, cap - n, "\n");
            }
        }
        if (want_ti >= 0 && want_ti < nst) { const NgxTevState& s = sts[want_ti];
            n += snprintf(out + n, cap - n, "  PE: blend_mode=%u src=%u dst=%u logic=%u | atest=%u comp0=%u ref0=%u aop=%u comp1=%u ref1=%u | z=%u/%u/%u cull=%u\n",
                s.pe.blend_mode, s.pe.src_factor, s.pe.dst_factor, s.pe.logic_op,
                s.pe.alpha_test, s.pe.comp0, s.pe.ref0, s.pe.aop, s.pe.comp1, s.pe.ref1,
                s.pe.z_test, s.pe.z_func, s.pe.z_write, s.pe.cull);
            for (int st = 0; st < s.num_stages && st < 16; st++)
                n += snprintf(out + n, cap - n, "  s%d ce=%06x ae=%06x map=%u coord=%u chan=%u kc=%02x ka=%02x rswap=%u tswap=%u\n",
                    st, s.stage[st].color_env, s.stage[st].alpha_env, s.stage[st].texmap, s.stage[st].texcoord, s.stage[st].color_chan, s.stage[st].kcsel, s.stage[st].kasel,
                    s.stage[st].alpha_env & 3, (s.stage[st].alpha_env >> 2) & 3);
            // Colour-channel detail: matColor/ambColor RGBA + alpha0 ctrl (the RASTER source —
            // RASC/RASA feed the combiner; a wrong raster alpha is the sea-haze wash suspect).
            if (want_ti < (int)TEVSTATE_CAP && g_tev_chan[want_ti].have) { const TevChan& ch = g_tev_chan[want_ti];
                n += snprintf(out + n, cap - n,
                    "  chan: cc(color0)=%04x alpha0=%04x matVtx=%d aVtx=%d matColor=(%u,%u,%u,a%u) ambColor=(%u,%u,%u,a%u)\n",
                    g_tev_cc[want_ti], ch.alpha0, g_tev_cc[want_ti]&1, ch.alpha0&1,
                    ch.mat[0],ch.mat[1],ch.mat[2],ch.mat[3], ch.amb[0],ch.amb[1],ch.amb[2],ch.amb[3]); }
            // TEV color registers (CPREV/C0/C1/C2 — combiner C-inputs) + konst colours.
            for (int c = 0; c < 4; c++)
                n += snprintf(out + n, cap - n, "  tevreg[%d]=(%d,%d,%d,a%d) kcolor[%d]=(%u,%u,%u,a%u)\n",
                    c, s.tev_color[c][0], s.tev_color[c][1], s.tev_color[c][2], s.tev_color[c][3],
                    c, s.kcolor[c][0], s.kcolor[c][1], s.kcolor[c][2], s.kcolor[c][3]);
            // 4 swap tables decoded to RGBA channel selectors (0=R 1=G 2=B 3=A); 0,1,2,3 = identity.
            for (int t = 0; t < 4; t++) { const u8 ix = s.swap_table[t];
                static const char ch[4] = {'R','G','B','A'};
                n += snprintf(out + n, cap - n, "  swaptbl[%d] id=%02x -> %c%c%c%c%s\n", t, ix,
                    ch[(ix>>6)&3], ch[(ix>>4)&3], ch[(ix>>2)&3], ch[ix&3], ix==0x1B?" (identity)":" *NON-IDENTITY*"); }
            // decode each bound texmap + report the batch's per-texcoord UV bbox
            const NgxRenderBatch* bp = nullptr;
            for (const auto& BB : bats) if (BB.tev_index == want_ti) { bp = &BB; break; }
            if (bp) for (int tm = 0; tm < 8; tm++) { const NgxTexBind& t = bp->tex[tm];
                if (!t.addr) continue; double r=0,g=0,b=0,a=0; size_t nn=0;
                if (t.w && t.h && t.w<=1024 && t.h<=1024) { const unsigned char* src = sb_ram_fast(t.addr);
                    const unsigned char* tl = t.tlut_addr ? sb_ram_fast(t.tlut_addr) : nullptr;
                    if (src){ std::vector<uint32_t> px((size_t)t.w*t.h); sb_tex_decode(px.data(), src, t.w, t.h, t.fmt, tl, t.tlut_fmt);
                        for (uint32_t v:px){r+=v&0xFF;g+=(v>>8)&0xFF;b+=(v>>16)&0xFF;a+=(v>>24)&0xFF;} nn=px.size(); } }
                n += snprintf(out + n, cap - n, "  texmap%d %08x fmt=%u %ux%u mean=(%.0f,%.0f,%.0f) a=%.0f\n",
                    tm, t.addr, t.fmt, t.w, t.h, nn?r/nn:0, nn?g/nn:0, nn?b/nn:0, nn?a/nn:0);
            }
            // UV bbox over the batch's verts (per texcoord 0,1)
            float u0mn=1e9f,u0mx=-1e9f,v0mn=1e9f,v0mx=-1e9f,u1mn=1e9f,u1mx=-1e9f,v1mn=1e9f,v1mx=-1e9f;
            for (const auto& BB : bats) if (BB.tev_index == want_ti)
                for (uint32_t vv=BB.vstart; vv<BB.vstart+BB.vcount && vv<snap.size(); vv++){
                    const float* uv=snap[vv].uv[0]; if(uv[0]<u0mn)u0mn=uv[0]; if(uv[0]>u0mx)u0mx=uv[0]; if(uv[1]<v0mn)v0mn=uv[1]; if(uv[1]>v0mx)v0mx=uv[1];
                    const float* uw=snap[vv].uv[1]; if(uw[0]<u1mn)u1mn=uw[0]; if(uw[0]>u1mx)u1mx=uw[0]; if(uw[1]<v1mn)v1mn=uw[1]; if(uw[1]>v1mx)v1mx=uw[1]; }
            n += snprintf(out + n, cap - n, "  UV0 bbox=[%.2f,%.2f]x[%.2f,%.2f] UV1 bbox=[%.2f,%.2f]x[%.2f,%.2f]\n",
                u0mn,u0mx,v0mn,v0mx, u1mn,u1mx,v1mn,v1mx);
        }
        for (size_t bi = 0; bi < bats.size(); bi++) {
            if (bats[bi].tev_index != want_ti) continue;
            const NgxRenderBatch& B = bats[bi];
            unsigned wneg = 0, wsmall = 0, wok = 0; float wmin = 1e30f, wmax = -1e30f;
            float nymin = 1e30f, nymax = -1e30f, nxmin = 1e30f, nxmax = -1e30f;
            double rpos=0,gpos=0,bpos=0, rneg=0,gneg=0,bneg=0;   // mean rgb split by w-sign
            for (uint32_t v = B.vstart; v < B.vstart + B.vcount && v < snap.size(); v++) {
                float w = snap[v].clip[3]; const float* rg = snap[v].rgba;
                if (w < wmin) wmin = w; if (w > wmax) wmax = w;
                if (w <= 0) { wneg++; rneg+=rg[0]; gneg+=rg[1]; bneg+=rg[2]; }
                else if (w < 1e-4f) wsmall++; else { wok++; rpos+=rg[0]; gpos+=rg[1]; bpos+=rg[2];
                    float nx = snap[v].clip[0]/w, ny = snap[v].clip[1]/w;
                    if(nx<nxmin)nxmin=nx; if(nx>nxmax)nxmax=nx; if(ny<nymin)nymin=ny; if(ny>nymax)nymax=ny; }
            }
            n += snprintf(out + n, cap - n, "  draw=%zu vstart=%u vcount=%u w[min=%.4g max=%.4g] wneg=%u wsmall=%u wok=%u  NDC x[%.2f,%.2f] y[%.2f,%.2f]\n",
                bi, B.vstart, B.vcount, wmin, wmax, wneg, wsmall, wok, nxmin, nxmax, nymin, nymax);
            n += snprintf(out + n, cap - n, "    mean rgb: w>0(visible)=(%.2f,%.2f,%.2f) w<=0(clipped)=(%.2f,%.2f,%.2f)\n",
                wok?rpos/wok:0, wok?gpos/wok:0, wok?bpos/wok:0, wneg?rneg/wneg:0, wneg?gneg/wneg:0, wneg?bneg/wneg:0);
            for (uint32_t v = B.vstart; v < B.vstart + B.vcount && v < B.vstart + 9 && v < snap.size(); v++) {
                const float* c = snap[v].clip;
                n += snprintf(out + n, cap - n, "    v%u clip=(%.2f,%.2f,%.2f,%.4g) rgba=(%.2f,%.2f,%.2f,%.2f)\n",
                    v - B.vstart, c[0], c[1], c[2], c[3], snap[v].rgba[0], snap[v].rgba[1], snap[v].rgba[2], snap[v].rgba[3]);
            }
        }
        return n;
    }

    struct Frag { int draw; int ti; float depth; uint8_t ztest, zfunc, zwrite, blend, atest;
                  float rgba[4]; uint32_t tex0; uint16_t cc; };
    std::vector<Frag> frags;

    auto edge = [](float ax, float ay, float bx, float by, float cx, float cy) {
        return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
    };
    for (size_t bi = 0; bi < bats.size(); bi++) {
        const NgxRenderBatch& B = bats[bi];
        const NgxTevState* S = (B.tev_index >= 0 && B.tev_index < nst) ? &sts[B.tev_index] : nullptr;
        for (uint32_t v = B.vstart; v + 2 < B.vstart + B.vcount && v + 2 < snap.size(); v += 3) {
            const float* c0 = snap[v].clip; const float* c1 = snap[v + 1].clip; const float* c2 = snap[v + 2].clip;
            if (c0[3] <= 1e-5f || c1[3] <= 1e-5f || c2[3] <= 1e-5f) continue;   // behind eye / clipped
            const float x0 = c0[0] / c0[3], y0 = c0[1] / c0[3];
            const float x1 = c1[0] / c1[3], y1 = c1[1] / c1[3];
            const float x2 = c2[0] / c2[3], y2 = c2[1] / c2[3];
            const float area = edge(x0, y0, x1, y1, x2, y2);
            if (area > -1e-12f && area < 1e-12f) continue;                       // degenerate
            float w0 = edge(x1, y1, x2, y2, px, py);
            float w1 = edge(x2, y2, x0, y0, px, py);
            float w2 = edge(x0, y0, x1, y1, px, py);
            // inside if all same sign as area (covers both winding orders / no cull here)
            const bool inside = (w0 <= 0 && w1 <= 0 && w2 <= 0) || (w0 >= 0 && w1 >= 0 && w2 >= 0);
            if (!inside) continue;
            w0 /= area; w1 /= area; w2 /= area;                                  // screen-space bary
            const float d0 = c0[2] / c0[3] + 1.f, d1 = c1[2] / c1[3] + 1.f, d2 = c2[2] / c2[3] + 1.f;
            const float depth = w0 * d0 + w1 * d1 + w2 * d2;                     // screen-linear NDC depth
            Frag f{}; f.draw = (int)bi; f.ti = B.tev_index; f.depth = depth; f.tex0 = B.tex[0].addr;
            f.cc = (B.tev_index >= 0 && B.tev_index < (int)TEVSTATE_CAP) ? g_tev_cc[B.tev_index] : 0;
            if (S) { f.ztest = S->pe.z_test; f.zfunc = S->pe.z_func; f.zwrite = S->pe.z_write;
                     f.blend = S->pe.blend_mode; f.atest = S->pe.alpha_test; }
            else   { f.ztest = 1; f.zfunc = 3; f.zwrite = 1; }
            for (int k = 0; k < 4; k++) f.rgba[k] = w0 * snap[v].rgba[k] + w1 * snap[v + 1].rgba[k] + w2 * snap[v + 2].rgba[k];
            frags.push_back(f);
        }
    }
    if (frags.empty()) { n += snprintf(out + n, cap - n, "  NO BATCH COVERS THIS PIXEL (uncaptured → hypothesis A)\n"); return n; }

    // Replay the depth contract in DRAW ORDER to find the visible (last-passing) fragment.
    auto ztest_pass = [](uint8_t func, float frag, float buf) -> bool {
        switch (func) { case 0: return false; case 1: return frag < buf; case 2: return frag == buf;
            case 3: return frag <= buf; case 4: return frag > buf; case 5: return frag != buf;
            case 6: return frag >= buf; default: return true; }   // 7 = ALWAYS
    };
    float zbuf = 1.0f; int winner = -1;
    for (size_t i = 0; i < frags.size(); i++) {                 // frags already in draw order
        const Frag& f = frags[i];
        const bool pass = !f.ztest || ztest_pass(f.zfunc, f.depth, zbuf);
        if (!pass) continue;
        // NOTE: alpha-test discard is texture-dependent (not simulated); flagged below.
        winner = (int)i;
        if (f.zwrite && f.ztest) zbuf = f.depth;
    }

    // Report front-to-back (sorted by depth) for readability; mark the draw-order winner.
    std::vector<int> idx(frags.size()); for (size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b){ return frags[a].depth < frags[b].depth; });
    n += snprintf(out + n, cap - n, "  %zu fragments cover the pixel (front->back; *=draw-order winner):\n", frags.size());
    for (size_t k = 0; k < idx.size() && k < 20; k++) {
        const Frag& f = frags[idx[k]];
        const char* mcat = f.cc==0xFFFF ? "noblk" : ((f.cc&1)?((f.cc&2)?"vtx/lit":"vtx/flat"):((f.cc&2)?"reg/lit":"reg/flat"));
        const char* s0 = "?"; uint32_t s0ce = 0; uint8_t stg = 0;
        if (f.ti >= 0 && f.ti < nst) { s0ce = sts[f.ti].stage[0].color_env; stg = sts[f.ti].num_stages; s0 = stg==1?"1stage":"multi"; }
        n += snprintf(out + n, cap - n,
            "   %c draw=%d ti=%d d=%.4f z=%u/%u/%u blend=%u atest=%u rgba=(%.2f,%.2f,%.2f,%.2f) tex0=%08x cc=%04x[%s] %s s0ce=%06x stg=%u\n",
            idx[k]==winner?'*':' ', f.draw, f.ti, f.depth, f.ztest, f.zfunc, f.zwrite, f.blend, f.atest,
            f.rgba[0], f.rgba[1], f.rgba[2], f.rgba[3], f.tex0, f.cc, mcat, s0, s0ce, stg);
    }
    if (winner >= 0) {
        const Frag& f = frags[winner];
        n += snprintf(out + n, cap - n, "  VISIBLE (draw-order winner): draw=%d ti=%d depth=%.4f rgba=(%.2f,%.2f,%.2f) tex0=%08x atest=%u\n",
            f.draw, f.ti, f.depth, f.rgba[0], f.rgba[1], f.rgba[2], f.tex0, f.atest);
    }
    return n;
}

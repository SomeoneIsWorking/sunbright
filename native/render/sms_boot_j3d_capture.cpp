// sms_boot_j3d_capture.cpp — SLICE 3a of attaching the native renderer to sms-boot.
//
// SLICE 1 rendered the clear colour; SLICE 2 added the immediate-mode 2D (fader/HUD).
// SLICE 3 captures the live 3D scene: the engine's J3D packet walk bottoms out in
// J3DShape::draw(), which in the native build issues GXCallDisplayList (a no-op) — so
// the 3D geometry currently goes nowhere and the frame is black behind the 2D.
//
// This TU provides the capture body sb_boot_capture_j3d(shape), called from a tiny
// SMS_NATIVE_PLATFORM hook at the top of J3DShape::draw() (reference/sms/.../J3DShape.cpp).
// It decodes ONE shape's geometry with the SAME proven recipe as native/render/tests/
// j3dmesh_test.cpp (build_native_cp + ngx_build_mesh, reading NATIVE host-endian struct
// fields), transforms each vertex to Vulkan NDC, and appends the triangles to a
// frame-global buffer. The present hook drains it via sb_boot_capture_j3d_take(), exactly
// mirroring sb_gx_imm_take() (sms_boot_present.cpp).
//
// ---------------------------------------------------------------------------
// 3a TRANSFORM (documented, deliberately simple):
//   The goal of 3a is to prove SOME 3D geometry reaches the frame (nonzero non-clear
//   pixels), NOT a correct camera. We therefore transform each shape with an
//   ORTHO / bounding-box fit over the shape's OWN bbox (J3DShape::unk10..unk1C), exactly
//   like j3dmesh_test's ortho fit (j3dmesh_test.cpp:275-285) — IGNORING the camera/draw
//   matrices entirely. This sidesteps the model-local-vs-model-view question (plan §5.2)
//   for the smallest verifiable step. 3c switches to imm_project() with the captured
//   projection/viewport + the per-shape draw matrix (the camera-correct path); the wiring
//   for that is present below behind SB_J3D_CAMERA=1 but is NOT the 3a default and is
//   UNVERIFIED — see the TODO at use_camera.
//
// ---------------------------------------------------------------------------
// ENDIANNESS (the #1 risk, plan §4) — DECISION, with evidence:
//   The ngx vertex/DL decoders read BIG-ENDIAN and byteswap on every read
//   (runtime/ngx/ngx_vertex.cpp read_comp/bef32; ngx_decode.cpp be16/be32). So the decoder
//   contract is: BOTH the DL stream AND the vertex arrays must be big-endian.
//
//   In the live sms-boot host BMD buffer (after bmd_swap_to_host):
//     * DL stream interior: bmd_swap DEFERS SHP1 display-list swapping → stays BIG-ENDIAN
//       (native/assets/bmd_swap.cpp swap_SHP1 note, lines 309-314). ✅ ngx reads it directly.
//     * Vertex arrays (VTX1): swap_VTX1 (native/assets/bmd_swap.cpp:280-291, swap_run over
//       each array region with the format-derived scalar unit) DOES byteswap the array DATA
//       to HOST-ENDIAN. ✗ This CONTRADICTS J3DModelLoader.cpp:30-32's comment ("deliberately
//       leaves ... vertex-array ... byte-streams big-endian") — the comment is aspirational;
//       the code swaps. ngx would then decode host-endian arrays through a BE reader = garbage.
//
//   ROOT-CAUSE FIX (plan §4 fix B-1, applied by the parent as a reviewed shipping edit):
//   make swap_VTX1 match its own contract — leave the VTX1 array DATA big-endian (keep the
//   structural offset + fmt-list swaps; SKIP the swap_run over the array regions). This is
//   safe: native GX is stubbed, so NOTHING reads array scalars host-endian — only the
//   structural fields (which stay swapped). After that edit, the live arrays are BE and this
//   capture works directly, just like j3dmesh_test (which keeps its own BE copy).
//
//   This TU is written for the POST-FIX world (arrays BE). If SB_J3D_CAPTURE shows garbage
//   geometry (positions far outside the shape bbox / NaN), the swap_VTX1 edit was not applied
//   — that is the gate, not a bug here. (We do NOT re-swap arrays back to BE on the fly here:
//   that would fork the decoder and hide the real contract violation — a bandaid. Fix it at
//   the source.)

#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DVertex.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>   // j3dSys (live array-base overrides + view mtx)

#include "nvk.h"            // NvkVertex
#include "gx_imm_xform.h"   // imm_project, SbImmRawVtx, SbImmVtx (pure transform)
#include "ngx_mesh.h"       // NgxCP, NgxVertex, ngx_build_mesh

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace sb::render;

// Bridge into native gx_impl (added there, mirrors sb_gx_get_clear_color): the captured
// projection + viewport, layout exactly as imm_project() wants.
//   type: 0 = GX_PERSPECTIVE, 1 = GX_ORTHOGRAPHIC
//   proj[0..5] = GXState.projMtx  (== GXGetProjectionv ptr[1..6])
//   vp[0..5]   = {left, top, width, height, nearz, farz}
extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]);

namespace {

bool capture_enabled() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_J3D_CAPTURE"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v != 0;
}
// 3a default = ortho bbox fit (camera-independent). Opt in to the (unverified) camera path
// with SB_J3D_CAMERA=1 once 3a is proven; see the TODO below.
bool use_camera() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_J3D_CAMERA"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v != 0;
}

// Frame-global capture buffer of transformed NDC triangle verts (3N). Clear-on-first-
// append-after-consume — the same latch as gx_imm_impl's g_consumed (sms_boot_present.cpp
// drains via _take, which sets g_consumed=true; the next append clears).
std::vector<NvkVertex> g_tris;
bool g_consumed = true;

// ---- build_native_cp: ported from native/render/tests/j3dmesh_test.cpp:76-132 ----------
// Reads NATIVE J3DShape + J3DVertexData struct fields (host-endian, 64-bit ptrs). Array
// bases become OFFSETS relative to `base` (the model buffer start) for the BE resolver.
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

    auto off = [&](const void* p) -> u32 { return p ? (u32)((const uint8_t*)p - base) : 0; };
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

// Resolver: array_base holds an offset into the model buffer (relative to `base`); the
// array bytes are BIG-ENDIAN (post swap_VTX1 fix) — ngx byteswaps them on read.
struct ResolveCtx { const uint8_t* base; size_t size; };
const unsigned char* resolve_native(unsigned off, void* user) {
    auto* c = (ResolveCtx*)user;
    if (off == 0 || (c->size && off >= c->size)) return nullptr;
    return c->base + off;
}

} // namespace

// ============================ THE CAPTURE BODY ===========================================
// Called from J3DShape::draw() under SMS_NATIVE_PLATFORM (the tap), BEFORE the no-op GX
// issue. At that point the shape packet has already populated mDrawMatrices/mCurrentViewNo
// (J3DShapePacket::draw, reference/sms/.../J3DPacket.cpp), and j3dSys holds the live
// POS/NRM/CLR0 array-base overrides + the view matrix.
//
// Returns true if it captured this shape (the caller may then skip the GX no-op — though
// skipping is moot since GXCallDisplayList does nothing). When SB_J3D_CAPTURE is off it
// early-returns false so a normal run pays nothing.
extern "C" bool sb_boot_capture_j3d(J3DShape* shape) {
    if (!capture_enabled() || !shape) return false;
    // DIAGNOSTIC (SB_J3D_DBG): count calls + early-return reasons so we can see whether the
    // J3DShape::draw tap fires at all and where shapes drop out. Remove once 3a is verified.
    static int dbg = -1;
    if (dbg < 0) { const char* e = std::getenv("SB_J3D_DBG"); dbg = (e && e[0] && e[0] != '0') ? 1 : 0; }
    static long s_calls = 0, s_novd = 0, s_nobase = 0, s_nocp = 0, s_noidx = 0, s_ok = 0, s_tris = 0;
    ++s_calls;
    if (dbg && (s_calls % 2000) == 0)
        std::fprintf(stderr, "[j3dcap] calls=%ld ok=%ld tris=%ld | drop: novd=%ld nobase=%ld nocp=%ld noidx=%ld\n",
                     s_calls, s_ok, s_tris, s_novd, s_nobase, s_nocp, s_noidx);
#define J3DCAP_DROP(ctr) do { ++(ctr); return true; } while (0)
    if (g_consumed) { g_tris.clear(); g_consumed = false; }
    // Safety cap: the present hook drains via _take() once per presented frame, but it only
    // runs while frame-dumping is active (sms_boot_present.cpp). If capture is enabled WITHOUT
    // dumping, _take is never called and the buffer would grow unbounded across frames. Cap it
    // so an undrained run can't OOM; the verify recipe pairs SB_J3D_CAPTURE with SB_FRAME_DUMP
    // (drained every frame), so this never triggers there. (Not a fidelity bandaid — purely an
    // out-of-memory guard for the undrained-config; 3b ties capture lifetime to the present.)
    if (g_tris.size() > 6u * 1024 * 1024) return true;

    J3DVertexData* vd = shape->unk44;   // == the shape's vertex data (public field)
    if (!vd) J3DCAP_DROP(s_novd);

    // Model-buffer base for the resolver. The decoded arrays live in the persisted host BMD
    // buffer; build_native_cp turns each native array pointer into an offset relative to
    // `base`, and resolve_native turns it back — so as long as `base` is a fixed anchor that
    // sits at or before every array, the round-trip is identity. We use the POS array start
    // as that anchor and size=0 (no upper bound) so any array AFTER pos resolves; an array
    // BEFORE pos would give a negative (wrapped) offset.
    // TODO(3b): thread the true J3DModelData buffer base (J3DModelData has the block base)
    // so arrays preceding POS also resolve; for 3a/most rigid plaza shapes POS is first.
    const uint8_t* base = (const uint8_t*)vd->getVtxPosArray();
    if (!base) J3DCAP_DROP(s_nobase);

    NgxCP cp{};
    if (!build_native_cp(shape, *vd, base, cp)) J3DCAP_DROP(s_nocp);

    // Decode every matrix group's DL into native verts + tri indices (BE DL stream).
    ResolveCtx rc{ base, 0 };
    std::vector<NgxVertex> verts;
    std::vector<unsigned> idx;
    for (u16 e = 0; e < shape->getMtxGroupNum(); ++e) {
        J3DShapeDraw* dp = shape->getShapeDraw(e);
        if (!dp || !dp->getDisplayList()) continue;
        ngx_build_mesh(cp, dp->getDisplayList(), dp->getDisplayListSize(),
                       resolve_native, &rc, verts, idx);
    }
    if (idx.empty()) J3DCAP_DROP(s_noidx);
    ++s_ok; s_tris += (long)idx.size();

    // --- transform decoded model-space verts to Vulkan NDC ---
    g_tris.reserve(g_tris.size() + idx.size());

    if (use_camera()) {
        // 3c path (UNVERIFIED — opt-in via SB_J3D_CAMERA): real camera. Feed the per-shape
        // draw matrix as imm_project's current position matrix and the captured projection +
        // viewport. mDrawMatrices is Mtx** (Mtx = f32[3][4]); mDrawMatrices[viewNo] is Mtx*
        // (== f32(*)[4]) = the per-view draw-matrix table; [0] is the rigid (single-packet)
        // matrix — same one J3DShape::draw feeds j3dSys.setModelDrawMtx (J3DShape.cpp:230).
        // TODO(3c): (a) honor per-vertex matidx/packet for skinned shapes; (b) if geometry is
        // DOUBLY-transformed (draw matrix is model-LOCAL not model-view), left-multiply
        // j3dSys.getViewMtx() — the #1 thing to eyeball here (plan §5.2).
        int projType; float proj[6]; float vp[6];
        sb_gx_get_projection(&projType, proj, vp);
        float ident[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
        float posMtx[3][4];
        Mtx* drawTbl = shape->mDrawMatrices ? shape->mDrawMatrices[*shape->mCurrentViewNo] : nullptr;
        if (drawTbl) std::memcpy(posMtx, &drawTbl[0][0], sizeof(posMtx));
        else         std::memcpy(posMtx, ident, sizeof(posMtx));

        for (unsigned i : idx) {
            const NgxVertex& s = verts[i];
            SbImmRawVtx raw{ s.pos[0], s.pos[1], s.pos[2],
                             s.clr[0][0]/255.f, s.clr[0][1]/255.f, s.clr[0][2]/255.f, s.clr[0][3]/255.f };
            if ((s.clr[0][0]|s.clr[0][1]|s.clr[0][2]) == 0) { raw.r = raw.g = raw.b = 0.86f; }
            SbImmVtx p = imm_project(raw, projType, proj, posMtx, vp);
            g_tris.push_back(NvkVertex{ p.x, p.y, p.z, p.r, p.g, p.b, p.a });
        }
        return true;
    }

    // 3a DEFAULT: ortho bbox fit over the shape's OWN bbox (unk10..unk1C), camera-ignored
    // (j3dmesh_test.cpp:275-285). Proves the geometry decodes + rasterizes without depending
    // on any matrix being correct. Maps model XY into [-1,1] NDC (Vulkan +Y down → flip Y so
    // the model isn't upside down); z is set to 0.5 (mid-depth) since 3a has no depth test.
    const Vec& mn = shape->unk10; const Vec& mx = shape->unk1C;
    const float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
    float ex = std::max(mx.x - mn.x, mx.y - mn.y) * 0.55f;
    if (!(ex > 0.f)) ex = 1.f;   // degenerate/zero bbox guard (also catches NaN)

    for (unsigned i : idx) {
        const NgxVertex& s = verts[i];
        float nx = (s.pos[0] - cx) / ex;
        float ny = -(s.pos[1] - cy) / ex;   // Vulkan NDC y is DOWN
        unsigned char r = s.clr[0][0], g = s.clr[0][1], b = s.clr[0][2];
        if ((r | g | b) == 0) { r = g = b = 220; }   // no vertex colour → light grey
        g_tris.push_back(NvkVertex{ nx, ny, 0.5f, r/255.f, g/255.f, b/255.f, 1.f });
    }
    return true;
}

// Present drains the frame's captured scene tris (mirrors sb_gx_imm_take's signature).
// Returns the vertex count (multiple of 3); marks the buffer consumed so the next append
// (next frame's first shape) clears it.
extern "C" int sb_boot_capture_j3d_take(const NvkVertex** out) {
    if (out) *out = g_tris.empty() ? nullptr : g_tris.data();
    int n = (int)g_tris.size();
    g_consumed = true;
    return n;
}

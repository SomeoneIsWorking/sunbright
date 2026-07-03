#include <execinfo.h>
#include <cstdlib>
#include <cstdio>
// gx_impl.cpp — native GX seam, SLICE 1: the transform block (projection/viewport/
// scissor + GXProject). "Rebuild as a PC game": GXSet* capture into the native GXState
// (gx_state.h); the GameCube FIFO / XF-register writes the decomp does are DROPPED —
// the native renderer reads GXState, there is no command stream to emulate.
//
// GXProject is ported verbatim from the decomp (GXTransform.c) — pure eye->screen math.
// GXSetProjection/GXSetViewport/GXSetScissor port only the gx-struct writes (not the
// GX_WRITE_* FIFO pokes). GXGetProjectionv/GXGetViewportv read the state back in the
// exact pm[]/vp[] layout GXProject expects. The full GX surface (TEV/blend/Z/tex/draw/
// present) lands in later slices, routed to the ngx renderer.

#include <dolphin/gx.h>
#include "gx_state.h"
#include "gx_fifo.h"

// Host sink for the GameCube write-gather pipe. The decomp's inline GX FIFO writers
// (GXVert.h: GXCmd*/GXPosition*/...) target this 32-byte bit-bucket natively instead
// of the nonexistent 0xCC008000 MMIO — the native renderer reads the object model, so
// the command stream is dead output. (See the SMS_NATIVE_PLATFORM redirect in GXVert.h.)
volatile unsigned char sb_gx_wgfifo_sink[32];

namespace sb::platform::gx {
GXState& state() { static GXState g_gx{}; return g_gx; }
}

using sb::platform::gx::state;

extern "C" {

// Eye->screen projection (verbatim from the decomp; self-contained pure math).
void GXProject(f32 x, f32 y, f32 z, f32 mtx[3][4], f32* pm, f32* vp,
               f32* sx, f32* sy, f32* sz) {
    f32 ex = mtx[0][3] + ((mtx[0][2]*z) + ((mtx[0][0]*x) + (mtx[0][1]*y)));
    f32 ey = mtx[1][3] + ((mtx[1][2]*z) + ((mtx[1][0]*x) + (mtx[1][1]*y)));
    f32 ez = mtx[2][3] + ((mtx[2][2]*z) + ((mtx[2][0]*x) + (mtx[2][1]*y)));
    f32 xc, yc, zc, wc;
    if (pm[0] == 0.0f) {           // perspective (pm[0] = GX_PERSPECTIVE = 0)
        xc = (ex*pm[1]) + (ez*pm[2]);
        yc = (ey*pm[3]) + (ez*pm[4]);
        zc = pm[6] + (ez*pm[5]);
        wc = 1.0f / -ez;
    } else {                        // orthographic
        xc = pm[2] + (ex*pm[1]);
        yc = pm[4] + (ey*pm[3]);
        zc = pm[6] + (ez*pm[5]);
        wc = 1.0f;
    }
    *sx = (vp[2]/2.0f) + (vp[0] + (wc * (xc * vp[2]/2.0f)));
    *sy = (vp[3]/2.0f) + (vp[1] + (wc * (-yc * vp[3]/2.0f)));
    *sz = vp[5] + (wc * (zc * (vp[5] - vp[4])));
}

void GXSetProjection(f32 mtx[4][4], GXProjectionType type) {
    auto& g = state();
    g.projType   = type;
    g.projMtx[0] = mtx[0][0];
    g.projMtx[2] = mtx[1][1];
    g.projMtx[4] = mtx[2][2];
    g.projMtx[5] = mtx[2][3];
    if (type == GX_ORTHOGRAPHIC) {
        g.projMtx[1] = mtx[0][3];
        g.projMtx[3] = mtx[1][3];
    } else {
        g.projMtx[1] = mtx[0][2];
        g.projMtx[3] = mtx[1][2];
    }
    // Latch the last PERSPECTIVE projection (+ the live viewport) for the native scene
    // render — the HUD overwrites the live projection with an ortho before model calc().
    if (type == GX_PERSPECTIVE) {
        for (int i = 0; i < 6; ++i) g.proj3dMtx[i] = g.projMtx[i];
        // Full row-major 4x4 (C_MTXPerspective authored it) for the clip-space scene project.
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) g.proj3dM44[r*4+c] = mtx[r][c];
        g.proj3dType = type;
        g.proj3dVp[0] = g.vpLeft; g.proj3dVp[1] = g.vpTop;  g.proj3dVp[2] = g.vpWd;
        g.proj3dVp[3] = g.vpHt;   g.proj3dVp[4] = g.vpNearz; g.proj3dVp[5] = g.vpFarz;
        g.proj3dValid = true;
    }
}

void GXGetProjectionv(f32* ptr) {
    auto& g = state();
    ptr[0] = (f32)g.projType;
    ptr[1] = g.projMtx[0];
    ptr[2] = g.projMtx[1];
    ptr[3] = g.projMtx[2];
    ptr[4] = g.projMtx[3];
    ptr[5] = g.projMtx[4];
    ptr[6] = g.projMtx[5];
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz,
                         u32 field) {
    if (field == 0) top -= 0.5f;   // matches the decomp (de-jitter for field 0)
    auto& g = state();
    g.vpLeft = left; g.vpTop = top; g.vpWd = wd; g.vpHt = ht;
    g.vpNearz = nearz; g.vpFarz = farz;
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    GXSetViewportJitter(left, top, wd, ht, nearz, farz, 1u);
}

void GXGetViewportv(f32* vp) {
    auto& g = state();
    vp[0] = g.vpLeft; vp[1] = g.vpTop; vp[2] = g.vpWd; vp[3] = g.vpHt;
    vp[4] = g.vpNearz; vp[5] = g.vpFarz;
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    auto& g = state();
    g.scLeft = left; g.scTop = top; g.scWd = wd; g.scHt = ht;
}

// --- SLICE 2: core pixel-pipeline state -----------------------------------
// Capture clean semantic values into GXState (no GC BP register bit-packing) — the
// native renderer maps these to its pipeline directly.
// BP register addresses (from VideoCommon/BPMemory.h) — the SAME opcodes Dolphin's
// CommandProcessor decodes on a real emulator run. Kept here so gx_impl.cpp doesn't
// need to include Dolphin headers.
namespace bp {
constexpr uint8_t GENMODE      = 0x00;
constexpr uint8_t SCISSORTL    = 0x20;
constexpr uint8_t SCISSORBR    = 0x21;
constexpr uint8_t SCISSOROFFSET= 0x59;
constexpr uint8_t ZMODE        = 0x40;
constexpr uint8_t BLENDMODE    = 0x41;
constexpr uint8_t CONSTANTALPHA= 0x42;
constexpr uint8_t ZCOMPARE     = 0x43;   // PE_CONTROL — has color/alpha update bits
constexpr uint8_t ALPHACOMPARE = 0xF3;
constexpr uint8_t TEV_COLOR_ENV_BASE = 0xC0;   // +2*stage
constexpr uint8_t TEV_ALPHA_ENV_BASE = 0xC1;   // +2*stage
constexpr uint8_t TEV_REGISTER_L_BASE = 0xE0;  // TEV colour reg lo (rr, aa) — +2*reg
constexpr uint8_t TEV_REGISTER_H_BASE = 0xE1;  // TEV colour reg hi (bb, gg) — +2*reg
constexpr uint8_t TEV_KSEL_BASE = 0xF6;
}

// Pack {r,g,b,a} into 24 low bits + one BP register write. BPMEM_CLEAR_AR (0x4F)
// carries A<<8 | R, BPMEM_CLEAR_GB (0x50) carries G<<8 | B. That's how the GC's
// EFB clear registers work — used by GXSetCopyClear below.
static inline void bp_write_clear_ar(u8 a, u8 r) {
    sb::gxfifo::bp_write(0x4F, ((u32)a << 8) | (u32)r);
}
static inline void bp_write_clear_gb(u8 g, u8 b) {
    sb::gxfifo::bp_write(0x50, ((u32)g << 8) | (u32)b);
}

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
    auto& g = state();
    g.blendType = type; g.blendSrc = src; g.blendDst = dst; g.blendLogicOp = op;
    // BPMEM_BLENDMODE bit layout (VideoCommon/BPMemory.h union BlendMode):
    //   bit0=enable, bit1=logicop_enable, bit2=dither, bit3=colorUpd, bit4=alphaUpd,
    //   bit5-7=dst_factor, bit8-10=src_factor, bit11=subtract, bit12-15=logic_mode.
    // GXBlendMode maps: GX_BM_NONE=0 → blend=0, GX_BM_BLEND=1 → blend=1,
    //   GX_BM_LOGIC=2 → logic_enable=1, GX_BM_SUBTRACT=3 → subtract=1.
    // GXBlendFactor / GXLogicOp values match Dolphin's Src/DstBlendFactor / LogicOp 1:1.
    u32 v = 0;
    v |= (type == 1) ? 0x1 : 0;              // blend_enable
    v |= (type == 2) ? 0x2 : 0;              // logic_op_enable
    // color/alpha update come from state().colorUpdate / alphaUpdate — bind them
    // into every BLENDMODE write so Dolphin sees them together.
    v |= (g.colorUpdate ? 0x8 : 0);
    v |= (g.alphaUpdate ? 0x10 : 0);
    v |= ((u32)(dst & 7) << 5);
    v |= ((u32)(src & 7) << 8);
    v |= (type == 3) ? (1u << 11) : 0;       // subtract
    v |= ((u32)(op & 0xF) << 12);
    sb::gxfifo::bp_write(bp::BLENDMODE, v);
}
void GXSetZMode(GXBool compare, GXCompare func, GXBool update) {
    auto& g = state();
    g.zCompare = compare; g.zFunc = func; g.zUpdate = update;
    // BPMEM_ZMODE layout: bit0=test_enable, bit1-3=func, bit4=update_enable.
    u32 v = 0;
    v |= compare ? 0x1 : 0;
    v |= ((u32)(func & 7) << 1);
    v |= update  ? 0x10 : 0;
    sb::gxfifo::bp_write(bp::ZMODE, v);
}
void GXSetZCompLoc(GXBool beforeTex) {
    state().zCompLocBeforeTex = beforeTex;
    // BPMEM_ZCOMPARE is PE_CONTROL — bit6 zcomparelocation. Preserve other bits
    // by shadowing them; on first call the shadow starts zero, subsequent writes
    // OR in bits progressively as the game touches them.
    static u32 s_pe_control = 0;
    if (beforeTex) s_pe_control |= (1u << 6);
    else           s_pe_control &= ~(1u << 6);
    sb::gxfifo::bp_write(bp::ZCOMPARE, s_pe_control);
}
void GXSetCullMode(GXCullMode mode) {
    state().cullMode = mode;
    // GXSetCullMode maps to BPMEM_GENMODE bits 14-15 (cullmode). Since GENMODE
    // holds many fields we write from GENMODE-shadowing helpers elsewhere; for
    // isolated cull changes, punt to the game's next GENMODE write to carry it.
    // (The game re-emits GENMODE frequently via GXSetNumChans etc.)
}
// GXSetColorUpdate call history (b76 overbright drill, 2026-06-30): a monotonically increasing
// call counter + the call index of the most recent GX_FALSE write, so the J3D capture can print,
// at b76's draw, how recently colorUpdate was set FALSE (and then restored TRUE). If g_colupd_last_false
// is far below g_colupd_calls at the draw, a TRUE restore ran in between → native runs the mask draw
// OUTSIDE the GC colorUpdate=FALSE window (the structural pass-routing divergence).
long g_colupd_calls = 0;
long g_colupd_last_false = -1;   // call index of the last GXSetColorUpdate(GX_FALSE)
// Ring of the last 16 GXSetColorUpdate values (b76 drill: see what restores cU=TRUE before the mask draw).
unsigned char g_colupd_ring[16] = {0};
int           g_colupd_ring_pos = 0;
// WEAK: only defined inside the sms-boot executable (native/render/sms_boot_j3d_capture.cpp) —
// a test target that links this file without the render-capture pipeline (e.g. sms-j3dload_test)
// must still link cleanly. Safe: every call site below is gated behind a getenv() debug flag.
extern "C" int sb_boot_capture_phase() __attribute__((weak));
extern "C" int sb_present_frame();
void GXSetColorUpdate(GXBool enable) {
    ++g_colupd_calls;
    if (!enable) g_colupd_last_false = g_colupd_calls;
    g_colupd_ring[g_colupd_ring_pos & 15] = enable ? 1 : 0;
    g_colupd_ring_pos++;
    if (const char* d = std::getenv("SB_DBG_COLUPD"); d && d[0] && d[0] != '0') {
        static long n0 = 0, n1 = 0; if (enable) ++n1; else ++n0;
        if ((n0 + n1) <= 8 || ((n0 + n1) % 256) == 0)
            std::fprintf(stderr, "[GXSetColorUpdate] enable=%d (false-count=%ld true-count=%ld)\n",
                         (int)enable, n0, n1);
    }
    // SB_COLUPD_BT: in GX Post (phase 6), log every GXSetColorUpdate value + its guest caller, so the
    // exact sequence native runs before the MapXlu mask flush is visible (vs Dolphin's, which is FALSE).
    if (const char* d = std::getenv("SB_COLUPD_BT"); d && d[0] && d[0] != '0'
        && sb_boot_capture_phase() == 6) {
        static int n = 0; if (n < 400) { ++n;
            void* fr[6]; int nf = backtrace(fr, 6);
            char** sym = backtrace_symbols(fr, nf);
            // shorten: keep just the mangled caller name token
            const char* c2 = nf > 2 && sym ? sym[2] : "?";
            std::fprintf(stderr, "[colupd6 #%d] enable=%d via %s\n", n, (int)enable, c2);
            free(sym);
        }
    }
    // SB_COLUPD_ALL: log EVERY GXSetColorUpdate across ALL phases with the global call index + phase
    // + caller, so the exact ordering of the cU=FALSE effect vs the intervening frameInit(cU=TRUE)
    // restore vs the sea-mask (b76) draw is visible in ONE interleaved stream (the b76 capture prints
    // its own [b76] line at the same call index). Gated + capped; only fires once the scene settles.
    if (const char* d = std::getenv("SB_COLUPD_ALL"); d && d[0] && d[0] != '0'
        && sb_boot_capture_phase() != 0) {
        static int n = 0; if (n < 400 && g_colupd_calls > 4000) { ++n;
            void* fr[6]; int nf = backtrace(fr, 6);
            char** sym = backtrace_symbols(fr, nf);
            const char* c2 = nf > 2 && sym ? sym[2] : "?";
            std::fprintf(stderr, "[colupd @%ld ph%d] enable=%d via %s\n",
                         g_colupd_calls, sb_boot_capture_phase(), (int)enable, c2);
            free(sym);
        }
    }
    state().colorUpdate = enable;
}
extern "C" void sb_gx_colupd_history(long* calls, long* last_false) {
    if (calls)      *calls      = g_colupd_calls;
    if (last_false) *last_false = g_colupd_last_false;
}
// Fill `out` (size 16) with the last 16 GXSetColorUpdate values, OLDEST-first.
extern "C" void sb_gx_colupd_ring(int out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = g_colupd_ring[(g_colupd_ring_pos + i) & 15];
}

// Live GXSetColorUpdate / GXSetAlphaUpdate state, for the J3D capture (the J3DMaterial PE block has
// NO color/alpha-update field — those are global GX state the effect's draw code sets, e.g.
// GXSetColorUpdate(GX_FALSE) for a no-colour pass). Without this, native captured every J3D batch as
// colour-writing and painted the file-select composite (b76, SRCALPHA/SRCCLR) WHITE where the GC
// writes nothing → the overbright. Mirrors the imm path's g_colorUpdate read (gx_imm_impl.cpp).
extern "C" void sb_gx_get_color_alpha_update(int* color_update, int* alpha_update) {
    if (color_update) *color_update = state().colorUpdate ? 1 : 0;
    if (alpha_update) *alpha_update = state().alphaUpdate ? 1 : 0;
}
void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    auto& g = state();
    g.alphaComp0 = comp0; g.alphaRef0 = ref0; g.alphaOp = op;
    g.alphaComp1 = comp1; g.alphaRef1 = ref1;
}
// GXColor passed BY VALUE per the SDK header — the native compiler handles the ABI
// (the "GXColor by pointer" caveat was a RECOMP/MWcc artifact, irrelevant here).
void GXSetCopyClear(GXColor clear, u32 z) {
    auto& g = state();
    g.copyClearColor = clear; g.copyClearZ = z;
    static const bool trace = std::getenv("SB_CLEAR_TRACE") != nullptr;
    if (trace) {
        static int n = 0; if (n < 20) { ++n;
            std::fprintf(stderr, "[GXSetCopyClear] rgba=(%u,%u,%u,%u) z=0x%x\n",
                         clear.r, clear.g, clear.b, clear.a, z);
        }
    }
}

// Bridge for the renderer-attach present layer (native/render/sms_boot_present.cpp),
// which lives in the Vulkan-only sms-render lib and must not pull in the dolphin/gx
// headers. Hands the captured copy-clear colour back as floats (0..1).
extern "C" void sb_gx_get_clear_color(float* rgba) {
    const GXColor c = state().copyClearColor;
    rgba[0] = c.r / 255.0f; rgba[1] = c.g / 255.0f;
    rgba[2] = c.b / 255.0f; rgba[3] = c.a / 255.0f;
}

// SLICE 3 bridge: the captured projection + viewport for the J3D scene-capture path
// (native/render/sms_boot_j3d_capture.cpp), laid out exactly as imm_project()/GXProject
// want — type, proj[0..5] = GXState.projMtx, vp[0..5] = {l,t,w,h,nearz,farz}. Mirrors
// GXGetProjectionv/GXGetViewportv but as a clean extern "C" the Vulkan-only render lib
// can call without pulling in dolphin/gx headers.
// Latch the perspective 4x4 from C_MTXPerspective (mtx_impl.cpp). The camera computes its
// perspective every frame but only calls GXSetProjection when its perform flag has bit 0x10
// (the GC draw phase, which doesn't run in sms-boot) — so the projection reaches GX only via
// this seam. Row-major 16 floats. Stores into the same slot GXSetProjection's latch uses.
extern "C" void sb_gx_latch_proj44(const float m[16]) {
    auto& g = state();
    for (int i = 0; i < 16; ++i) g.proj3dM44[i] = m[i];
    // ALSO populate the 6-element GX form (proj3dMtx) that sb_gx_get_projection returns — the
    // capture/imm_project reads THAT, not the 4x4. Without this it kept a STALE GXSetProjection
    // (e.g. fovy 50 + a degenerate z-row) that diverged from the live camera projection. The GX
    // perspective 6-tuple = {m00, m02, m11, m12, m22, m23} (row-major 4x4 indices 0,2,5,6,10,11).
    g.proj3dMtx[0] = m[0];  g.proj3dMtx[1] = m[2];  g.proj3dMtx[2] = m[5];
    g.proj3dMtx[3] = m[6];  g.proj3dMtx[4] = m[10]; g.proj3dMtx[5] = m[11];
    g.proj3dType = GX_PERSPECTIVE;
    g.proj3dValid = true;
}

// The latched PERSPECTIVE 4x4 (row-major) for the native scene render's clip-space project
// (ngx_project_eye). Returns 1 if a perspective projection has been seen, else 0 (identity).
extern "C" int sb_gx_get_proj44(float m[16]) {
    auto& g = state();
    if (g.proj3dValid) { for (int i = 0; i < 16; ++i) m[i] = g.proj3dM44[i]; return 1; }
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    return 0;
}

extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]) {
    auto& g = state();
    // Prefer the latched PERSPECTIVE projection (the 3D scene matrix); the live projMtx
    // is the HUD ortho by the time the scene render reads it. Fall back to live if no
    // perspective has been set yet.
    const bool p3 = g.proj3dValid;
    if (type) *type = (int)(p3 ? g.proj3dType : g.projType);
    if (proj) for (int i = 0; i < 6; ++i) proj[i] = p3 ? g.proj3dMtx[i] : g.projMtx[i];
    if (vp) {
        if (p3) { for (int i = 0; i < 6; ++i) vp[i] = g.proj3dVp[i]; }
        else {
            vp[0] = g.vpLeft; vp[1] = g.vpTop;  vp[2] = g.vpWd;
            vp[3] = g.vpHt;   vp[4] = g.vpNearz; vp[5] = g.vpFarz;
        }
    }
}

// The LIVE projection + viewport (NOT the latched perspective). GXPost / J2D 2D draws call a real
// GXSetProjection(GX_ORTHOGRAPHIC) + GXSetViewport right before drawing, so at synchronous capture
// time (SB_OWN_GXLIST) the live state IS that ortho — the correct matrix for a 2D screen-space draw.
// sb_gx_get_projection deliberately hides this (it prefers the perspective for the 3D scene, whose
// projection reaches GX only via the camera latch, never a live GXSetProjection). The capture uses
// this to project ortho draws with their own ortho instead of the leftover 3D perspective.
extern "C" void sb_gx_get_live_projection(int* type, float proj[6], float vp[6]) {
    auto& g = state();
    if (type) *type = (int)g.projType;
    if (proj) for (int i = 0; i < 6; ++i) proj[i] = g.projMtx[i];
    if (vp) {
        vp[0] = g.vpLeft; vp[1] = g.vpTop;  vp[2] = g.vpWd;
        vp[3] = g.vpHt;   vp[4] = g.vpNearz; vp[5] = g.vpFarz;
    }
}
// Export the live hardware lights for the native scene capture's per-vertex lighting. Each
// row: [valid, r,g,b, px,py,pz, dx,dy,dz, a0,a1,a2, k0,k1,k2] (view-space pos/dir, 0..1 colour,
// matching ngx::LightSrc). Returns the count of VALID lights (0 ⇒ no light pipeline yet).
extern "C" int sb_gx_get_lights(float out[8][16]) {
    auto& g = state();
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        const auto& L = g.light[i];
        float* o = out[i];
        o[0] = L.valid ? 1.f : 0.f;
        o[1]=L.color[0]; o[2]=L.color[1]; o[3]=L.color[2];
        o[4]=L.pos[0];   o[5]=L.pos[1];   o[6]=L.pos[2];
        o[7]=L.dir[0];   o[8]=L.dir[1];   o[9]=L.dir[2];
        o[10]=L.cosAtt[0];  o[11]=L.cosAtt[1];  o[12]=L.cosAtt[2];
        o[13]=L.distAtt[0]; o[14]=L.distAtt[1]; o[15]=L.distAtt[2];
        if (L.valid) ++n;
    }
    return n;
}
// Export a colour channel's ambient REGISTER (set by GXSetChanAmbColor) as floats 0..1 for
// the native scene capture's per-vertex lighting. When a J3D material carries no ambient block,
// GX semantics (ambSrc=register) say the lit ambient comes from this register — which the stage
// light loader populates from "Ambient Group". slot: 0=COLOR0, 1=COLOR1.
extern "C" void sb_gx_get_chan_amb(int slot, float rgb[3]) {
    const GXColor c = state().chan[slot & 1].ambColor;
    rgb[0] = c.r / 255.f; rgb[1] = c.g / 255.f; rgb[2] = c.b / 255.f;
}

// A colour channel's MATERIAL colour register (set by GXSetChanMatColor), as floats 0..1 — the
// raster colour for PASSCLR draws like TSky's GXDrawSphere backdrop (slot 0=COLOR0, 1=COLOR1).
extern "C" void sb_gx_get_chan_matcolor(int slot, float rgba[4]) {
    const GXColor c = state().chan[slot & 1].matColor;
    rgba[0] = c.r / 255.f; rgba[1] = c.g / 255.f; rgba[2] = c.b / 255.f; rgba[3] = c.a / 255.f;
}

// The image pointer currently bound to GX texmap `slot` by the last GXLoadTexObj (state().boundTex).
// Used by the J3D capture to catch a texmap whose texture is set at RUNTIME via a GXTexObj (the sea
// MIRROR / pollution graffito EFB-copy textures), NOT via the model's static ResTIMG table — the
// material's table texNo then resolves to a stale/wrong asset, but boundTex holds the real (EFB) image.
extern "C" const void* sb_gx_bound_tex_image(int slot) {
    if ((unsigned)slot >= 8) return nullptr;
    const auto& b = state().boundTex[slot];
    return b.valid ? b.image : nullptr;
}

// The CURRENT GX position matrix (GX_PNMTX selected by GXSetCurrentMtx, loaded by
// GXLoadPosMtxImm) as a 3x4 — the modelview an immediate-mode draw (GXDrawSphere) uses.
extern "C" void sb_gx_get_cur_posmtx(float m[3][4]) {
    auto& g = state();
    const u32 slot = g.currentMtx / 3;
    const auto& pm = g.posMtx[slot < 64 ? slot : 0];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m[r][c] = pm[r][c];
}

void GXSetNumChans(u8 n)     { state().numChans = n; }
void GXSetNumTexGens(u8 n)   { state().numTexGens = n; }
void GXSetNumTevStages(u8 n) { state().numTevStages = n; }

// --- SLICE 3: lighting (chan-ctrl/material/ambient + light objects) ---------
// GXSet* capture into GXState; the native renderer (ngx_light) consumes them per
// vertex. "Rebuild as a PC game": the GXLightObj is OPAQUE on hardware (16 HW
// register words), so we own its packing — overlaying it with a friendly float
// layout that GXLoadLightObjImm copies into GXState. No FIFO/XF register writes.

namespace {
// Map a GXChannelID to the two GXState.chan slots it writes. COLOR/ALPHA channels
// write one slot; the combined COLOR0A0/COLOR1A1 write the colour+alpha pair.
// Returns the count (1 or 2) and fills idx[0..count-1].
int chan_slots(GXChannelID chan, int idx[2]) {
    switch (chan) {
    case GX_COLOR0:   idx[0] = 0; return 1;
    case GX_COLOR1:   idx[0] = 1; return 1;
    case GX_ALPHA0:   idx[0] = 2; return 1;
    case GX_ALPHA1:   idx[0] = 3; return 1;
    case GX_COLOR0A0: idx[0] = 0; idx[1] = 2; return 2;
    case GX_COLOR1A1: idx[0] = 1; idx[1] = 3; return 2;
    default:          idx[0] = 0; return 1;
    }
}

// GXLightID is a single-bit mask (GX_LIGHT0=0x1 .. GX_LIGHT7=0x80) -> 0..7 index.
int light_index(GXLightID id) {
    unsigned m = (unsigned)id;
    for (int i = 0; i < 8; ++i) if (m & (1u << i)) return i;
    return 0;
}

// The native overlay of the opaque 64-byte GXLightObj (16 words = 16 floats).
struct NativeLightObj {
    f32 color[4];     // 16
    f32 pos[3];       // 12
    f32 dir[3];       // 12
    f32 cosAtt[3];    // 12  angle attenuation a0,a1,a2
    f32 distAtt[3];   // 12  distance attenuation k0,k1,k2
};
static_assert(sizeof(NativeLightObj) <= sizeof(GXLightObj), "light obj overlay fits");
NativeLightObj& obj(GXLightObj* o) { return *reinterpret_cast<NativeLightObj*>(o); }
} // namespace

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src,
                   u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    // Pack into the decode_chanctl() layout (b0 matSrc, b1 enable, b2-5 mask lo,
    // b6 ambSrc, b7-8 diffFn, b9-10 attnFn, b11-14 mask hi). attnFn packs as the GC
    // 2-bit field: NONE->0, SPEC->1 (enable, select=spec), SPOT->3 (enable, select=spot).
    u32 attn = (attn_fn == GX_AF_SPEC) ? 1u : (attn_fn == GX_AF_SPOT) ? 3u : 0u;
    u32 cc = ((u32)(mat_src & 1) << 0) | ((u32)(enable & 1) << 1)
           | (((u32)light_mask & 0x0F) << 2) | ((u32)(amb_src & 1) << 6)
           | (((u32)diff_fn & 3) << 7) | (attn << 9)
           | ((((u32)light_mask >> 4) & 0x0F) << 11);
    auto& g = state();
    int idx[2]; int n = chan_slots(chan, idx);
    for (int i = 0; i < n; ++i) g.chan[idx[i]].ctrl = cc;
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color) {
    auto& g = state();
    int idx[2]; int n = chan_slots(chan, idx);
    for (int i = 0; i < n; ++i) g.chan[idx[i]].matColor = mat_color;
}

void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) {
    auto& g = state();
    int idx[2]; int n = chan_slots(chan, idx);
    for (int i = 0; i < n; ++i) g.chan[idx[i]].ambColor = amb_color;
}

void GXInitLightColor(GXLightObj* lt, GXColor color) {
    auto& o = obj(lt);
    o.color[0] = color.r / 255.f; o.color[1] = color.g / 255.f;
    o.color[2] = color.b / 255.f; o.color[3] = color.a / 255.f;
}
void GXInitLightPos(GXLightObj* lt, f32 x, f32 y, f32 z) {
    auto& o = obj(lt); o.pos[0] = x; o.pos[1] = y; o.pos[2] = z;
}
void GXInitLightDir(GXLightObj* lt, f32 nx, f32 ny, f32 nz) {
    // GX stores the NEGATED direction (light-to-vertex); the decomp/HW negate here.
    auto& o = obj(lt); o.dir[0] = -nx; o.dir[1] = -ny; o.dir[2] = -nz;
}
void GXInitSpecularDir(GXLightObj* lt, f32 nx, f32 ny, f32 nz) {
    auto& o = obj(lt); o.dir[0] = nx; o.dir[1] = ny; o.dir[2] = nz;
}
void GXInitLightAttn(GXLightObj* lt, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    auto& o = obj(lt);
    o.cosAtt[0]=a0; o.cosAtt[1]=a1; o.cosAtt[2]=a2;
    o.distAtt[0]=k0; o.distAtt[1]=k1; o.distAtt[2]=k2;
}
void GXInitLightAttnA(GXLightObj* lt, f32 a0, f32 a1, f32 a2) {
    auto& o = obj(lt); o.cosAtt[0]=a0; o.cosAtt[1]=a1; o.cosAtt[2]=a2;
}
void GXInitLightAttnK(GXLightObj* lt, f32 k0, f32 k1, f32 k2) {
    auto& o = obj(lt); o.distAtt[0]=k0; o.distAtt[1]=k1; o.distAtt[2]=k2;
}
// GC SDK GXInitLightDistAttn — computes (k0,k1,k2) from reference distance +
// brightness + dist-fn enum. Only two fn enums are used in SMS: GX_DA_OFF (0),
// GX_DA_GENTLE (1), GX_DA_MEDIUM (3). Matches the SDK's math:
//   kfactor = 0.5 * (1 - ref_br)
//   dist_fn switch selects the (k0, k1, k2) triple.
void GXInitLightDistAttn(GXLightObj* lt, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_fn) {
    if (dist_fn == 0) { GXInitLightAttnK(lt, 1.0f, 0.0f, 0.0f); return; }  // OFF
    f32 kf = 0.5f * (1.0f - ref_br);
    f32 k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    switch (dist_fn) {
    case 1: /* GENTLE */ k1 = kf / (ref_dist);                                  break;
    case 3: /* MEDIUM */ k1 = kf / (ref_dist);                                  break;
    case 4: /* STEEP  */ k2 = kf / (ref_dist * ref_dist);                       break;
    }
    GXInitLightAttnK(lt, k0, k1, k2);
}

unsigned long g_light_load_count = 0;   // s26 diag: does ANY code path load a GX light natively?
extern "C" unsigned long sb_gx_light_load_count() { return g_light_load_count; }
void GXLoadLightObjImm(GXLightObj* lt, GXLightID light) {
    ++g_light_load_count;
    auto& o = obj(lt);
    auto& L = state().light[light_index(light)];
    L.valid = true;
    for (int i = 0; i < 4; ++i) L.color[i] = o.color[i];
    for (int i = 0; i < 3; ++i) { L.pos[i]=o.pos[i]; L.dir[i]=o.dir[i];
                                  L.cosAtt[i]=o.cosAtt[i]; L.distAtt[i]=o.distAtt[i]; }
}

void GXGetLightColor(const GXLightObj* lt, GXColor* color) {
    const auto& o = obj(const_cast<GXLightObj*>(lt));
    color->r = (u8)(o.color[0]*255.f + 0.5f); color->g = (u8)(o.color[1]*255.f + 0.5f);
    color->b = (u8)(o.color[2]*255.f + 0.5f); color->a = (u8)(o.color[3]*255.f + 0.5f);
}

// --- SLICE 4: TEV combiner state (GXSetTev*) --------------------------------
// Ported from the decomp GXTev.c, but writing into GXState.tev (the BP-register bit
// layout = NgxTevState.color_env/alpha_env) instead of the GC FIFO. The renderer's
// tev_shader decodes this directly (see gx_tev_bridge.h ngx_tevstate_from_gx).
namespace {
// GXTev.c SET_REG_FIELD(reg, size, shift, val): replace `size` bits at `shift`.
inline void set_field(u32& reg, int size, int shift, u32 val) {
    const u32 mask = ((1u << size) - 1u) << shift;
    reg = (reg & ~mask) | ((val << shift) & mask);
}
} // namespace

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                     GXTevColorArg c, GXTevColorArg d) {
    u32& r = state().tev.colorEnv[stage];
    set_field(r, 4, 12, a); set_field(r, 4, 8, b); set_field(r, 4, 4, c); set_field(r, 4, 0, d);
}
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                     GXTevAlphaArg c, GXTevAlphaArg d) {
    u32& r = state().tev.alphaEnv[stage];
    set_field(r, 3, 13, a); set_field(r, 3, 10, b); set_field(r, 3, 7, c); set_field(r, 3, 4, d);
}
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale,
                     GXBool clamp, GXTevRegID out_reg) {
    u32& r = state().tev.colorEnv[stage];
    set_field(r, 1, 18, op & 1);
    if (op <= 1) { set_field(r, 2, 20, scale); set_field(r, 2, 16, bias); }
    else         { set_field(r, 2, 20, (op >> 1) & 3); set_field(r, 2, 16, 3); }
    set_field(r, 1, 19, clamp & 0xFF); set_field(r, 2, 22, out_reg);
}
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale,
                     GXBool clamp, GXTevRegID out_reg) {
    u32& r = state().tev.alphaEnv[stage];
    set_field(r, 1, 18, op & 1);
    if (op <= 1) { set_field(r, 2, 20, scale); set_field(r, 2, 16, bias); }
    else         { set_field(r, 2, 20, (op >> 1) & 3); set_field(r, 2, 16, 3); }
    set_field(r, 1, 19, clamp & 0xFF); set_field(r, 2, 22, out_reg);
}
void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    GXTevColorArg carg = (id != GX_TEVSTAGE0) ? GX_CC_CPREV : GX_CC_RASC;
    GXTevAlphaArg aarg = (id != GX_TEVSTAGE0) ? GX_CA_APREV : GX_CA_RASA;
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, carg, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO); break;
    case GX_DECAL:
        GXSetTevColorIn(id, carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg); break;
    case GX_BLEND:
        GXSetTevColorIn(id, carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO); break;
    case GX_REPLACE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA); break;
    case GX_PASSCLR:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, carg);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg); break;
    default: break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    auto& t = state().tev;
    u32 tmap = map & ~0x100;
    t.texmap[stage]   = (tmap >= GX_MAX_TEXMAP) ? 0xff : (u8)tmap;
    t.texcoord[stage] = (coord >= GX_MAX_TEXCOORD) ? 0xff : (u8)coord;
    t.colorChan[stage] = (u8)color;     // raw GXChannelID (shader maps COLOR0/COLOR1)
}
void GXSetTevColor(GXTevRegID id, GXColor color) {
    auto& c = state().tev.tevColor[id];
    c[0] = color.r; c[1] = color.g; c[2] = color.b; c[3] = color.a;
}
void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    auto& c = state().tev.tevColor[id];
    c[0] = color.r; c[1] = color.g; c[2] = color.b; c[3] = color.a;
}
void GXSetTevKColor(GXTevKColorID id, GXColor color) {
    auto& k = state().tev.kColor[id];
    k[0] = color.r; k[1] = color.g; k[2] = color.b; k[3] = color.a;
}
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) { state().tev.kcsel[stage] = (u8)sel; }
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) { state().tev.kasel[stage] = (u8)sel; }
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    // The renderer reads swap_table[alphaEnv&3] (raster) and [(alphaEnv>>2)&3] (texture).
    u32& r = state().tev.alphaEnv[stage];
    set_field(r, 2, 0, ras_sel); set_field(r, 2, 2, tex_sel);
}
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green,
                           GXTevColorChan blue, GXTevColorChan alpha) {
    // NgxTevState swizzle byte: r=(b>>6)&3 g=(b>>4)&3 b=(b>>2)&3 a=b&3.
    state().tev.swapTable[table] = (u8)(((red & 3) << 6) | ((green & 3) << 4) |
                                        ((blue & 3) << 2) | (alpha & 3));
}
void GXSetTevDirect(GXTevStageID /*stage*/) { /* no indirect captured: stage is direct */ }
void GXSetNumIndStages(u8 n) { state().tev.numIndStages = n; }

// =============================================================================
// SLICE 5 — the per-draw setters the J3D draw path invokes (vertex format/arrays,
// XF matrix memory, textures, texcoord-gen, indirect, pipeline flags). These are
// captured into GXState for the renderer to consume when it draws a shape; the
// GameCube FIFO/CP/XF register writes the decomp does are DROPPED (no command
// stream). NOT exercised by the model LOADER (which only parses blocks) — they
// fire on draw, so the draw path verifies them end-to-end. The round-trippable
// captures (vtx attr fmt/array, matrix-memory load, texobj) are unit-tested now.
// =============================================================================

// ---- vertex attribute formats + indexed arrays ----------------------------
void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt,
                     GXCompType type, u8 frac) {
    if (vtxfmt >= 8 || (u32)attr >= 26) return;
    auto& f = state().vtxAttrFmt[vtxfmt][attr];
    f.cnt = (u8)cnt; f.type = (u8)type; f.frac = frac;
}
void GXSetArray(GXAttr attr, const void* base_ptr, u8 stride) {
    if ((u32)attr >= 26) return;
    state().vtxArray[attr] = { base_ptr, stride };
}
void GXInvalidateVtxCache(void) { /* no native vertex cache; nothing to flush */ }
// Immediate-mode vertex descriptor (J2D / fader use GXBegin/GXEnd quads). Capture the
// per-attr type. GXBegin opens an immediate primitive captured by the gx_imm seam
// (gx_imm_impl.cpp): the writers (GXVert.h) stream verts to sb_gx_imm_*, the present
// layer projects + renders them. (SLICE 2 of renderer-attach.)
void GXClearVtxDesc(void) {
    for (auto& d : state().immVtxDesc) d = 0; // GX_NONE
}
void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    if ((u32)attr < 26) state().immVtxDesc[attr] = (u8)type;
}
extern void sb_gx_imm_begin(int prim, int vtxfmt, int nverts);   // gx_imm_impl.cpp
void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    // nverts lets the imm seam auto-terminate the primitive (GC HW behaviour) so draws
    // that skip the no-op GXEnd (JUTResFont glyphs) still flush their batch.
    sb_gx_imm_begin((int)type, (int)vtxfmt, (int)nverts);
}
void GXSetLineWidth(u8 /*width*/, GXTexOffset /*texOffsets*/) { /* HW line raster width */ }

// ---- XF matrix memory ------------------------------------------------------
// id is the matrix-memory row; the game loads at id (= slot*3). Slot = id/3.
static void load_mtx34(f32 dst[64][3][4], f32 src[3][4], u32 id) {
    u32 slot = id / 3;
    if (slot >= 64) return;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) dst[slot][r][c] = src[r][c];
}
void GXLoadPosMtxImm(f32 mtx[3][4], u32 id) { load_mtx34(state().posMtx, mtx, id); }
void GXLoadNrmMtxImm(f32 mtx[3][4], u32 id) { load_mtx34(state().nrmMtx, mtx, id); }
void GXLoadTexMtxImm(f32 mtx[][4], u32 id, GXTexMtxType type) {
    // TEXMTX rows live in the same memory; a 2x4 (GX_MTX2x4) tex matrix fills 2 rows.
    u32 slot = id / 3;
    if (slot >= 64) return;
    int rows = (type == GX_MTX2x4) ? 2 : 3;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < 4; ++c) state().texMtx[slot][r][c] = mtx[r][c];
}
// Indexed loads pull the matrix from the bound array (GXSetArray POS/NRM-mtx). The
// array base is a guest matrix table; resolving it needs the draw path's array
// binding, so capture the request and let the draw consumer dereference. Until then
// they record the (index,id) pairing without a faithful copy — honest no-op-copy.
void GXLoadPosMtxIndx(u16 /*mtx_indx*/, u32 /*id*/) { /* draw-path array resolve */ }
void GXLoadNrmMtxIndx3x3(u16 /*mtx_indx*/, u32 /*id*/) { /* draw-path array resolve */ }
void GXSetCurrentMtx(u32 id) { state().currentMtx = id; }

// ---- textures --------------------------------------------------------------
// A native descriptor overlaid on the caller-owned 32-byte GXTexObj (8 u32). The
// GC GXInitTexObj packs HW register bits; we instead store the friendly fields the
// renderer's tex_decode wants, and GXLoadTexObj copies them into the bound slot.
struct NativeTexObj {
    const void* image;   // 8 bytes on a 64-bit host (GXTexObj is 32 -> fits)
    u16 w, h;
    u8  fmt, wrapS, wrapT, mipmap;
    u32 tlutName;        // GXInitTexObjCI: bound palette name (0 for non-CI)
    u8  minFilt, magFilt;// GXInitTexObjLOD
    u32 magic;           // tag so GXLoadTexObj knows it's our overlay
};
static_assert(sizeof(NativeTexObj) <= sizeof(GXTexObj), "tex obj overlay fits");
static const u32 kTexObjMagic = 0x4E545830; // 'NTX0'
void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t,
                  u8 mipmap) {
    NativeTexObj* n = reinterpret_cast<NativeTexObj*>(obj);
    n->image = image_ptr; n->w = width; n->h = height;
    n->fmt = (u8)format; n->wrapS = (u8)wrap_s; n->wrapT = (u8)wrap_t;
    n->mipmap = mipmap; n->tlutName = 0;
    n->minFilt = (u8)GX_LINEAR; n->magFilt = (u8)GX_LINEAR;
    n->magic = kTexObjMagic;
}
// Color-index texture: same overlay as GXInitTexObj plus the bound TLUT name. The
// decomp (GXTexture.c) clears the non-CI flag and stores tlutName; our native overlay
// records the CI format (GX_TF_C4/C8/C14X2) in `fmt` and the palette name in tlutName.
void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                    GXCITexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t,
                    u8 mipmap, u32 tlut_name) {
    GXInitTexObj(obj, image_ptr, width, height, (GXTexFmt)format, wrap_s, wrap_t, mipmap);
    reinterpret_cast<NativeTexObj*>(obj)->tlutName = tlut_name;
}
// LOD/filtering: the native sampler honours min/mag filter; the GC LOD-bias/aniso/
// edge-LOD knobs have no native mip pipeline yet -> recorded-but-unused (filters used).
void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt,
                     f32 /*min_lod*/, f32 /*max_lod*/, f32 /*lod_bias*/,
                     GXBool /*bias_clamp*/, GXBool /*do_edge_lod*/, GXAnisotropy /*aniso*/) {
    NativeTexObj* n = reinterpret_cast<NativeTexObj*>(obj);
    if (n->magic != kTexObjMagic) return;
    n->minFilt = (u8)min_filt; n->magFilt = (u8)mag_filt;
}
u16  GXGetTexObjWidth(const GXTexObj* to) {
    return reinterpret_cast<const NativeTexObj*>(to)->w;
}
u16  GXGetTexObjHeight(const GXTexObj* to) {
    return reinterpret_cast<const NativeTexObj*>(to)->h;
}
void GXGetTexObjAll(const GXTexObj* obj, void** image_ptr, u16* width, u16* height,
                    GXTexFmt* format, GXTexWrapMode* wrap_s, GXTexWrapMode* wrap_t,
                    u8* mipmap) {
    const NativeTexObj* n = reinterpret_cast<const NativeTexObj*>(obj);
    if (image_ptr) *image_ptr = const_cast<void*>(n->image);
    if (width)  *width  = n->w;
    if (height) *height = n->h;
    if (format) *format = (GXTexFmt)n->fmt;
    if (wrap_s) *wrap_s = (GXTexWrapMode)n->wrapS;
    if (wrap_t) *wrap_t = (GXTexWrapMode)n->wrapT;
    if (mipmap) *mipmap = n->mipmap;
}
void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    if ((u32)id >= 8) return;
    NativeTexObj* n = reinterpret_cast<NativeTexObj*>(obj);
    auto& b = state().boundTex[id];
    if (n->magic != kTexObjMagic) { b.valid = false; return; }
    b = { true, n->image, n->w, n->h, n->fmt, n->wrapS, n->wrapT, n->mipmap,
          n->magFilt, n->tlutName };
}
void GXInitTexCacheRegion(GXTexRegion* /*region*/, u8 /*is_32b_mipmap*/, u32 /*tmem_even*/,
                          GXTexCacheSize /*size_even*/, u32 /*tmem_odd*/,
                          GXTexCacheSize /*size_odd*/) {
    /* no TMEM on the native renderer — textures sample from host images directly */
}

// ---- texcoord generation ---------------------------------------------------
void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func,
                       GXTexGenSrc src_param, u32 mtx, GXBool normalize,
                       u32 pt_texmtx) {
    if ((u32)dst_coord >= 8) return;
    state().texGen[dst_coord] = { (u8)func, (u8)src_param, (u16)mtx, (u8)normalize, (u16)pt_texmtx };
}

// ---- indirect texturing ----------------------------------------------------
void GXSetTevIndirect(GXTevStageID /*tev_stage*/, GXIndTexStageID /*ind_stage*/,
                      GXIndTexFormat /*format*/, GXIndTexBiasSel /*bias_sel*/,
                      GXIndTexMtxID /*matrix_sel*/, GXIndTexWrap /*wrap_s*/,
                      GXIndTexWrap /*wrap_t*/, GXBool /*add_prev*/, GXBool /*utc_lod*/,
                      GXIndTexAlphaSel /*alpha_sel*/) {
    /* per-stage indirect setup; consumed by the draw path's indirect TEV (parked) */
}
void GXSetIndTexMtx(GXIndTexMtxID mtx_id, f32 offset[2][3], s8 scale_exp) {
    u32 i = (u32)mtx_id - 1;             // GX_ITM_0 = 1
    if (i >= 3) return;
    auto& m = state().indMtx[i];
    for (int r = 0; r < 2; ++r) for (int c = 0; c < 3; ++c) m.offset[r][c] = offset[r][c];
    m.scaleExp = scale_exp;
}
void GXSetIndTexCoordScale(GXIndTexStageID ind_stage, GXIndTexScale scale_s,
                           GXIndTexScale scale_t) {
    if ((u32)ind_stage >= 4) return;
    state().indScale[ind_stage] = { (u8)scale_s, (u8)scale_t };
}

// ---- pipeline flags --------------------------------------------------------
void GXSetClipMode(GXClipMode mode) { state().clipMode = mode; }
void GXSetCoPlanar(GXBool enable)   { state().coPlanar = enable; }
void GXSetDither(GXBool dither)     { state().dither = dither; }
void GXSetMisc(GXMiscToken /*token*/, u32 /*val*/) {
    /* HW perf/refresh tokens — no native effect */
}

// ---- draw verb -------------------------------------------------------------
// The shape's primitive display list. The native renderer decodes the DL directly
// (ngx geometry decoders) rather than feeding a FIFO; wiring that to GXState's bound
// vtx-desc/arrays is the draw-path task. Defined so the J3D draw path links.
void GXCallDisplayList(void* /*list*/, u32 /*nbytes*/) {
    /* draw path: ngx decodes the shape DL; not a FIFO replay (parked until wired) */
}

} // extern "C"

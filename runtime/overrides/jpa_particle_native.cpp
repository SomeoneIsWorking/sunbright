// N7 — JPA particle capture for the native renderer (ngx).
//
// ngx renders from the J3D object model; JPA particles (FLUDD spray, ambient & NPC effects) draw
// as IMMEDIATE-MODE GX_QUADS billboards (JPADrawVisitor.cpp) with no J3D object, so the shape
// capture path misses them entirely → every particle is DROPPED under ngx present. Like the
// GXDrawCube/Sphere immediate-mode capture (imm_geom_native.cpp), we synthesize the geometry from
// the object model rather than parsing the GP-FIFO.
//
// Seam: JPADraw::drawParticle() @ 0x8032bd10. It is called once per drawn emitter, from
// JPADraw::draw(); the original sets up the per-emitter clipboard JPADraw::cb (setParticleClipBoard:
// size cb.unk4, pivot cb.unkC, view matrix cb.mViewMtx, prm colour cb.mPrmColor), then issues the
// per-shape GXSetZMode + GXSetBlendMode (so the live GX z/blend state below reflects THIS emitter),
// then draws all parent particles. We run the original (cb + GX state populated; its own GX draws
// land in the dropped EFB, keeping Dolphin consistent), then walk the parent particle list and emit
// each billboard into the ngx batch pipeline. drawParticle returns BEFORE draw() calls drawChild()
// → cb/z/blend are guaranteed to still be the PARENT's (a child draw would overwrite them).
//
// The billboard quad arrives ALREADY in eye/view space (JPADrawExecBillBoard maps the particle
// global position through the camera matrix, then adds screen-aligned half-extent offsets in eye
// X/Y at a constant eye Z) → we only project + clip + emit. zmode/blend come live from the shape
// (faithful depth test + alpha/additive blend). Flat slice: PASSCLR fragment = the per-particle prm
// colour (RegisterPrmColorAnm); no texture / no billboard-type variants yet (step 3/4).

#include "../overrides.h"
#include "../ngx/ngx_jpa_billboard.h"

#ifdef HAVE_DOLPHIN_CORE
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void sb_run_original_around(CPUState& cpu, u32 addr, void (*after)(u32), u32 cookie);
extern u32 mem_r32(u32 ea);
extern u8  mem_r8(u32 ea);
extern "C" void ngx_emit_particle_quad_eye(const float eye[4][3], float r, float g, float b, float a,
                                           int blend_mode, int src_factor, int dst_factor,
                                           int z_test, int z_write, int z_func);

namespace {

static const bool s_ngx_present = getenv("SUNBRIGHT_NGX_PRESENT") != nullptr;
static const bool s_no_jpa   = getenv("SUNBRIGHT_NO_JPA") != nullptr;     // A/B: skip particle emission
static const bool s_jpa_show = getenv("SUNBRIGHT_JPA_SHOW") != nullptr;   // debug: opaque magenta, no blend

static inline float rf(u32 ea) { u32 v = mem_r32(ea); float f; std::memcpy(&f, &v, 4); return f; }

// Live GX depth + blend state, captured at the GXSetZMode/GXSetBlendMode tees so the particle emit
// uses the EXACT state the shape set (faithful, not a hardcoded guess). GX power-on defaults.
thread_local int g_z_test = 1, g_z_func = 3 /*LEQUAL*/, g_z_write = 1;
thread_local int g_bl_mode = 0 /*NONE*/, g_bl_src = 1 /*ONE*/, g_bl_dst = 0 /*ZERO*/;

// JPADraw::cb — the static JPADrawClipBoard (verified from setParticleClipBoard's stfs base
// 0x8040C110; disasm 8032b6d4-8032b710). Fields per JPADraw.hpp JPADrawClipBoard layout.
constexpr u32 CB         = 0x8040C110u;
constexpr u32 CB_UNK4_X  = CB + 0x04, CB_UNK4_Y = CB + 0x08;   // base half-size (× particle scale)
constexpr u32 CB_UNKC_X  = CB + 0x0C, CB_UNKC_Y = CB + 0x10;   // pivot offset (0 for centred)
constexpr u32 CB_VIEWMTX = CB + 0x34;                          // MtxPtr (= the draw's view_mtx)
constexpr u32 CB_PRM     = CB + 0x98;                          // GXColor mPrmColor (emitter prm)

// JPABaseEmitter / JPAParticle offsets (JPAEmitter.hpp / JPAParticle.hpp; verified via disasm of
// getDrawParamPPtr=this+0xA0 @ 0x80328f1c and the billboard exec @ 0x8033025c).
constexpr u32 DRAW_IN_EMITTER  = 0x30;    // mDraw @ emitter+0x30 → emitter = this-0x30
constexpr u32 EM_PARTICLE_LIST = 0xF4;    // JSUList<JPABaseParticle> mParticleList; mHead @ +0xF4
constexpr u32 LINK_NEXT  = 0x0C;          // JSULink.mNext
constexpr u32 P_FLAGS    = 0x10;          // FLAG_INVISIBLE 0x8
constexpr u32 P_GLOBALX  = 0x2C;          // mGlobalPosition TVec3 (x@+0x2C y@+0x30 z@+0x34)
constexpr u32 P_SCALEX   = 0xA0 + 0x10;   // mDrawParams.unk10
constexpr u32 P_SCALEY   = 0xA0 + 0x14;   // mDrawParams.unk14
constexpr u32 P_ALPHA    = 0xA0 + 0x20;   // mDrawParams.mAlpha
constexpr u32 P_PRM      = 0xA0 + 0x2C;   // mDrawParams.mPrmColor GXColor (r@+0 g@+1 b@+2 a@+3)

// JPA_U8_THRE (JPADrawVisitor.cpp): ((a*(b+1))*0x10000)>>24.
static inline u32 thre(u32 a, u32 b) { return ((a * (b + 1)) * 0x10000u) >> 24; }

// ── GX state tees (capture live z/blend the shape set; run the original) ────────
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_jpa_zmode, 0x80361f54u, s_ngx_present) {   // GXSetZMode(enable,func,update)
    g_z_test  = (int)(cpu.gpr[3] & 0xFF) ? 1 : 0;
    g_z_func  = (int)(cpu.gpr[4] & 0xFF);
    g_z_write = (int)(cpu.gpr[5] & 0xFF) ? 1 : 0;
    sb_run_original_around(cpu, 0x80361f54u, nullptr, 0);
}
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_jpa_blend, 0x80361dd0u, s_ngx_present) {   // GXSetBlendMode(type,src,dst,op)
    g_bl_mode = (int)(cpu.gpr[3] & 0xFF);
    g_bl_src  = (int)(cpu.gpr[4] & 0xFF);
    g_bl_dst  = (int)(cpu.gpr[5] & 0xFF);
    sb_run_original_around(cpu, 0x80361dd0u, nullptr, 0);
}

SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_jpa_drawparticle, 0x8032bd10u, s_ngx_present) {
    const u32 jpadraw = cpu.gpr[3];
    sb_run_original_around(cpu, 0x8032bd10u, nullptr, 0);   // populate cb + GX z/blend + draw (dropped EFB)
    if (s_no_jpa) return;

    const u32 emitter = jpadraw - DRAW_IN_EMITTER;

    // Per-emitter clipboard (just computed by setParticleClipBoard inside the original).
    const float u4x = rf(CB_UNK4_X), u4y = rf(CB_UNK4_Y);
    const float ucx = rf(CB_UNKC_X), ucy = rf(CB_UNKC_Y);
    const u32   vm  = mem_r32(CB_VIEWMTX);
    if (vm < 0x80000000u) return;
    float m[3][4];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) m[r][c] = rf(vm + (u32)(r*4+c)*4);
    const u32 cbprm = mem_r32(CB_PRM);
    const u32 epr = (cbprm>>24)&0xff, epg = (cbprm>>16)&0xff, epb = (cbprm>>8)&0xff, epa = cbprm&0xff;

    // Live z/blend the shape set for THIS emitter (faithful; SHOW keeps z but forces opaque magenta).
    int zt = g_z_test, zf = g_z_func, zw = g_z_write, bm = g_bl_mode, bs = g_bl_src, bd = g_bl_dst;
    { const char* e = getenv("SUNBRIGHT_JPA_ZTEST"); if (e) zt = atoi(e);
      const char* f = getenv("SUNBRIGHT_JPA_ZFUNC"); if (f) zf = atoi(f); }

    unsigned drawn = 0, total = 0;
    u32 link = mem_r32(emitter + EM_PARTICLE_LIST);   // mHead
    for (int guard = 0; link >= 0x80000000u && guard < 100000; guard++) {
        const u32 particle = mem_r32(link + 0);       // JSULink.mData
        const u32 next     = mem_r32(link + LINK_NEXT);
        if (particle < 0x80000000u) break;
        total++;
        const u32 flags = mem_r32(particle + P_FLAGS);
        if (!(flags & 0x8u)) {   // skip FLAG_INVISIBLE (billboard exec returns early on it)
            const float gx = rf(particle + P_GLOBALX), gy = rf(particle + P_GLOBALX + 4),
                        gz = rf(particle + P_GLOBALX + 8);
            const float sx = rf(particle + P_SCALEX), sy = rf(particle + P_SCALEY);
            const float al = rf(particle + P_ALPHA);
            const u32   pp = mem_r32(particle + P_PRM);
            const u32 ppr = (pp>>24)&0xff, ppg = (pp>>16)&0xff, ppb = (pp>>8)&0xff, ppa = pp&0xff;

            // Eye-space position: pt = mViewMtx · globalPos
            const float ptx = m[0][0]*gx + m[0][1]*gy + m[0][2]*gz + m[0][3];
            const float pty = m[1][0]*gx + m[1][1]*gy + m[1][2]*gz + m[1][3];
            const float ptz = m[2][0]*gx + m[2][1]*gy + m[2][2]*gz + m[2][3];

            // JPADrawExecBillBoard half-extents (screen-aligned) — shared pure math (render_test:jpa_billboard).
            float eye[4][3];
            ngx_jpa::billboard_corners(sx, sy, u4x, u4y, ucx, ucy, ptx, pty, ptz, eye);

            float cr, cg, cb, ca;
            if (s_jpa_show) { cr = 1.f; cg = 0.f; cb = 1.f; ca = 1.f; }
            else {
                // RegisterPrmColorAnm: prm = thre(particle.prm, emitter.prm); prm.a = mAlpha·thre(a,a).
                cr = thre(ppr, epr) / 255.f; cg = thre(ppg, epg) / 255.f; cb = thre(ppb, epb) / 255.f;
                ca = al * (float)thre(ppa, epa) / 255.f / 255.f;
                if (ca < 0.f) ca = 0.f; else if (ca > 1.f) ca = 1.f;
            }
            const int em_bm = s_jpa_show ? 0 : bm, em_bs = s_jpa_show ? 1 : bs, em_bd = s_jpa_show ? 0 : bd;
            ngx_emit_particle_quad_eye(eye, cr, cg, cb, ca, em_bm, em_bs, em_bd, zt, zw, zf);
            drawn++;
        }
        link = next;
    }

    if (getenv("SUNBRIGHT_DBG_JPA")) {
        static unsigned long n = 0;
        if ((n++ % 120) == 0)
            fprintf(stderr, "[jpa] em %#x parts=%u drawn=%u unk4=(%.1f,%.1f) prm=(%u,%u,%u,%u) z(t=%d f=%d w=%d) bl(%d,%d,%d)\n",
                    emitter, total, drawn, u4x, u4y, epr, epg, epb, epa, zt, zf, zw, bm, bs, bd);
    }
}

}  // namespace
#endif  // HAVE_DOLPHIN_CORE

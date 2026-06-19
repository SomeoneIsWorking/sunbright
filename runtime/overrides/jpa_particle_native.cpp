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

// Runtime JPA-emission toggle (file scope so the extern "C" setter below + the override share it).
static int g_jpa_disable = -1;

extern void sb_run_original_around(CPUState& cpu, u32 addr, void (*after)(u32), u32 cookie);
extern u32 mem_r32(u32 ea);
extern u8  mem_r8(u32 ea);
extern "C" void ngx_emit_particle_quad(const ngx_jpa::NgxParticleQuad* q);

namespace {

static const bool s_ngx_present = getenv("SUNBRIGHT_NGX_PRESENT") != nullptr;
static const bool s_no_jpa_env = getenv("SUNBRIGHT_NO_JPA") != nullptr;   // A/B: skip particle emission
static const bool s_jpa_show = getenv("SUNBRIGHT_JPA_SHOW") != nullptr;   // debug: opaque magenta, no blend
// Runtime JPA-emission toggle (/ngxnojpa?v=1) for a DRIFT-FREE within-one-process A/B: capture a
// frame with particles, toggle off, capture without — no second process to drift the HUD/animation.
// -1 = follow the env (default), 0 = force ON, 1 = force OFF. (file-scope, set by sb_jpa_set_disable)

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
constexpr u32 CB_TEXCOORD0 = CB + 0x14;                        // mTexCoords[4] TVec2 (8 bytes each)
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
constexpr u32 P_ENV      = 0xA0 + 0x30;   // mDrawParams.mEnvColor GXColor

// JPADraw (this) layout (JPADraw.hpp): mDrawCtx @ +0x90 (JPADrawContext), then unkC0 @ +0xC0.
// JPADrawContext fields: mBaseShape @ +0x04, mTexResource @ +0x1C.
constexpr u32 D_BASESHAPE  = 0x90 + 0x04; // JPADraw+0x94 → JPABaseShape* (getMainTextureID base)
constexpr u32 D_TEXRES     = 0x90 + 0x1C; // JPADraw+0xAC → JPATextureResource*
// JPABaseShape (JPABaseShape.hpp): TevArgs (colour-combiner inputs) @ +0x48 (4 × GXTevColorArg s32),
// mTextureIndices(u8*) @ +0x08, mTextureAnmKeyNum @ +0x7E, mTextureIndex @ +0x7F, unk80(texanim) @ +0x80.
constexpr u32 BS_TEVARG0   = 0x48;        // GX_CC a (then +4 b, +8 c, +0xC d)
constexpr u32 BS_TEXIDX_TBL= 0x08;        // u8* texanim index table
constexpr u32 BS_ANMKEYNUM = 0x7E;
constexpr u32 BS_TEXIDX    = 0x7F;        // single main texture id (non-texanim)
constexpr u32 BS_TEXANIM   = 0x80;        // unk80 != 0 ⇒ texanim
constexpr u32 CB_ENV       = CB + 0x9C;   // GXColor mEnvColor (emitter env, set in JPADraw::draw)
// JPATextureResource (JPAResourceManager.hpp): unk2C @ +0x2C = JPATexture** table.
// JPATexture (JPATexture.hpp): unk8 @ +0x08 = JUTTexture. JUTTexture.mTexInfo (ResTIMG*) @ +0x20.
constexpr u32 TR_TABLE     = 0x2C;        // JPATexture** unk2C
constexpr u32 JTEX_JUT     = 0x08;        // JPATexture::unk8 (JUTTexture)
constexpr u32 JUT_TIMG     = 0x20;        // JUTTexture::mTexInfo (ResTIMG*)

// JPA_U8_THRE (JPADrawVisitor.cpp): ((a*(b+1))*0x10000)>>24.
static inline u32 thre(u32 a, u32 b) { return ((a * (b + 1)) * 0x10000u) >> 24; }
static inline int rs32(u32 ea) { return (int)mem_r32(ea); }   // GXTevColorArg is a small enum
static inline bool gvalid(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

// Resolve the emitter's texmap0 binding from the object model and fill the quad's texture fields.
// Chain: JPADraw.mTexResource → JPATextureResource.unk2C[texid] (JPATexture*) → +0x8 JUTTexture
// → +0x20 ResTIMG*. Decode the ResTIMG exactly like ngx_j3d_shape.cpp capture_textures
// (fmt@0,w@2,h@4,wrap@6/7,mipEnable@0x10,filter@0x14/15,tlutfmt@0x09,imgOff@0x1C,palOff@0x0C).
// texid = getMainTextureID(0) (disasm 8032c700): texanim → table[0], else baseShape.mTextureIndex.
static void resolve_texture(u32 jpadraw, ngx_jpa::NgxParticleQuad& q) {
    q.tex_addr = q.tlut_addr = 0; q.tex_w = q.tex_h = 0; q.tex_fmt = q.tlut_fmt = 0;
    q.wrap_s = q.wrap_t = 0 /*CLAMP*/; q.min_filter = q.mag_filter = 1 /*LINEAR*/;
    const u32 baseShape = mem_r32(jpadraw + D_BASESHAPE);
    const u32 texRes    = mem_r32(jpadraw + D_TEXRES);
    if (!gvalid(baseShape) || !gvalid(texRes)) return;
    int texid;
    if (mem_r8(baseShape + BS_TEXANIM)) {                 // texanim: table[0] (per-particle = step 4)
        const u32 tbl = mem_r32(baseShape + BS_TEXIDX_TBL);
        if (!gvalid(tbl) || mem_r8(baseShape + BS_ANMKEYNUM) == 0) return;
        texid = mem_r8(tbl);
    } else {
        texid = mem_r8(baseShape + BS_TEXIDX);
    }
    if (texid < 0 || texid > 255) return;
    const u32 table = mem_r32(texRes + TR_TABLE);                   // unk2C = JPATexture** (deref!)
    if (!gvalid(table)) return;
    const u32 jtex = mem_r32(table + (u32)texid * 4);               // unk2C[texid] (JPATexture*)
    if (!gvalid(jtex)) return;
    const u32 timg = mem_r32(jtex + JTEX_JUT + JUT_TIMG);           // JUTTexture.mTexInfo (ResTIMG*)
    if (!gvalid(timg)) return;
    q.tex_fmt = mem_r8(timg + 0x00);
    q.tex_w   = (u16)((mem_r8(timg + 0x02) << 8) | mem_r8(timg + 0x03));
    q.tex_h   = (u16)((mem_r8(timg + 0x04) << 8) | mem_r8(timg + 0x05));
    q.wrap_s  = mem_r8(timg + 0x06); q.wrap_t = mem_r8(timg + 0x07);
    q.min_filter = mem_r8(timg + 0x14); q.mag_filter = mem_r8(timg + 0x15);
    q.tlut_fmt = mem_r8(timg + 0x09);
    const u32 imgOff = mem_r32(timg + 0x1C);
    q.tex_addr = timg + imgOff;
    if (q.tex_fmt == 0x8 || q.tex_fmt == 0x9 || q.tex_fmt == 0xA) {  // CI → palette
        const u32 palOff = mem_r32(timg + 0x0C);
        q.tlut_addr = timg + palOff;
    }
    if (!gvalid(q.tex_addr)) { q.tex_addr = 0; q.tex_w = q.tex_h = 0; }
}

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
    const bool no_jpa = g_jpa_disable >= 0 ? (g_jpa_disable != 0) : s_no_jpa_env;
    if (no_jpa) return;

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
    const u32 cbenv = mem_r32(CB_ENV);
    const u32 eer = (cbenv>>24)&0xff, eeg = (cbenv>>16)&0xff, eeb = (cbenv>>8)&0xff;

    // Per-corner GX texcoords (cb.mTexCoords[4], 8 bytes each, set per shape by setParticleClipBoard).
    float uv[4][2];
    for (int i = 0; i < 4; i++) { uv[i][0] = rf(CB_TEXCOORD0 + i*8); uv[i][1] = rf(CB_TEXCOORD0 + i*8 + 4); }

    // The emitter's texmap0 binding (object model). All particles of one emitter share it (texanim
    // per-particle variation = step 4); resolved once here.
    ngx_jpa::NgxParticleQuad tq{};
    resolve_texture(jpadraw, tq);

    // The shape's colour combiner (JPADrawSetupTev): a/b/c/d = JPABaseShape.TevArgs (GX_CC_*).
    const u32 baseShape = mem_r32(jpadraw + D_BASESHAPE);
    int ta = 15/*ZERO*/, tb_ = 2/*C0*/, tc = 8/*TEXC*/, td = 15/*ZERO*/;   // default = MODULATE (type 1)
    if (gvalid(baseShape)) {
        ta = rs32(baseShape + BS_TEVARG0 + 0); tb_ = rs32(baseShape + BS_TEVARG0 + 4);
        tc = rs32(baseShape + BS_TEVARG0 + 8); td = rs32(baseShape + BS_TEVARG0 + 0xC);
    }
    tq.color_env = ngx_jpa::jpa_color_env(ta, tb_, tc, td);
    tq.alpha_env = ngx_jpa::jpa_alpha_env();

    // Live z/blend the shape set for THIS emitter (faithful; SHOW keeps z but forces opaque magenta).
    int zt = g_z_test, zf = g_z_func, zw = g_z_write, bm = g_bl_mode, bs = g_bl_src, bd = g_bl_dst;
    { const char* e = getenv("SUNBRIGHT_JPA_ZTEST"); if (e) zt = atoi(e);
      const char* f = getenv("SUNBRIGHT_JPA_ZFUNC"); if (f) zf = atoi(f); }
    if (s_jpa_show) {   // debug: opaque magenta flat — out = C0, no texture, no blend
        tq.color_env = ngx_jpa::jpa_color_env(15, 15, 15, 2 /*GX_CC_C0*/);
        tq.tex_addr = 0; bm = 0; bs = 1; bd = 0;
    } else if (getenv("SUNBRIGHT_JPA_TEXSHOW")) {  // debug: textured but OPAQUE (out = TEXC, no blend)
        tq.color_env = ngx_jpa::jpa_color_env(15, 15, 15, 8 /*GX_CC_TEXC*/);
        bm = 0; bs = 1; bd = 0;
    }
    tq.z_test = (u8)zt; tq.z_func = (u8)zf; tq.z_write = (u8)zw;
    tq.blend_mode = (u8)bm; tq.src_factor = (u8)bs; tq.dst_factor = (u8)bd;

    unsigned drawn = 0, total = 0;
    float dbg_al = -1.f; int dbg_pa = -1;   // last particle's mAlpha + resolved A0 (DBG_JPA)
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
            const u32   pe = mem_r32(particle + P_ENV);
            const u32 per = (pe>>24)&0xff, peg = (pe>>16)&0xff, peb = (pe>>8)&0xff;

            // Eye-space position: pt = mViewMtx · globalPos
            const float ptx = m[0][0]*gx + m[0][1]*gy + m[0][2]*gz + m[0][3];
            const float pty = m[1][0]*gx + m[1][1]*gy + m[1][2]*gz + m[1][3];
            const float ptz = m[2][0]*gx + m[2][1]*gy + m[2][2]*gz + m[2][3];

            // JPADrawExecBillBoard half-extents (screen-aligned) — shared pure math (render_test:jpa_billboard).
            ngx_jpa::billboard_corners(sx, sy, u4x, u4y, ucx, ucy, ptx, pty, ptz, tq.eye);
            for (int i = 0; i < 4; i++) { tq.uv[i][0] = uv[i][0]; tq.uv[i][1] = uv[i][1]; }

            // TEV colour registers: C0 = prm (RegisterPrmColorAnm), C1 = env (RegisterEnvColorAnm).
            // prm.rgb = thre(particle.prm, emitter.prm); prm.a (u8) = mAlpha·thre(particle.a, emitter.a).
            if (s_jpa_show) {
                tq.c0[0]=255; tq.c0[1]=0; tq.c0[2]=255; tq.c0[3]=255;
                tq.c1[0]=tq.c1[1]=tq.c1[2]=tq.c1[3]=0;
            } else {
                int pa = (int)(al * (float)thre(ppa, epa));
                if (pa < 0) pa = 0; else if (pa > 255) pa = 255;
                tq.c0[0]=(int16_t)thre(ppr, epr); tq.c0[1]=(int16_t)thre(ppg, epg);
                tq.c0[2]=(int16_t)thre(ppb, epb); tq.c0[3]=(int16_t)pa;
                tq.c1[0]=(int16_t)thre(per, eer); tq.c1[1]=(int16_t)thre(peg, eeg);
                tq.c1[2]=(int16_t)thre(peb, eeb); tq.c1[3]=255;
                dbg_al = al; dbg_pa = pa;
            }
            ngx_emit_particle_quad(&tq);
            drawn++;
        }
        link = next;
    }

    if (getenv("SUNBRIGHT_DBG_JPA")) {
        static unsigned long n = 0;
        if ((n++ % 120) == 0)
            fprintf(stderr, "[jpa] em %#x parts=%u drawn=%u tex=%#x %ux%u fmt=%u cc(a%d b%d c%d d%d) "
                    "prm=(%u,%u,%u,%u) al=%.3f A0=%d uv0=(%.2f,%.2f) z(t=%d f=%d w=%d) bl(%d,%d,%d)\n",
                    emitter, total, drawn, tq.tex_addr, tq.tex_w, tq.tex_h, tq.tex_fmt,
                    ta, tb_, tc, td, epr, epg, epb, epa, dbg_al, dbg_pa, uv[0][0], uv[0][1], zt, zf, zw, bm, bs, bd);
    }
}

}  // namespace

// Runtime toggle for the drift-free A/B (probe /ngxnojpa?v=).
extern "C" void sb_jpa_set_disable(int v) { g_jpa_disable = v; }

#endif  // HAVE_DOLPHIN_CORE

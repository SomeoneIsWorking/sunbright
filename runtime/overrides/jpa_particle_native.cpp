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
constexpr u32 P_AXIS     = 0xA0 + 0x00;   // mDrawParams.unk0 (TVec3 orientation axis, dir variants)
constexpr u32 P_LOCALPOS = 0x20;          // JPABaseParticle.mLocalPosition (dirType 1/2)
constexpr u32 P_VELOCITY = 0x38;          // JPABaseParticle.mVelocity      (dirType 0)
constexpr u32 BS_DIRTYPE = 0x6A;          // JPABaseShape.mDirType (0=Vel 1=Pos 2=PosInv 3=EmtrDir 4=PrevPtcl)
constexpr u32 BS_TYPE    = 0x69;          // JPABaseShape.mType (draw-exec selector)
constexpr u32 BS_FLAGS   = 0x7C;          // JPABaseShape.mFlags (bit0 = stripe traversal reversed)
constexpr u32 P_UNK34    = 0xA0 + 0x34;   // mDrawParams.unk34 (u16 rotation angle, JMASSin/Cos)
// Stripe is an EMITTER-level visitor → it walks the WHOLE particle list (JSUList @ emitter+0xF4):
constexpr u32 EM_LIST_TAIL  = 0xF8;       // JSUPtrList.mTail
constexpr u32 EM_LIST_COUNT = 0xFC;       // JSUPtrList.mLinkCount (getNumLinks)
constexpr u32 LINK_PREV  = 0x08;          // JSUPtrLink.mPrev (reverse traversal / dirTypePrevPtcl)
constexpr u32 EM_DIR     = 0x210;         // JPABaseEmitter.mEmitterDirection (dirType 3)
// JPADraw (this) emitter base colours (set in JPADraw::draw): mPrmColor @ +0xB8, mEnvColor @ +0xBC;
// the stripe colour is EMITTER-level (RegisterColorEmitterPE), THRE'd vs cb.mPrmColor/mEnvColor.
constexpr u32 D_UNK14    = 0x90 + 0x14;   // JPADrawContext.unk14 → JPADraw* (== this)
constexpr u32 JD_EMPRM   = 0xB8;          // JPADraw.mPrmColor
constexpr u32 JD_EMENV   = 0xBC;          // JPADraw.mEnvColor
// Child particle path (drawChild @ 0x8032bf70): child list + SweepShape (the child shape descriptor).
constexpr u32 D_SWEEP        = 0x90 + 0x0C;   // JPADrawContext.mSweepShape (JPADraw+0x9C)
constexpr u32 D_TEXINDICES   = 0x90 + 0x20;   // JPADrawContext.mTexIndices (u16*, JPADraw+0xB0)
constexpr u32 EM_CHILD_LIST  = 0x100;         // JPABaseEmitter.mChildParticleList mHead
constexpr u32 EM_UNK174      = 0x174;         // JPABaseEmitter.unk174 (TVec3 emitter scale)
constexpr u32 CB_UNK68       = CB + 0x68;     // JPADrawClipBoard.unk68 (Mtx 3x4, child PNMTX0 model mtx)
constexpr u32 SW_SCALEY  = 0x10, SW_SCALEX = 0x14;   // JPASweepShape mScaleY/mScaleX
constexpr u32 SW_PRM     = 0x38, SW_ENV    = 0x3C;   // JPASweepShape mPrmColor/mEnvColor
constexpr u32 SW_TYPE    = 0x44, SW_DIRTYPE = 0x45;  // JPASweepShape mType/mDirType
constexpr u32 SW_ALPHAOUT= 0x4B, SW_TEXIDX = 0x4C, SW_INHERIT = 0x4E;  // unk4B / mTextureIndex / unk4E
constexpr u32 BS_BASESIZEY = 0x10, BS_BASESIZEX = 0x14;  // JPABaseShape mBaseSizeY/mBaseSizeX

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

// Decode a JPATextureResource table entry (unk2C[texid]) into the quad's texture fields.
// Chain: JPATextureResource.unk2C[texid] (JPATexture*) → +0x8 JUTTexture → +0x20 ResTIMG*. Decode the
// ResTIMG exactly like ngx_j3d_shape.cpp capture_textures (fmt@0,w@2,h@4,wrap@6/7,filter@0x14/15,
// tlutfmt@0x09,imgOff@0x1C,palOff@0x0C). texid is the resolved unk2C index (parent: getMainTextureID;
// child: mTexIndices[sweepShape.getTextureIndex()]).
static void decode_jpa_texid(u32 texRes, int texid, ngx_jpa::NgxParticleQuad& q) {
    q.tex_addr = q.tlut_addr = 0; q.tex_w = q.tex_h = 0; q.tex_fmt = q.tlut_fmt = 0;
    q.wrap_s = q.wrap_t = 0 /*CLAMP*/; q.min_filter = q.mag_filter = 1 /*LINEAR*/;
    if (!gvalid(texRes) || texid < 0 || texid > 255) return;
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

// Parent texmap0: texid = getMainTextureID(0) (disasm 8032c700): texanim → table[0], else
// baseShape.mTextureIndex; then decode unk2C[texid].
static void resolve_texture(u32 jpadraw, ngx_jpa::NgxParticleQuad& q) {
    q.tex_addr = 0; q.tex_w = q.tex_h = 0;
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
    decode_jpa_texid(texRes, texid, q);
}

// Child texmap0 (drawChild): id = mTexIndices[sweepShape.getTextureIndex()] → decode unk2C[id].
static void resolve_child_texture(u32 jpadraw, u32 sweep, ngx_jpa::NgxParticleQuad& q) {
    q.tex_addr = 0; q.tex_w = q.tex_h = 0;
    const u32 texRes = mem_r32(jpadraw + D_TEXRES);
    const u32 idxTbl = mem_r32(jpadraw + D_TEXINDICES);   // JPADrawContext.mTexIndices (u16*)
    if (!gvalid(texRes) || !gvalid(idxTbl) || !gvalid(sweep)) return;
    const int sweepIdx = mem_r8(sweep + SW_TEXIDX);
    const int texid = (int)(u16)((mem_r8(idxTbl + sweepIdx*2) << 8) | mem_r8(idxTbl + sweepIdx*2 + 1));
    decode_jpa_texid(texRes, texid, q);
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
    // JPABaseShape.mType @ +0x69 selects the draw exec (setDrawExecVisitorsAfterCB): 0=Point 1=Line
    // 2=BillBoard 3=Directional 4=DirCross 5=Stripe 6=StripeCross 7=Rotation 8=RotCross 9=DirBillBoard
    // 10=YBillBoard. We currently emit all as screen-aligned BillBoard (type 2); other types need
    // their own offset math (step 4). Histogram which types a scene actually uses (reachability).
    const int shapeType = gvalid(baseShape) ? mem_r8(baseShape + BS_TYPE) : -1;
    const int dirType   = gvalid(baseShape) ? mem_r8(baseShape + BS_DIRTYPE) : 0;
    if (getenv("SUNBRIGHT_DBG_JPA") && shapeType >= 0 && shapeType < 16) {
        static unsigned long type_hist[16] = {0};
        type_hist[shapeType]++;
        // Reachability of the remaining step-4 work in THIS scene: child particles (drawChild,
        // gated on mSweepShape != null) and per-particle texanim (BS_TEXANIM flag → unk3A index).
        static unsigned long sweep_em = 0, child_parts = 0, texanim_em = 0;
        static int ck_ptype = -1, ck_stype = -1, ck_sdir = -1;   // config of a window's child emitter
        const u32 sweep = mem_r32(jpadraw + 0x9C);          // JPADrawContext.mSweepShape (JPADraw+0x9C)
        if (gvalid(sweep)) {
            sweep_em++;
            const u32 cc = mem_r32(emitter + 0x108 /*childList count*/);
            child_parts += cc;
            if (cc > 0) {   // record the LIVE child config (parent type + sweepShape type/dir)
                ck_ptype = shapeType; ck_stype = mem_r8(sweep + 0x44); ck_sdir = mem_r8(sweep + 0x45);
            }
        }
        if (gvalid(baseShape) && mem_r8(baseShape + BS_TEXANIM)) texanim_em++;
        static unsigned long tn = 0;
        if ((tn++ % 600) == 0) {
            fprintf(stderr, "[jpa-types]");
            for (int t = 0; t < 16; t++) if (type_hist[t]) fprintf(stderr, " t%d=%lu", t, type_hist[t]);
            fprintf(stderr, "  sweepEm=%lu childParts=%lu texanimEm=%lu  child[ptype=%d stype=%d sdir=%d] (last-window)\n",
                    sweep_em, child_parts, texanim_em, ck_ptype, ck_stype, ck_sdir);
            sweep_em = child_parts = texanim_em = 0; ck_ptype = ck_stype = ck_sdir = -1;  // per-window
        }
    }
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

    // ── Stripe / StripeCross (t5/t6): the RIBBON path (the FLUDD water-jet) ────────────────────────
    // JPADrawExecStripe/StripeCross are EMITTER-level visitors (run once over the whole particle list,
    // not per particle): they connect ALL particles into one continuous textured ribbon. We emit one
    // quad per consecutive-particle segment, reusing ngx_emit_particle_quad (all texture/TEV/blend
    // machinery shared). The colour is EMITTER-level (RegisterColorEmitterPE): C0 = THRE(emitter.prm,
    // cb.prm), C1 = THRE(emitter.env, cb.env) — NOT the per-particle prm/env of the billboard path.
    if (shapeType == 5 || shapeType == 6) {
        const u32 elems = mem_r32(emitter + EM_LIST_COUNT);
        if (elems < 2) return;
        const bool reverse = gvalid(baseShape) && (mem_r32(baseShape + BS_FLAGS) & 0x1u);
        const float step = (reverse ? -1.f : 1.f) / (float)(elems - 1);

        // Emitter-level TEV colours (RegisterColorEmitterPE). emitter base prm/env (JPADraw.mPrmColor/
        // mEnvColor via unk14) THRE'd against the per-frame-animated cb.prm/env (epr.. / eer..).
        const u32 unk14 = mem_r32(jpadraw + D_UNK14);
        u32 emPrm = 0xffffffffu, emEnv = 0xffffffffu;
        if (gvalid(unk14)) { emPrm = mem_r32(unk14 + JD_EMPRM); emEnv = mem_r32(unk14 + JD_EMENV); }
        tq.c0[0]=(int16_t)thre((emPrm>>24)&0xff, epr); tq.c0[1]=(int16_t)thre((emPrm>>16)&0xff, epg);
        tq.c0[2]=(int16_t)thre((emPrm>>8)&0xff, epb);  tq.c0[3]=(int16_t)thre(emPrm&0xff, epa);
        tq.c1[0]=(int16_t)thre((emEnv>>24)&0xff, eer); tq.c1[1]=(int16_t)thre((emEnv>>16)&0xff, eeg);
        tq.c1[2]=(int16_t)thre((emEnv>>8)&0xff, eeb);  tq.c1[3]=255;
        if (s_jpa_show) { tq.c0[0]=255; tq.c0[1]=0; tq.c0[2]=255; tq.c0[3]=255; }

        auto to_eye = [&](const float w[3], float e[3]) {
            e[0]=m[0][0]*w[0]+m[0][1]*w[1]+m[0][2]*w[2]+m[0][3];
            e[1]=m[1][0]*w[0]+m[1][1]*w[1]+m[1][2]*w[2]+m[1][3];
            e[2]=m[2][0]*w[0]+m[2][1]*w[1]+m[2][2]*w[2]+m[2][3];
        };
        // mDirTypeFunc(particle): 0=velocity 1=localPos 2=-localPos 3=emitterDir 4=prev-ptcl→this.
        auto dir_for = [&](u32 particle, float d[3]) {
            switch (dirType) {
                default:
                case 0: d[0]=rf(particle+P_VELOCITY); d[1]=rf(particle+P_VELOCITY+4); d[2]=rf(particle+P_VELOCITY+8); break;
                case 1: d[0]=rf(particle+P_LOCALPOS); d[1]=rf(particle+P_LOCALPOS+4); d[2]=rf(particle+P_LOCALPOS+8); break;
                case 2: d[0]=-rf(particle+P_LOCALPOS); d[1]=-rf(particle+P_LOCALPOS+4); d[2]=-rf(particle+P_LOCALPOS+8); break;
                case 3: d[0]=rf(emitter+EM_DIR); d[1]=rf(emitter+EM_DIR+4); d[2]=rf(emitter+EM_DIR+8); break;
                case 4: { const u32 prev = mem_r32(particle + LINK_PREV);   // embedded link @ particle+0
                          if (gvalid(prev)) { const u32 pp = mem_r32(prev + 0);
                              if (gvalid(pp)) { d[0]=rf(pp+P_GLOBALX)-rf(particle+P_GLOBALX);
                                                d[1]=rf(pp+P_GLOBALX+4)-rf(particle+P_GLOBALX+4);
                                                d[2]=rf(pp+P_GLOBALX+8)-rf(particle+P_GLOBALX+8); break; } }
                          d[0]=d[1]=d[2]=0.f; break; }   // no prev → (0,1,0) basis fallback
            }
        };

        unsigned segs = 0;
        // run one ribbon; fVar2 carries across both StripeCross ribbons (decomp does NOT reset it).
        float fVar2;
        auto run_ribbon = [&](bool cross2) {
            const u32 start = reverse ? mem_r32(emitter + EM_LIST_TAIL) : mem_r32(emitter + EM_PARTICLE_LIST);
            bool have_prev = false; float pL[3], pR[3], pv = 0.f;
            u32 link = start;
            for (int guard = 0; link >= 0x80000000u && guard < 100000; guard++) {
                const u32 particle = mem_r32(link + 0);
                if (particle < 0x80000000u) break;
                const float ang = (float)(u16)(mem_r32(particle + P_UNK34) >> 16) * (6.28318530718f / 65536.f);
                float sn, cs;
                if (!cross2) { sn = sinf(ang);  cs = cosf(ang); }
                else         { cs = cosf(ang);  sn = -sinf(ang); }       // StripeCross 2nd ribbon
                const float width = cross2 ? rf(particle + P_SCALEY) : rf(particle + P_SCALEX);  // unk14 / unk10
                float dir[3];
                if (cross2) { dir[0]=rf(particle+P_VELOCITY); dir[1]=rf(particle+P_VELOCITY+4); dir[2]=rf(particle+P_VELOCITY+8); }
                else dir_for(particle, dir);
                const float axis[3] = {rf(particle+P_AXIS), rf(particle+P_AXIS+4), rf(particle+P_AXIS+8)};
                const float pt0[3]  = {rf(particle+P_GLOBALX), rf(particle+P_GLOBALX+4), rf(particle+P_GLOBALX+8)};
                float Lw[3], Rw[3];
                ngx_jpa::jpa_stripe_corners(width, u4x, ucx, sn, cs, axis, dir, pt0, Lw, Rw);
                float eL[3], eR[3]; to_eye(Lw, eL); to_eye(Rw, eR);
                const float curv = fVar2;
                if (have_prev) {   // segment quad [Li,Ri,Ri+1,Li+1] (= the strip's two tris)
                    for (int k=0;k<3;k++){ tq.eye[0][k]=pL[k]; tq.eye[1][k]=pR[k]; tq.eye[2][k]=eR[k]; tq.eye[3][k]=eL[k]; }
                    tq.uv[0][0]=0; tq.uv[0][1]=pv;  tq.uv[1][0]=1; tq.uv[1][1]=pv;
                    tq.uv[2][0]=1; tq.uv[2][1]=curv; tq.uv[3][0]=0; tq.uv[3][1]=curv;
                    ngx_emit_particle_quad(&tq);
                    segs++;
                }
                for (int k=0;k<3;k++){ pL[k]=eL[k]; pR[k]=eR[k]; } pv = curv; have_prev = true;
                fVar2 += step;
                link = reverse ? mem_r32(link + LINK_PREV) : mem_r32(link + LINK_NEXT);
            }
        };
        fVar2 = reverse ? 1.f : 0.f;
        run_ribbon(false);
        if (shapeType == 6) run_ribbon(true);   // StripeCross: 2nd perpendicular ribbon (fVar2 carries)

        if (getenv("SUNBRIGHT_DBG_JPA")) {
            static unsigned long n = 0;
            if ((n++ % 120) == 0)
                fprintf(stderr, "[jpa-stripe] em %#x type=%d dir=%d elems=%u segs=%u rev=%d tex=%#x %ux%u "
                        "cc(a%d b%d c%d d%d) c0=(%d,%d,%d,%d)\n", emitter, shapeType, dirType, elems, segs,
                        reverse, tq.tex_addr, tq.tex_w, tq.tex_h, ta, tb_, tc, td,
                        tq.c0[0], tq.c0[1], tq.c0[2], tq.c0[3]);
        }
        return;
    }

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

            // Per-type quad geometry (JPADraw setDrawExecVisitorsAfterCB selects by mType). t2 BillBoard
            // (screen-aligned), t9 DirBillBoard (screen quad rotated to follow the dir), t3 Directional
            // (world quad oriented by the dir+axis basis). The direction vector (mDirType): 0=velocity,
            // 1=localPos, 2=-localPos. Other dir types (emitter/prev-particle) → fall back to billboard.
            bool emitted_special = false;
            if ((shapeType == 9 || shapeType == 3) && dirType <= 2) {
                float dir[3];
                if (dirType == 0)      { dir[0]=rf(particle+P_VELOCITY); dir[1]=rf(particle+P_VELOCITY+4); dir[2]=rf(particle+P_VELOCITY+8); }
                else                   { dir[0]=rf(particle+P_LOCALPOS); dir[1]=rf(particle+P_LOCALPOS+4); dir[2]=rf(particle+P_LOCALPOS+8);
                                         if (dirType == 2) { dir[0]=-dir[0]; dir[1]=-dir[1]; dir[2]=-dir[2]; } }
                float dn[3] = {dir[0],dir[1],dir[2]};
                if (ngx_jpa::norm3(dn)) {
                    if (shapeType == 9) {   // DirBillBoard: eye-space, rotate offsets by screen-dir
                        const float up[3] = {m[0][1], m[1][1], m[2][1]};   // camera up = mViewMtx col 1
                        float s[3]; ngx_jpa::cross3(dn, up, s);
                        if (ngx_jpa::norm3(s)) {
                            // rotate s into eye (SR = 3x3 of mViewMtx)
                            const float ex = m[0][0]*s[0]+m[0][1]*s[1]+m[0][2]*s[2];
                            const float ey = m[1][0]*s[0]+m[1][1]*s[1]+m[1][2]*s[2];
                            float off[4][2]; ngx_jpa::jpa_dirbb_offsets(sx, sy, u4x, u4y, ucx, ucy, ex, ey, off);
                            for (int i=0;i<4;i++){ tq.eye[i][0]=off[i][0]+ptx; tq.eye[i][1]=off[i][1]+pty; tq.eye[i][2]=ptz; }
                            emitted_special = true;
                        }
                    } else {                // Directional: world quad → eye via mViewMtx per corner
                        float axis[3] = {rf(particle+P_AXIS), rf(particle+P_AXIS+4), rf(particle+P_AXIS+8)};
                        float R[3][3];
                        if (ngx_jpa::jpa_dir_basis(axis, dn, R)) {
                            const float ptw[3] = {gx, gy, gz};
                            float wc[4][3]; ngx_jpa::jpa_directional_corners(sx, sy, u4x, u4y, ucx, ucy, R, ptw, wc);
                            for (int i=0;i<4;i++){
                                const float wx=wc[i][0], wy=wc[i][1], wz=wc[i][2];
                                tq.eye[i][0]=m[0][0]*wx+m[0][1]*wy+m[0][2]*wz+m[0][3];
                                tq.eye[i][1]=m[1][0]*wx+m[1][1]*wy+m[1][2]*wz+m[1][3];
                                tq.eye[i][2]=m[2][0]*wx+m[2][1]*wy+m[2][2]*wz+m[2][3];
                            }
                            emitted_special = true;
                        }
                    }
                }
                if (!emitted_special && dn[0]==0.f && dn[1]==0.f && dn[2]==0.f) { link = next; continue; }  // game skips zero-dir
            }
            if (!emitted_special)   // BillBoard (t2) + fallback — shared pure math (render_test:jpa_billboard)
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

// ── JPADraw::drawChild (0x8032bf70): child particles (the FLUDD splash/mist) ───────────────────────
// A separate seam from drawParticle: draw() calls drawChild() only when the emitter has a SweepShape
// (the child descriptor). setChildClipBoard re-fills the static cb with the CHILD clipboard (its own
// size cb.unk4 = 25·sweepScale·emitter.scale, pivot cb.unkC=0, fixed unit texcoords, and a PNMTX0
// MODEL matrix cb.unk68 — identity when the parent baseShape is BillBoard/DirBillBoard, else a copy of
// the view matrix), then issues child GXSetZMode/BlendMode and draws all child particles. We run the
// original (cb + GX state populated, draws to the dropped EFB), then walk the child list and emit each
// billboard — same machinery as the parent path, reading the CHILD cb. The child exec is the SAME
// JPADrawExec* objects as the parent (selected by the SweepShape type); the reachable plaza spray uses
// SweepShape type 2 = BillBoard, so we emit screen-aligned billboards (other child types share the
// per-type math the parent path already has, addable later). Faithful to JPADraw.cpp:1003 drawChild.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_jpa_drawchild, 0x8032bf70u, s_ngx_present) {
    const u32 jpadraw = cpu.gpr[3];
    sb_run_original_around(cpu, 0x8032bf70u, nullptr, 0);   // setChildClipBoard + child GX state + draws
    const bool no_jpa = g_jpa_disable >= 0 ? (g_jpa_disable != 0) : s_no_jpa_env;
    if (no_jpa) return;

    const u32 emitter   = jpadraw - DRAW_IN_EMITTER;
    const u32 sweep     = mem_r32(jpadraw + D_SWEEP);
    const u32 baseShape = mem_r32(jpadraw + D_BASESHAPE);
    if (!gvalid(sweep)) return;

    // Child clipboard (just computed by setChildClipBoard): size cb.unk4, pivot cb.unkC(=0), view mtx,
    // and the cb.unk68 PNMTX0 model matrix the child positions pass through on the GPU.
    const float u4x = rf(CB_UNK4_X), u4y = rf(CB_UNK4_Y);
    const float ucx = rf(CB_UNKC_X), ucy = rf(CB_UNKC_Y);
    const u32   vm  = mem_r32(CB_VIEWMTX);
    if (vm < 0x80000000u) return;
    float m[3][4], M68[3][4];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) m[r][c]   = rf(vm + (u32)(r*4+c)*4);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) M68[r][c] = rf(CB_UNK68 + (u32)(r*4+c)*4);
    float uv[4][2];
    for (int i = 0; i < 4; i++) { uv[i][0] = rf(CB_TEXCOORD0 + i*8); uv[i][1] = rf(CB_TEXCOORD0 + i*8 + 4); }
    const u32 cbprm = mem_r32(CB_PRM), cbenv = mem_r32(CB_ENV);
    const u32 epr=(cbprm>>24)&0xff, epg=(cbprm>>16)&0xff, epb=(cbprm>>8)&0xff, epa=cbprm&0xff;
    const u32 eer=(cbenv>>24)&0xff, eeg=(cbenv>>16)&0xff, eeb=(cbenv>>8)&0xff;

    ngx_jpa::NgxParticleQuad tq{};
    resolve_child_texture(jpadraw, sweep, tq);

    // Combiner = parent baseShape TevArgs (drawChild keeps the shape's combiner; setupTev ran in draw()).
    int ta = 15, tb_ = 2, tc = 8, td = 15;
    if (gvalid(baseShape)) {
        ta = rs32(baseShape + BS_TEVARG0 + 0); tb_ = rs32(baseShape + BS_TEVARG0 + 4);
        tc = rs32(baseShape + BS_TEVARG0 + 8); td = rs32(baseShape + BS_TEVARG0 + 0xC);
    }
    tq.color_env = ngx_jpa::jpa_color_env(ta, tb_, tc, td);
    tq.alpha_env = ngx_jpa::jpa_alpha_env();

    int zt = g_z_test, zf = g_z_func, zw = g_z_write, bm = g_bl_mode, bs = g_bl_src, bd = g_bl_dst;
    { const char* e = getenv("SUNBRIGHT_JPA_ZTEST"); if (e) zt = atoi(e);
      const char* f = getenv("SUNBRIGHT_JPA_ZFUNC"); if (f) zf = atoi(f); }
    if (s_jpa_show)                         { tq.color_env = ngx_jpa::jpa_color_env(15,15,15,2); tq.tex_addr=0; bm=0; bs=1; bd=0; }
    else if (getenv("SUNBRIGHT_JPA_TEXSHOW")) { tq.color_env = ngx_jpa::jpa_color_env(15,15,15,8); bm=0; bs=1; bd=0; }
    tq.z_test=(u8)zt; tq.z_func=(u8)zf; tq.z_write=(u8)zw;
    tq.blend_mode=(u8)bm; tq.src_factor=(u8)bs; tq.dst_factor=(u8)bd;

    // Child colour: emitter-level RegisterColorChildPE (sweepShape prm/env) UNLESS the sweep enables
    // alpha-out / inherited alpha / inherited RGB → then per-particle RegisterPrmCEnv (child drawParams).
    const bool perPart = mem_r8(sweep + SW_ALPHAOUT) || (mem_r8(sweep + SW_INHERIT) & 0x6u);
    if (!perPart && !s_jpa_show) {   // emitter-level child colour (compute once)
        const u32 sp = mem_r32(sweep + SW_PRM), se = mem_r32(sweep + SW_ENV);
        tq.c0[0]=(int16_t)thre((sp>>24)&0xff,epr); tq.c0[1]=(int16_t)thre((sp>>16)&0xff,epg);
        tq.c0[2]=(int16_t)thre((sp>>8)&0xff,epb);  tq.c0[3]=(int16_t)thre(sp&0xff,epa);
        tq.c1[0]=(int16_t)thre((se>>24)&0xff,eer); tq.c1[1]=(int16_t)thre((se>>16)&0xff,eeg);
        tq.c1[2]=(int16_t)thre((se>>8)&0xff,eeb);  tq.c1[3]=255;
    } else if (s_jpa_show) { tq.c0[0]=255; tq.c0[1]=0; tq.c0[2]=255; tq.c0[3]=255; tq.c1[0]=tq.c1[1]=tq.c1[2]=tq.c1[3]=0; }

    unsigned drawn = 0, total = 0;
    u32 link = mem_r32(emitter + EM_CHILD_LIST);
    for (int guard = 0; link >= 0x80000000u && guard < 100000; guard++) {
        const u32 particle = mem_r32(link + 0);
        const u32 next     = mem_r32(link + LINK_NEXT);
        if (particle < 0x80000000u) break;
        total++;
        if (!(mem_r32(particle + P_FLAGS) & 0x8u)) {   // skip FLAG_INVISIBLE
            const float gx = rf(particle+P_GLOBALX), gy = rf(particle+P_GLOBALX+4), gz = rf(particle+P_GLOBALX+8);
            const float sx = rf(particle+P_SCALEX), sy = rf(particle+P_SCALEY);
            const float ptx = m[0][0]*gx+m[0][1]*gy+m[0][2]*gz+m[0][3];
            const float pty = m[1][0]*gx+m[1][1]*gy+m[1][2]*gz+m[1][3];
            const float ptz = m[2][0]*gx+m[2][1]*gy+m[2][2]*gz+m[2][3];
            float bc[4][3];
            ngx_jpa::billboard_corners(sx, sy, u4x, u4y, ucx, ucy, ptx, pty, ptz, bc);
            for (int i = 0; i < 4; i++) {   // apply the cb.unk68 PNMTX0 model matrix (identity for t2 parents)
                const float x=bc[i][0], y=bc[i][1], z=bc[i][2];
                tq.eye[i][0]=M68[0][0]*x+M68[0][1]*y+M68[0][2]*z+M68[0][3];
                tq.eye[i][1]=M68[1][0]*x+M68[1][1]*y+M68[1][2]*z+M68[1][3];
                tq.eye[i][2]=M68[2][0]*x+M68[2][1]*y+M68[2][2]*z+M68[2][3];
                tq.uv[i][0]=uv[i][0]; tq.uv[i][1]=uv[i][1];
            }
            if (perPart && !s_jpa_show) {   // per-particle child colour (RegisterPrmCEnv)
                const u32 pp = mem_r32(particle+P_PRM), pe = mem_r32(particle+P_ENV);
                const float al = rf(particle+P_ALPHA);
                int pa = (int)(al * (float)thre(pp&0xff, epa)); if (pa<0) pa=0; else if (pa>255) pa=255;
                tq.c0[0]=(int16_t)thre((pp>>24)&0xff,epr); tq.c0[1]=(int16_t)thre((pp>>16)&0xff,epg);
                tq.c0[2]=(int16_t)thre((pp>>8)&0xff,epb);  tq.c0[3]=(int16_t)pa;
                tq.c1[0]=(int16_t)thre((pe>>24)&0xff,eer); tq.c1[1]=(int16_t)thre((pe>>16)&0xff,eeg);
                tq.c1[2]=(int16_t)thre((pe>>8)&0xff,eeb);  tq.c1[3]=255;
            }
            ngx_emit_particle_quad(&tq);
            drawn++;
        }
        link = next;
    }
    if (getenv("SUNBRIGHT_DBG_JPA") && total > 0) {   // print only when child particles are actually live
        static unsigned long n = 0;
        if ((n++ % 30) == 0)
            fprintf(stderr, "[jpa-child] em %#x stype=%d listCount=%u walked=%u drawn=%u tex=%#x %ux%u perPart=%d\n",
                    emitter, mem_r8(sweep + SW_TYPE), mem_r32(emitter + 0x108), total, drawn,
                    tq.tex_addr, tq.tex_w, tq.tex_h, perPart);
    }
}

}  // namespace

// Runtime toggle for the drift-free A/B (probe /ngxnojpa?v=).
extern "C" void sb_jpa_set_disable(int v) { g_jpa_disable = v; }

#endif  // HAVE_DOLPHIN_CORE

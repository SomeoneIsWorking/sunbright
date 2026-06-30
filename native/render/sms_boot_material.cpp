// sms_boot_material.cpp — see sms_boot_material.h.
#include "sms_boot_material.h"
#include "tex_decode.h"

#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DTevBlocks.hpp>
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DColorBlocks.hpp>
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DPEBlocks.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>

#include <cstring>
#include <cstdint>
#include <cstdio>

extern "C" int sb_boot_capture_texsrc_is_efb_dest(const void*);  // sms_boot_j3d_capture.cpp
#include <cstdlib>
#include <unistd.h>

// Signal-safe host-memory readability probe. A ResTIMG shared between models via
// J3DTexture::setResTIMG carries imageDataOffset/paletteOffset as a 32-bit POINTER DELTA
// (source timg − dest copy), which on a 32-bit GC wraps in u32 space but on our LP64 host is
// a truncated, frequently NEGATIVE value (e.g. Mario's cap copies body texNo=0 → 0xffdd93a4).
// The faithful read is therefore (char*)t + (int32_t)offset (sign-extended). We can't bound
// such a delta by magnitude, so to stay SEGV-safe we directly probe that the computed source
// span is readable before the tiled decoder walks it: write() returns EFAULT on a bad address
// instead of faulting. Returns true only when both ends of [p, p+n) are mapped.
static bool host_readable(const void* p, size_t n) {
    if (!p || n == 0) return false;
    static int fds[2] = {-1, -1};
    if (fds[1] < 0 && pipe(fds) != 0) return true;   // can't probe → don't block the read
    const uint8_t* ends[2] = { (const uint8_t*)p, (const uint8_t*)p + n - 1 };
    for (const uint8_t* q : ends) {
        ssize_t r = write(fds[1], q, 1);
        if (r < 0) return false;                      // EFAULT → unreadable
        char drain; if (read(fds[0], &drain, 1) < 0) { /* keep pipe drained */ }
    }
    return true;
}

// GX TEV konst-selection defaults when the block carries no per-stage konst sel
// (TVB1 / J3DTevBlock1 has no konst storage): GX_TEV_KCSEL_1 / GX_TEV_KASEL_1.
static constexpr uint8_t kKCSEL_1 = 0x0C;
static constexpr uint8_t kKASEL_1 = 0x1C;
static constexpr uint8_t kSwapIdentity = 0x1B;   // r=0,g=1,b=2,a=3 (identity)

bool sb_build_tev_state(J3DMaterial* mat, NgxTevState& st) {
    st = NgxTevState{};
    J3DTevBlock* tb = mat ? mat->getTevBlock() : nullptr;
    if (!tb) return false;

    const bool tvb1 = (tb->getType() == 'TVB1');   // 1-stage block, no konst/reg/swap storage
    int ns = tb->getTevStageNum();
    if (ns < 1) ns = 1;
    if (ns > 16) ns = 16;
    st.num_stages = (uint8_t)ns;

    for (int s = 0; s < ns; ++s) {
        // J3DTevStage's 8 bytes ARE the GX BP combiner registers. color_env =
        // [mTevColorOp:mTevColorAB:mTevColorCD] (bytes 1,2,3); alpha_env =
        // [mTevAlphaOp:mTevAlphaAB:mTevSwapModeInfo] (bytes 5,6,7) — exactly what the
        // recomp captured and what tev_shader.cpp decodes.
        const uint8_t* sb = reinterpret_cast<const uint8_t*>(tb->getTevStage(s));
        st.stage[s].color_env = ((uint32_t)sb[1] << 16) | ((uint32_t)sb[2] << 8) | sb[3];
        st.stage[s].alpha_env = ((uint32_t)sb[5] << 16) | ((uint32_t)sb[6] << 8) | sb[7];

        J3DTevOrder* ord = tb->getTevOrder(s);
        st.stage[s].texcoord   = ord ? ord->mTexCoord  : 0xff;
        st.stage[s].texmap     = ord ? ord->mTexMap    : 0xff;
        st.stage[s].color_chan = ord ? ord->mColorChan : 0xff;

        st.stage[s].kcsel = tvb1 ? kKCSEL_1 : tb->getTevKColorSel(s);
        st.stage[s].kasel = tvb1 ? kKASEL_1 : tb->getTevKAlphaSel(s);
    }

    // S10 TEV colour registers CPREV/C0/C1/C2 (TVB1 has none → leave 0).
    for (int c = 0; c < 4; ++c) {
        if (J3DGXColorS10* tc = tb->getTevColor(c)) {
            st.tev_color[c][0] = tc->color.r; st.tev_color[c][1] = tc->color.g;
            st.tev_color[c][2] = tc->color.b; st.tev_color[c][3] = tc->color.a;
        }
    }
    // KONST0..3 (default 0xFF when absent, matching the recomp).
    bool anyK = false;
    for (int c = 0; c < 4; ++c) {
        if (J3DGXColor* kc = tb->getTevKColor(c)) {
            anyK = true;
            st.kcolor[c][0] = kc->color.r; st.kcolor[c][1] = kc->color.g;
            st.kcolor[c][2] = kc->color.b; st.kcolor[c][3] = kc->color.a;
        }
    }
    if (!anyK) std::memset(st.kcolor, 0xFF, sizeof st.kcolor);

    // Swap tables (4); identity (0x1B) when the block has none.
    for (int t = 0; t < 4; ++t) {
        J3DTevSwapModeTable* sw = tb->getTevSwapModeTable(t);
        st.swap_table[t] = sw ? sw->mIdx : kSwapIdentity;
    }

    // ── PE block: alpha test → shader discard; blend/zmode/cull → pipeline state ──
    st.pe = NgxPEState{};
    st.pe.z_test = 1; st.pe.z_func = 3 /*GX_LEQUAL*/; st.pe.z_write = 1;   // opaque default
    if (J3DPEBlock* pe = mat->getPEBlock()) {
        if (J3DAlphaComp* ac = pe->getAlphaComp()) {
            uint8_t comp0 = (uint8_t)ac->getComp0(), comp1 = (uint8_t)ac->getComp1();
            uint8_t op = (uint8_t)ac->getOp();
            // ALWAYS/ALWAYS with AND ⇒ no meaningful test. comp 7 == GX_ALWAYS.
            bool trivial = (comp0 == 7 && comp1 == 7);
            if (!trivial) {
                st.pe.alpha_test = 1;
                st.pe.comp0 = comp0; st.pe.ref0 = ac->getRef0(); st.pe.aop = op;
                st.pe.comp1 = comp1; st.pe.ref1 = ac->getRef1();
            }
        }
        if (J3DZMode* zm = pe->getZMode()) {
            st.pe.z_test  = zm->getCompareEnable();
            st.pe.z_func  = zm->getFunc();
            st.pe.z_write = zm->getUpdateEnable();
        }
        if (J3DBlend* bl = pe->getBlend()) {
            st.pe.blend_mode = bl->mBlendMode;
            st.pe.src_factor = bl->mSrcFactor;
            st.pe.dst_factor = bl->mDstFactor;
            st.pe.logic_op   = bl->mLogicOp;
        }
        // GX fog (J3DFog → GXSetFog). Capture the per-material fog so the fragment can blend the
        // combiner output toward the fog colour over the eye-space-z ramp. A fog block with type
        // GX_FOG_NONE (0), or a degenerate ramp (startz==endz, which GXSetFog maps to a no-op
        // A=0/B=0.5/C=0 → zero fog factor), means no fog — leave fog_type 0.
        // SB_NO_FOG: A/B control — skip fog capture entirely to isolate fog's visual contribution.
        static const bool no_fog = [](){ const char* v = std::getenv("SB_NO_FOG"); return v && v[0] && v[0] != '0'; }();
        if (J3DFog* fog = no_fog ? nullptr : pe->getFog()) {
            if (fog->mType != 0 /*GX_FOG_NONE*/ && fog->mEndZ != fog->mStartZ) {
                st.pe.fog_type     = fog->mType;
                st.pe.fog_startz   = fog->mStartZ;
                st.pe.fog_endz     = fog->mEndZ;
                st.pe.fog_color[0] = fog->mColor.r;
                st.pe.fog_color[1] = fog->mColor.g;
                st.pe.fog_color[2] = fog->mColor.b;
            }
        }
        // SB_FOG_DBG: one-shot dump of each material's J3D fog block + blend, to verify whether the
        // additive sky/ray materials carry an enabled GX fog (mType != GX_FOG_NONE). GX applies fog
        // AFTER the TEV combiner; our native renderer implements no fog, so a far additive layer the
        // game fogs to near-invisible stays full-bright in ours. This dump is the value-proof BEFORE
        // implementing fog.
        if (const char* fd = std::getenv("SB_FOG_DBG"); fd && fd[0] && fd[0] != '0') {
            static long s_fog = 0;
            if (s_fog < 80) { ++s_fog;
                J3DFog* fog = pe->getFog();
                // Resolve chan0 matColor + chanCtrl (raster CLR0 source) for this material.
                int mc[4] = {-1,-1,-1,-1}; int cc = -1; int nchan = -1;
                if (J3DColorBlock* cb = mat->getColorBlock()) {
                    nchan = cb->getColorChanNum();
                    if (J3DGXColor* m0 = cb->getMatColor(0)) { mc[0]=m0->color.r; mc[1]=m0->color.g; mc[2]=m0->color.b; mc[3]=m0->color.a; }
                    if (J3DColorChan* ch = cb->getColorChan(0)) cc = ch->mChanCtrl;
                }
                // stage0 combiner envs + tev order (texmap/colorchan).
                uint32_t cenv = st.num_stages ? st.stage[0].color_env : 0;
                uint32_t aenv = st.num_stages ? st.stage[0].alpha_env : 0;
                int tmap = st.num_stages ? st.stage[0].texmap : -1;
                int tchan = st.num_stages ? st.stage[0].color_chan : -1;
                std::fprintf(stderr, "[fogdbg] blend=%u/%u/%u fogType=%u start=%.1f end=%.1f near=%.1f far=%.1f col=%u,%u,%u "
                             "nstg=%u chan0Ctrl=0x%x matC0=%d,%d,%d,%d nchan=%d "
                             "s0.tmap=%d s0.chan=%d s0.cenv=0x%06x s0.aenv=0x%06x\n",
                             st.pe.blend_mode, st.pe.src_factor, st.pe.dst_factor,
                             fog ? fog->mType : 0xFF,
                             fog ? fog->mStartZ : 0.f, fog ? fog->mEndZ : 0.f,
                             fog ? fog->mNearZ : 0.f, fog ? fog->mFarZ : 0.f,
                             fog ? fog->mColor.r : 0, fog ? fog->mColor.g : 0, fog ? fog->mColor.b : 0,
                             st.num_stages, cc, mc[0],mc[1],mc[2],mc[3], nchan,
                             tmap, tchan, cenv, aenv);
            }
        }
    }
    if (J3DColorBlock* cb = mat->getColorBlock())
        st.pe.cull = cb->getCullMode();

    return true;
}

// GX texture format codes match SbTexFormat 1:1; ResTIMG.colorFormat matches SbTlutFormat.
void sb_resolve_textures(J3DMaterial* mat, void* j3dTexturePtr, std::vector<SbTexImage>& out) {
    J3DTevBlock* tb = mat ? mat->getTevBlock() : nullptr;
    J3DTexture*  tx = reinterpret_cast<J3DTexture*>(j3dTexturePtr);
    if (!tb || !tx) return;

    int ns = tb->getTevStageNum(); if (ns < 1) ns = 1; if (ns > 16) ns = 16;
    bool done[8] = {false,false,false,false,false,false,false,false};

    for (int s = 0; s < ns; ++s) {
        J3DTevOrder* ord = tb->getTevOrder(s);
        if (!ord) continue;
        uint8_t m = ord->mTexMap;
        if (m == 0xff || m >= 8 || done[m]) continue;
        done[m] = true;

        uint16_t texNo = tb->getTexNo(m);
        const char* dbg0 = std::getenv("SB_J3D_DBG");
        const bool wantdbg = dbg0 && dbg0[0] && dbg0[0] != '0';
        if (texNo >= tx->getNum()) {
            // Material declares a texmap (texcoords are generated) but its texNo is out of range
            // for the bound texture table → no sampler → the surface renders flat/white (the beach
            // sand bug). Not silent: surface the offending indices.
            static long s_oob = 0;
            if (wantdbg && s_oob < 40) { ++s_oob;
                std::fprintf(stderr, "[texres] OOB stage%d texmap%d texNo=%u >= num=%u — no sampler (white)\n",
                             s, m, texNo, tx->getNum()); }
            continue;
        }
        ResTIMG* t = tx->getResTIMG(texNo);
        if (!t) {
            static long s_null = 0;
            if (wantdbg && s_null < 40) { ++s_null;
                std::fprintf(stderr, "[texres] NULL stage%d texmap%d texNo=%u getResTIMG=null — no sampler (white)\n",
                             s, m, texNo); }
            continue;
        }

        const int fmt = t->format;
        const int lw = t->width, lh = t->height;

        // Sanity-gate the ResTIMG before decoding: a garbage/unswapped header (absurd dims or
        // unknown format) or an offset pointing at unmapped memory would send the tiled decoder
        // reading wild memory (a CMPR SEGV). Skip such a texmap → 1×1 white, and LOUDLY dump the
        // offending fields once. Not a silent nil: an unrenderable texmap legitimately falls back
        // to white in a GX renderer.
        //
        // imageDataOffset/paletteOffset are SIGN-EXTENDED: a setResTIMG-shared texture stores them
        // as a 32-bit pointer delta that is frequently negative on LP64 (see host_readable above).
        const int pw = sb_tex_pad_w(lw, fmt), ph = sb_tex_pad_h(lh, fmt);
        const auto*  base = reinterpret_cast<const uint8_t*>(t);
        const uint8_t* src = base + (intptr_t)(int32_t)t->imageDataOffset;
        const size_t srcbytes = (size_t)sb_tex_size_bytes(pw, ph, fmt);

        // EFB-sampler attribution: if this texmap's image data IS an EFB-copy destination recorded
        // this frame, it samples a snapshot of the EFB (the post-pass soft-focus/bloom composite) —
        // on GC the real scene, here decoded as a stale/garbage asset → the file-select overbright.
        if (sb_boot_capture_texsrc_is_efb_dest(src)) {
            const char* d = std::getenv("SB_J3D_DBG");
            if (d && d[0] && d[0] != '0')
                std::fprintf(stderr, "[texres] *** EFB-SAMPLER stage%d texmap%d texNo=%u %dx%d src=%p "
                             "(samples an EFB-copy snapshot) ***\n", s, m, texNo, lw, lh, (const void*)src);
        }

        const uint8_t* tlut = nullptr; int tlutfmt = 0;
        if (sb_tex_is_paletted(fmt)) {
            tlut = base + (intptr_t)(int32_t)t->paletteOffset;
            tlutfmt = t->colorFormat;
        }

        bool fmtok = (fmt==0||fmt==1||fmt==2||fmt==3||fmt==4||fmt==5||fmt==6||fmt==8||fmt==9||fmt==0xA||fmt==0xE);
        bool dimok = (lw > 0 && lh > 0 && lw <= 4096 && lh <= 4096);
        // TLUT: at most 256 entries × 2 bytes (RGB5A3/IA8/RGB565) = 0x200.
        bool offok = host_readable(src, srcbytes) && (!tlut || host_readable(tlut, 0x200));
        static long s_dbg = 0;
        const char* dbg = std::getenv("SB_J3D_DBG");
        if ((dbg && dbg[0] && dbg[0] != '0') && s_dbg < 40) {
            ++s_dbg;
            std::fprintf(stderr, "[texres] stage%d texmap%d texNo=%u fmt=0x%x %dx%d "
                         "imgOff=0x%x palOff=0x%x mip=%d minF=%u magF=%u mipEn=%u aniso=%u %s\n",
                         s, m, texNo, fmt, lw, lh,
                         t->imageDataOffset, t->paletteOffset, t->mipmapCount,
                         t->minFilter, t->magFilter, t->mipmapEnabled, t->maxAnisotropy,
                         (fmtok&&dimok&&offok) ? "ok" : "REJECT");
        }
        if (!fmtok || !dimok || !offok) {
            static long s_rej = 0;
            if (s_rej < 20) { ++s_rej;
                std::fprintf(stderr, "[texres] REJECT texNo=%u fmt=0x%x %dx%d imgOff=0x%x (src=%p readable=%d) — skipping (white)\n",
                             texNo, fmt, lw, lh, t->imageDataOffset, (const void*)src, (int)host_readable(src, srcbytes)); }
            continue;
        }

        // Decode at block-padded dims, then copy the logical lw×lh sub-rect into a
        // tightly-packed buffer (avoids the padding-column UV leak; nvk uploads packed).
        std::vector<uint32_t> padded((size_t)pw * ph);
        sb_tex_decode(padded.data(), src, pw, ph, fmt, tlut, tlutfmt);

        SbTexImage img;
        img.slot = m; img.w = (uint32_t)lw; img.h = (uint32_t)lh;
        img.wrap_s = t->wrapS; img.wrap_t = t->wrapT;
        img.linear = (t->magFilter == 1);   // GX_LINEAR (MAG only)
        // GC tracks the minification filter SEPARATELY (and it encodes the mip mode). The
        // renderer previously reused magFilter for both, so a ground texture authored with
        // GX_LIN_MIP_LIN minification got point-sampled at grazing angles → shoreline moire.
        img.min_filter = t->minFilter;
        img.max_aniso  = t->maxAnisotropy;
        img.rgba.resize((size_t)lw * lh);
        for (int y = 0; y < lh; ++y)
            std::memcpy(&img.rgba[(size_t)y * lw], &padded[(size_t)y * pw], (size_t)lw * 4);
        out.push_back(std::move(img));
    }
}

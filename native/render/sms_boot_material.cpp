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
#include <cstdlib>

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
        if (texNo >= tx->getNum()) continue;
        ResTIMG* t = tx->getResTIMG(texNo);
        if (!t) continue;

        const int fmt = t->format;
        const int lw = t->width, lh = t->height;

        // Sanity-gate the ResTIMG before decoding: a garbage/unswapped header (absurd
        // dims, unknown format, or an image offset outside any sane range) would send the
        // tiled decoder reading wild memory (a CMPR SEGV). Skip that texmap → 1×1 white,
        // and LOUDLY dump the offending fields once so the root cause is visible (a
        // systematic byte-swap shows up as byte-swapped dims here). Not a silent nil:
        // an unrenderable texmap legitimately falls back to white in a GX renderer.
        bool fmtok = (fmt==0||fmt==1||fmt==2||fmt==3||fmt==4||fmt==5||fmt==6||fmt==8||fmt==9||fmt==0xA||fmt==0xE);
        bool dimok = (lw > 0 && lh > 0 && lw <= 4096 && lh <= 4096);
        bool offok = (t->imageDataOffset >= 0x20u && t->imageDataOffset < 0x4000000u);
        static long s_dbg = 0;
        const char* dbg = std::getenv("SB_J3D_DBG");
        if ((dbg && dbg[0] && dbg[0] != '0') && s_dbg < 40) {
            ++s_dbg;
            std::fprintf(stderr, "[texres] stage%d texmap%d texNo=%u fmt=0x%x %dx%d "
                         "imgOff=0x%x palOff=0x%x mip=%d %s\n", s, m, texNo, fmt, lw, lh,
                         t->imageDataOffset, t->paletteOffset, t->mipmapCount,
                         (fmtok&&dimok&&offok) ? "ok" : "REJECT");
        }
        if (!fmtok || !dimok || !offok) {
            static long s_rej = 0;
            if (s_rej < 20) { ++s_rej;
                std::fprintf(stderr, "[texres] REJECT texNo=%u fmt=0x%x %dx%d imgOff=0x%x — skipping (white)\n",
                             texNo, fmt, lw, lh, t->imageDataOffset); }
            continue;
        }
        const int pw = sb_tex_pad_w(lw, fmt), ph = sb_tex_pad_h(lh, fmt);

        const uint8_t* src = reinterpret_cast<const uint8_t*>(t) + t->imageDataOffset;
        const uint8_t* tlut = nullptr; int tlutfmt = 0;
        if (sb_tex_is_paletted(fmt)) {
            tlut = reinterpret_cast<const uint8_t*>(t) + t->paletteOffset;
            tlutfmt = t->colorFormat;
        }

        // Decode at block-padded dims, then copy the logical lw×lh sub-rect into a
        // tightly-packed buffer (avoids the padding-column UV leak; nvk uploads packed).
        std::vector<uint32_t> padded((size_t)pw * ph);
        sb_tex_decode(padded.data(), src, pw, ph, fmt, tlut, tlutfmt);

        SbTexImage img;
        img.slot = m; img.w = (uint32_t)lw; img.h = (uint32_t)lh;
        img.wrap_s = t->wrapS; img.wrap_t = t->wrapT;
        img.linear = (t->magFilter == 1);   // GX_LINEAR
        img.rgba.resize((size_t)lw * lh);
        for (int y = 0; y < lh; ++y)
            std::memcpy(&img.rgba[(size_t)y * lw], &padded[(size_t)y * pw], (size_t)lw * 4);
        out.push_back(std::move(img));
    }
}

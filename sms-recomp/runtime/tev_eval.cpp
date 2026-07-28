// tev_eval — see tev_eval.h. This is the reference; the shader mirrors it.

#include "tev_eval.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>

namespace {

// GXTevColorArg, in hardware order. Returns a vec3; the scalar selectors replicate.
void color_arg(unsigned sel, const float texc[4], const float ras[4], const float konst[4],
               const float reg[4][4], float out[3]) {
    auto rep = [&out](float v) { out[0] = out[1] = out[2] = v; };
    auto rgb = [&out](const float* v) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; };
    switch (sel) {
    case 0:  rgb(reg[0]); return;              // CPREV
    case 1:  rep(reg[0][3]); return;           // APREV
    case 2:  rgb(reg[1]); return;              // C0
    case 3:  rep(reg[1][3]); return;           // A0
    case 4:  rgb(reg[2]); return;              // C1
    case 5:  rep(reg[2][3]); return;           // A1
    case 6:  rgb(reg[3]); return;              // C2
    case 7:  rep(reg[3][3]); return;           // A2
    case 8:  rgb(texc); return;                // TEXC
    case 9:  rep(texc[3]); return;             // TEXA
    case 10: rgb(ras); return;                 // RASC
    case 11: rep(ras[3]); return;              // RASA
    case 12: rep(1.0f); return;                // ONE
    case 13: rep(0.5f); return;                // HALF
    case 14: rgb(konst); return;               // KONST
    default: rep(0.0f); return;                // ZERO
    }
}

float alpha_arg(unsigned sel, const float texc[4], const float ras[4], const float konst[4],
                const float reg[4][4]) {
    switch (sel) {
    case 0:  return reg[0][3];   // APREV
    case 1:  return reg[1][3];   // A0
    case 2:  return reg[2][3];   // A1
    case 3:  return reg[3][3];   // A2
    case 4:  return texc[3];     // TEXA
    case 5:  return ras[3];      // RASA
    case 6:  return konst[3];    // KONST
    default: return 0.0f;        // ZERO
    }
}

float bias_of(unsigned b) { return b == 1 ? 0.5f : (b == 2 ? -0.5f : 0.0f); }
float scale_of(unsigned s) { return s == 1 ? 2.0f : (s == 2 ? 4.0f : (s == 3 ? 0.5f : 1.0f)); }

int u8_of(float v) { return (int)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

bool cmp1(bool isEq, float a, float b) {
    return isEq ? (u8_of(a) == u8_of(b)) : (u8_of(a) > u8_of(b));
}

// GXCompare against an 8-bit alpha, on the QUANTISED value — which is what the hardware compares.
bool alpha_compare(unsigned op, int a, int ref) {
    switch (op) {
    case 0: return false;        // NEVER
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return true;        // ALWAYS
    }
}

} // namespace

void sbr_tev_konst(const SbrTevState& tev, unsigned stage, float out[4]) {
    // GXTevKColorSel / GXTevKAlphaSel, exactly as the hardware defines them:
    //   0x00..0x07  the constant ramp 8/8, 7/8 ... 1/8
    //   0x0C..0x0F  konst register 0..3 as RGB
    //   0x10..0x13  .R of konst 0..3     0x14..0x17  .G     0x18..0x1B  .B     0x1C..0x1F  .A
    auto ramp = [](unsigned s) { return (float)(8 - (int)s) / 8.0f; };
    auto chan = [&](unsigned sel) -> float {
        const unsigned reg = sel & 3;
        const unsigned comp = (sel - 0x10) >> 2;   // 0=R 1=G 2=B 3=A
        return tev.konstReg[reg][comp < 4 ? comp : 0];
    };
    const unsigned kc = tev.stage[stage & 15].kC;
    const unsigned ka = tev.stage[stage & 15].kA;

    if (kc <= 7) {
        out[0] = out[1] = out[2] = ramp(kc);
    } else if (kc >= 0x0C && kc <= 0x0F) {
        for (int i = 0; i < 3; ++i) out[i] = tev.konstReg[kc & 3][i];
    } else if (kc >= 0x10 && kc <= 0x1F) {
        out[0] = out[1] = out[2] = chan(kc);
    } else {
        out[0] = out[1] = out[2] = 1.0f;
    }

    if (ka <= 7) out[3] = ramp(ka);
    else if (ka >= 0x10 && ka <= 0x1F) out[3] = chan(ka);
    else out[3] = 1.0f;
}

bool sbr_tev_compare_packed(unsigned width, bool isEq, const float a[3], const float b[3]) {
    int av, bv;
    if (width == 0) {            // R8
        av = u8_of(a[0]);
        bv = u8_of(b[0]);
    } else if (width == 1) {     // GR16
        av = (u8_of(a[1]) << 8) | u8_of(a[0]);
        bv = (u8_of(b[1]) << 8) | u8_of(b[0]);
    } else {                     // BGR24
        av = (u8_of(a[2]) << 16) | (u8_of(a[1]) << 8) | u8_of(a[0]);
        bv = (u8_of(b[2]) << 16) | (u8_of(b[1]) << 8) | u8_of(b[0]);
    }
    return isEq ? (av == bv) : (av > bv);
}

void sbr_tev_evaluate(const SbrTevState& tev, const SbrTevInputs& in, float out[4], bool* discarded,
                      SbrTevTrace* trace) {
    float reg[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) reg[r][c] = tev.reg[r][c];

    const unsigned n = std::clamp<unsigned>(tev.numStages, 1u, 16u);
    if (trace != nullptr) trace->numStages = n;

    for (unsigned i = 0; i < n; ++i) {
        const SbrTevStage& st = tev.stage[i];

        // A stage with its texture disabled must not read one: GX feeds it nothing, and sampling
        // anyway would tint untextured stages with whatever was last bound.
        float texel[4] = {0, 0, 0, 0};
        if (st.texEnable) {
            const unsigned m = st.texmap & 7;
            for (int c = 0; c < 4; ++c) texel[c] = in.tex[m][c];
        }
        float konst[4];
        sbr_tev_konst(tev, i, konst);

        float ca[3], cb[3], cc[3], cd[3];
        color_arg(st.cA, texel, in.ras, konst, reg, ca);
        color_arg(st.cB, texel, in.ras, konst, reg, cb);
        color_arg(st.cC, texel, in.ras, konst, reg, cc);
        color_arg(st.cD, texel, in.ras, konst, reg, cd);

        float cr[3];
        const bool cCompare = st.cBias == 3;
        if (cCompare) {
            // RGB8 (width 3) compares each channel independently; the narrower widths compare one
            // packed integer and gate all three together.
            if (st.cScale == 3)
                for (int c = 0; c < 3; ++c)
                    cr[c] = cd[c] + (cmp1(st.cSub != 0, ca[c], cb[c]) ? cc[c] : 0.0f);
            else {
                const bool hit = sbr_tev_compare_packed(st.cScale, st.cSub != 0, ca, cb);
                for (int c = 0; c < 3; ++c) cr[c] = cd[c] + (hit ? cc[c] : 0.0f);
            }
        } else {
            const float sign = st.cSub ? -1.0f : 1.0f;
            for (int c = 0; c < 3; ++c) {
                const float mix = ca[c] * (1.0f - cc[c]) + cb[c] * cc[c];
                cr[c] = (cd[c] + sign * mix + bias_of(st.cBias)) * scale_of(st.cScale);
            }
        }
        if (st.cClamp)
            for (int c = 0; c < 3; ++c) cr[c] = std::clamp(cr[c], 0.0f, 1.0f);

        const float aa = alpha_arg(st.aA, texel, in.ras, konst, reg);
        const float ab = alpha_arg(st.aB, texel, in.ras, konst, reg);
        const float ac = alpha_arg(st.aC, texel, in.ras, konst, reg);
        const float ad = alpha_arg(st.aD, texel, in.ras, konst, reg);

        float ar;
        const bool aCompare = st.aBias == 3;
        if (aCompare) {
            // The alpha unit shares the comparator with the colour unit: only A8 (width 3) compares
            // the alpha pair — the narrower widths compare the COLOUR inputs and gate the alpha
            // result with them, which is how a material keys its alpha off an RGB threshold.
            const bool hit = (st.aScale == 3) ? cmp1(st.aSub != 0, aa, ab)
                                              : sbr_tev_compare_packed(st.aScale, st.aSub != 0, ca, cb);
            ar = ad + (hit ? ac : 0.0f);
        } else {
            const float sign = st.aSub ? -1.0f : 1.0f;
            ar = (ad + sign * (aa * (1.0f - ac) + ab * ac) + bias_of(st.aBias)) * scale_of(st.aScale);
        }
        if (st.aClamp) ar = std::clamp(ar, 0.0f, 1.0f);

        if (trace != nullptr) {
            SbrTevStageTrace& t = trace->stage[i];
            t.stage = i;
            t.texmap = st.texmap; t.texcoord = st.texcoord; t.texEnabled = st.texEnable != 0;
            for (int c = 0; c < 4; ++c) { t.texel[c] = texel[c]; t.konst[c] = konst[c]; }
            for (int c = 0; c < 3; ++c) {
                t.cArg[0][c] = ca[c]; t.cArg[1][c] = cb[c];
                t.cArg[2][c] = cc[c]; t.cArg[3][c] = cd[c];
                t.cOut[c] = cr[c];
            }
            t.aArg[0] = aa; t.aArg[1] = ab; t.aArg[2] = ac; t.aArg[3] = ad;
            t.aOut = ar;
            t.cCompare = cCompare; t.aCompare = aCompare;
            t.cDest = st.cDest; t.aDest = st.aDest;
        }

        for (int c = 0; c < 3; ++c) reg[st.cDest & 3][c] = cr[c];
        reg[st.aDest & 3][3] = ar;
    }

    for (int c = 0; c < 4; ++c) out[c] = std::clamp(reg[0][c], 0.0f, 1.0f);

    // ALPHA TEST. Two comparisons combined by a logic op — GX can express a band as well as a plain
    // cutout, so both are evaluated rather than only the first.
    const int a8 = (int)(out[3] * 255.0f + 0.5f);
    const bool c0 = alpha_compare(tev.alphaOp0, a8, tev.alphaRef0);
    const bool c1 = alpha_compare(tev.alphaOp1, a8, tev.alphaRef1);
    bool pass;
    switch (tev.alphaLogic) {
    case 0:  pass = c0 && c1; break;
    case 1:  pass = c0 || c1; break;
    case 2:  pass = c0 != c1; break;
    default: pass = c0 == c1; break;
    }
    if (discarded != nullptr) *discarded = !pass;

    if (trace != nullptr) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) trace->reg[r][c] = reg[r][c];
        for (int c = 0; c < 4; ++c) trace->out[c] = out[c];
        trace->alpha8 = a8;
        trace->alphaPass = pass;
    }
}

void sbr_tev_trace_report(const SbrTevTrace& t, const char* channel) {
    static const char* kCArg[16] = {"CPREV", "APREV", "C0", "A0", "C1", "A1", "C2", "A2",
                                    "TEXC",  "TEXA",  "RASC", "RASA", "ONE", "HALF", "KONST", "ZERO"};
    (void)kCArg;
    for (unsigned i = 0; i < t.numStages; ++i) {
        const SbrTevStageTrace& s = t.stage[i];
        lucent::Line l;
        l.add("  stage {}: map{}{} coord{} texel[{:.3f} {:.3f} {:.3f} a{:.3f}] konst[{:.3f} a{:.3f}]",
              i, s.texmap, s.texEnabled ? "" : "(off)", s.texcoord, s.texel[0], s.texel[1],
              s.texel[2], s.texel[3], s.konst[0], s.konst[3]);
        l.add("  {}-> c[{:.3f} {:.3f} {:.3f}]->reg{}  a {:.3f}->reg{}",
              s.cCompare ? "CMP " : "", s.cOut[0], s.cOut[1], s.cOut[2], s.cDest, s.aOut, s.aDest);
        l.flush(lucent::Level::Info, channel);
    }
    lucent::info(channel, "  final [{:.3f} {:.3f} {:.3f} a{:.3f}] alpha8={} {}", t.out[0], t.out[1],
                 t.out[2], t.out[3], t.alpha8, t.alphaPass ? "PASS" : "DISCARDED");
}

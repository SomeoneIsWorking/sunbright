// tev_eval_test — the TEV pipeline checked against values derived BY HAND from the SDK, not
// against another run of the same code.
//
// Each case states where its expectation comes from: a GX entry point in decomp/sms, or the
// hardware rule that entry point encodes. That is what makes this a verification rather than a
// change-detector — a test whose expected value came from running the implementation only tells you
// the implementation has not changed.

#include "../runtime/render/tev_eval.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void check_near(float got, float want, const char* what, float eps = 1.0f / 512.0f) {
    if (std::fabs(got - want) > eps) {
        std::printf("FAIL: %s — got %.5f want %.5f\n", what, got, want);
        ++g_failures;
    }
}

// A TEV state with everything in its power-on/neutral position, so each test sets only the fields
// it is about. GX_CA_ZERO is 7 and GX_CC_ZERO is 15, hence the asymmetric defaults.
SbrTevState neutral() {
    SbrTevState t{};
    t.numStages = 1;
    for (auto& s : t.stage) {
        s.cA = s.cB = s.cC = s.cD = 15;   // GX_CC_ZERO
        s.aA = s.aB = s.aC = s.aD = 7;    // GX_CA_ZERO
        s.cClamp = s.aClamp = 1;
        s.kC = 0x00;                      // GX_TEV_KCSEL_1 = 0x00 (GXEnum.h) -> the constant 1.0
        s.kA = 0x00;                      // GX_TEV_KASEL_1
    }
    t.alphaOp0 = t.alphaOp1 = 7;          // ALWAYS
    return t;
}

// ---------------------------------------------------------------------------------------------
// The combiner form itself. From GXSetTevColorOp (decomp/sms/src/dolphin/gx/GXTev.c): for ops <= 1
// the register carries (bias, scale) and bit 18 is the SUBTRACT flag, and the hardware computes
//     out = (d + sign*((1-c)*a + c*b) + bias) * scale
void test_lerp_form() {
    SbrTevState t = neutral();
    SbrTevInputs in{};
    in.tex[0][0] = 0.25f; in.tex[0][1] = 0.25f; in.tex[0][2] = 0.25f; in.tex[0][3] = 1.0f;
    in.ras[0] = in.ras[1] = in.ras[2] = 0.75f;

    // a = TEXC (0.25), b = RASC (0.75), c = HALF (0.5), d = ZERO. Expect the midpoint, 0.5.
    t.stage[0].texEnable = 1;
    t.stage[0].cA = 8;    // TEXC
    t.stage[0].cB = 10;   // RASC
    t.stage[0].cC = 13;   // HALF
    t.stage[0].cD = 15;   // ZERO
    float out[4];
    bool discarded = false;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 0.5f, "lerp: (1-0.5)*0.25 + 0.5*0.75");

    // SUBTRACT flips the sign of the mix term: d=ONE, so 1.0 - 0.5 = 0.5.
    t.stage[0].cD = 12;   // ONE
    t.stage[0].cSub = 1;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 0.5f, "lerp: subtract flips the mix term");

    // Bias and scale, from the same encoding: GX_TB_ADDHALF = 1, GX_CS_SCALE_2 = 1.
    t = neutral();
    t.stage[0].cD = 15;          // ZERO
    t.stage[0].cBias = 1;        // +0.5
    t.stage[0].cScale = 1;       // x2
    t.stage[0].cClamp = 0;       // so the x2 is visible rather than saturated
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 1.0f, "bias ADDHALF then scale x2 = 1.0");
}

// ---------------------------------------------------------------------------------------------
// COMPARE MODE. GXSetTevColorOp writes bias = 3 for every op >= GX_TEV_COMP_R8_GT, puts
// (op >> 1) & 3 in the SCALE field (the comparison width) and op & 1 in the subtract bit (== over
// >). The stage then computes out = d + (cmp(a,b) ? c : 0), with no bias and no scale.
//
// Widths, from the GXTevOp enum in decomp/sms/include/dolphin/gx/GXEnum.h:
//   GX_TEV_COMP_R8_GT   = 8  -> (8>>1)&3  = 0
//   GX_TEV_COMP_GR16_GT = 10 -> (10>>1)&3 = 1
//   GX_TEV_COMP_BGR24_GT= 12 -> (12>>1)&3 = 2
//   GX_TEV_COMP_RGB8_GT = 14 -> (14>>1)&3 = 3
void test_compare_mode() {
    SbrTevState t = neutral();
    SbrTevInputs in{};
    t.stage[0].texEnable = 1;
    t.stage[0].cBias = 3;      // compare
    t.stage[0].cA = 8;         // TEXC
    t.stage[0].cB = 14;        // KONST (1.0 by the neutral kC)
    t.stage[0].cC = 12;        // ONE
    t.stage[0].cD = 15;        // ZERO

    float out[4];
    bool discarded = false;

    // R8, GT: texel.r must EXCEED konst.r. konst is 1.0, so nothing exceeds it.
    t.stage[0].cScale = 0;
    in.tex[0][0] = 1.0f;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 0.0f, "compare R8 GT: equal does not count as greater");

    // Drive the comparison through the texel instead, against HALF.
    t.stage[0].cB = 13;         // HALF (0.5)
    in.tex[0][0] = 1.0f;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 1.0f, "compare R8 GT: 1.0 > 0.5 selects c (=ONE)");

    in.tex[0][0] = 0.25f;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 0.0f, "compare R8 GT: 0.25 > 0.5 is false, selects 0");

    // EQ (the subtract bit). 0.5 vs HALF must be equal on the QUANTISED value: 128 == 128.
    t.stage[0].cSub = 1;
    in.tex[0][0] = 0.5f;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 1.0f, "compare R8 EQ: 0.5 == HALF on the 8-bit value");
    t.stage[0].cSub = 0;

    // RGB8 (width 3) compares each channel INDEPENDENTLY, so one channel can pass while another
    // fails — the property that distinguishes it from the packed widths.
    t.stage[0].cScale = 3;
    t.stage[0].cC = 12;                       // ONE
    in.tex[0][0] = 1.0f; in.tex[0][1] = 0.0f; in.tex[0][2] = 1.0f;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 1.0f, "compare RGB8: red passes");
    check_near(out[1], 0.0f, "compare RGB8: green fails independently");
    check_near(out[2], 1.0f, "compare RGB8: blue passes");

    // GR16 packs green into the high byte, so green DOMINATES red. (0, 255) as a packed value is
    // 0x00FF; (1, 0) is 0xFF00 and is the larger — a result impossible under a per-channel compare.
    float a[3] = {0.0f, 1.0f, 0.0f};
    float b[3] = {1.0f, 0.0f, 0.0f};
    check(sbr_tev_compare_packed(1, false, a, b), "compare GR16: green outranks red");
    check(!sbr_tev_compare_packed(0, false, a, b), "compare R8: red alone decides, so the same "
                                                   "pair compares the other way");
}

// ---------------------------------------------------------------------------------------------
// The register file. Stage N writes to one of four registers and later stages read it back; PREV
// (register 0) is what the frame ultimately takes.
void test_register_chaining() {
    SbrTevState t = neutral();
    t.numStages = 2;
    SbrTevInputs in{};

    // Stage 0: write HALF into C0 (register 1). d = HALF, everything else zero.
    t.stage[0].cD = 13;      // HALF
    t.stage[0].cDest = 1;    // C0
    // Stage 1: read C0 as `a`, with c = ZERO so the mix is just `a`, and write it to PREV.
    t.stage[1].cA = 2;       // C0
    t.stage[1].cB = 15;      // ZERO
    t.stage[1].cC = 15;      // ZERO -> mix = a
    t.stage[1].cD = 15;      // ZERO
    t.stage[1].cDest = 0;    // PREV

    float out[4];
    bool discarded = false;
    SbrTevTrace tr{};
    sbr_tev_evaluate(t, in, out, &discarded, &tr);
    check_near(out[0], 0.5f, "register chaining: stage 1 reads what stage 0 wrote to C0");
    check_near(tr.reg[1][0], 0.5f, "trace: C0 holds stage 0's output");
}

// ---------------------------------------------------------------------------------------------
// A stage whose texture is DISABLED must read a zero texel, not whatever was last bound. GX feeds
// it nothing (JRNISetTevOrder clears the enable bit for GX_TEXMAP_NULL), and sampling anyway is how
// an untextured stage picks up a stale texture's colour.
void test_texture_disabled_reads_zero() {
    SbrTevState t = neutral();
    SbrTevInputs in{};
    in.tex[0][0] = in.tex[0][1] = in.tex[0][2] = in.tex[0][3] = 1.0f;

    t.stage[0].texEnable = 0;
    t.stage[0].cA = 8;       // TEXC
    t.stage[0].cB = 15;
    t.stage[0].cC = 15;      // mix = a = TEXC
    t.stage[0].cD = 15;

    float out[4];
    bool discarded = false;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check_near(out[0], 0.0f, "a disabled stage samples nothing, not the bound texture");
}

// ---------------------------------------------------------------------------------------------
// ALPHA TEST. Two comparisons combined by a logic op, on the QUANTISED alpha — a cutout whose
// texels sit exactly on the reference flips entirely on whether the rounding matches.
// GXCompare: 0 NEVER, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 ALWAYS.
void test_alpha_test() {
    SbrTevState t = neutral();
    SbrTevInputs in{};
    in.tex[0][3] = 200.0f / 255.0f;
    t.stage[0].texEnable = 1;
    t.stage[0].aA = 4;       // TEXA
    t.stage[0].aB = 7;       // ZERO
    t.stage[0].aC = 7;       // ZERO -> alpha = TEXA
    t.stage[0].aD = 7;

    float out[4];
    bool discarded = false;

    // GREATER than 128: 200 passes.
    t.alphaOp0 = 4; t.alphaRef0 = 128;
    t.alphaOp1 = 7;                       // ALWAYS
    t.alphaLogic = 0;                     // AND
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check(!discarded, "alpha test: 200 > 128 passes");

    t.alphaRef0 = 220;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check(discarded, "alpha test: 200 > 220 is rejected");

    // A BAND, which is why both comparisons exist: 150 <= a <= 250 with AND.
    t.alphaOp0 = 6; t.alphaRef0 = 150;    // >= 150
    t.alphaOp1 = 3; t.alphaRef1 = 250;    // <= 250
    t.alphaLogic = 0;                     // AND
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check(!discarded, "alpha test: 200 is inside the band 150..250");
    in.tex[0][3] = 10.0f / 255.0f;
    sbr_tev_evaluate(t, in, out, &discarded, nullptr);
    check(discarded, "alpha test: 10 is outside the band");
}

// ---------------------------------------------------------------------------------------------
// The KONST ramp. GX's constant selectors 0..7 are 1.0, 7/8, 6/8 ... 1/8 — an exact ramp, not an
// approximation, and getting it wrong tints every material that uses a constant.
void test_konst_ramp() {
    SbrTevState t = neutral();
    for (unsigned sel = 0; sel < 8; ++sel) {
        t.stage[0].kC = (uint8_t)sel;   // 0x00..0x07 is the ramp; GX_TEV_KCSEL_1 == 0x00
        float k[4];
        sbr_tev_konst(t, 0, k);
        const float want = (float)(8 - (int)sel) / 8.0f;   // 8/8, 7/8 ... 1/8
        char msg[96];
        std::snprintf(msg, sizeof msg, "konst ramp selector %u", sel);
        check_near(k[0], want, msg);
    }
}

// ---------------------------------------------------------------------------------------------
// RAS channel select + swap tables. From GXSetTevOrder/RAS1_TREF (channel field), GXSetTevSwapMode
// (selectors in the alpha combiner word, bits 0..1 ras / 2..3 tex) and GXSetTevSwapModeTable
// (table rows in the KSEL registers): the stage reads the CHANNEL it names, remapped so that
// output component c takes source channel table[row][c]. A row of A,A,A,A therefore presents the
// channel's alpha as an RGB grey — the idiom the plaza terrain uses on colour channel 1.
void test_ras_select_and_swap() {
    SbrTevState t = neutral();
    SbrTevInputs in{};
    in.ras[0] = 0.25f; in.ras[1] = 0.50f; in.ras[2] = 0.75f; in.ras[3] = 0.125f;   // channel 0
    in.ras1[0] = in.ras1[1] = in.ras1[2] = 0.0f; in.ras1[3] = 1.0f;                // channel 1

    float out[4];

    // Stage names channel 0, identity swap: RASC passes through.
    t.stage[0].cD = 10;   // GX_CC_RASC
    sbr_tev_evaluate(t, in, out, nullptr, nullptr);
    check_near(out[0], 0.25f, "ras: channel 0 identity R");
    check_near(out[2], 0.75f, "ras: channel 0 identity B");

    // Same stage through swap row 1 = A,A,A,A: every colour component reads channel 0's alpha.
    t.stage[0].swapRas = 1;
    t.swapTable[1][0] = t.swapTable[1][1] = t.swapTable[1][2] = t.swapTable[1][3] = 3;
    sbr_tev_evaluate(t, in, out, nullptr, nullptr);
    check_near(out[0], 0.125f, "ras: swap AAAA reads alpha into R");
    check_near(out[1], 0.125f, "ras: swap AAAA reads alpha into G");

    // Channel 1 with the same AAAA row: black RGB, but the ALPHA is 1.0 — so RASC is WHITE. This
    // is the exact configuration that renders the plaza terrain bright from a black channel.
    t.stage[0].rasChannel = 1;
    sbr_tev_evaluate(t, in, out, nullptr, nullptr);
    check_near(out[0], 1.0f, "ras: channel 1 AAAA is its alpha, not its black RGB");

    // Channel 7 is the constant ZERO regardless of swap.
    t.stage[0].rasChannel = 7;
    sbr_tev_evaluate(t, in, out, nullptr, nullptr);
    check_near(out[0], 0.0f, "ras: channel 7 is the constant zero");

    // TEX swap: an I-format-style texel read through row 2 = R,G,A,A puts texel alpha in B and A.
    t = neutral();
    t.stage[0].texEnable = 1;
    t.stage[0].cD = 8;   // GX_CC_TEXC
    t.stage[0].swapTex = 2;
    t.swapTable[2][0] = 0; t.swapTable[2][1] = 1; t.swapTable[2][2] = 3; t.swapTable[2][3] = 3;
    in.tex[0][0] = 0.1f; in.tex[0][1] = 0.2f; in.tex[0][2] = 0.3f; in.tex[0][3] = 0.9f;
    sbr_tev_evaluate(t, in, out, nullptr, nullptr);
    check_near(out[0], 0.1f, "tex swap: R stays R");
    check_near(out[2], 0.9f, "tex swap: B reads the texel's alpha");
}

} // namespace

int main() {
    test_lerp_form();
    test_compare_mode();
    test_register_chaining();
    test_texture_disabled_reads_zero();
    test_alpha_test();
    test_konst_ramp();
    test_ras_select_and_swap();
    if (g_failures == 0) {
        std::printf("tev_eval: all checks passed\n");
        return 0;
    }
    std::printf("tev_eval: %d check(s) FAILED\n", g_failures);
    return 1;
}

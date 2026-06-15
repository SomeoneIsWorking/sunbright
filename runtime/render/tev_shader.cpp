// tev_shader — GX TEV combiner → GLSL 450 (see tev_shader.h). The per-stage
// emission mirrors Dolphin's PixelShaderGen integer pipeline, re-derived; the
// input/output/konst tables below are the GX combiner enums.

#include "tev_shader.h"
#include <cstdarg>
#include <cstdio>

namespace {

// GX color-combiner input (4-bit GXTevColorArg) → GLSL .rgb expression.
const char* CC_IN[16] = {
    "prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
    "textemp.rgb", "textemp.aaa", "rastemp.rgb", "rastemp.aaa",
    "ivec3(255,255,255)", "ivec3(128,128,128)", "konsttemp.rgb", "ivec3(0,0,0)",
};
// GX alpha-combiner input (3-bit GXTevAlphaArg) → GLSL .a expression.
const char* CA_IN[8] = {
    "prev.a", "c0.a", "c1.a", "c2.a", "textemp.a", "rastemp.a", "konsttemp.a", "0",
};
// Destination register (2-bit GXTevRegID-ish): Prev/Color0/Color1/Color2.
const char* COUT[4] = { "prev.rgb", "c0.rgb", "c1.rgb", "c2.rgb" };
const char* AOUT[4] = { "prev.a",   "c0.a",   "c1.a",   "c2.a"   };

// Konst color/alpha selection tables (GXTevKColorSel / GXTevKAlphaSel).
const char* KSEL_C[32] = {
    "ivec3(255,255,255)", "ivec3(223,223,223)", "ivec3(191,191,191)", "ivec3(159,159,159)",
    "ivec3(128,128,128)", "ivec3(96,96,96)", "ivec3(64,64,64)", "ivec3(32,32,32)",
    "ivec3(0,0,0)", "ivec3(0,0,0)", "ivec3(0,0,0)", "ivec3(0,0,0)",
    "m.kcolor[0].rgb", "m.kcolor[1].rgb", "m.kcolor[2].rgb", "m.kcolor[3].rgb",
    "m.kcolor[0].rrr", "m.kcolor[1].rrr", "m.kcolor[2].rrr", "m.kcolor[3].rrr",
    "m.kcolor[0].ggg", "m.kcolor[1].ggg", "m.kcolor[2].ggg", "m.kcolor[3].ggg",
    "m.kcolor[0].bbb", "m.kcolor[1].bbb", "m.kcolor[2].bbb", "m.kcolor[3].bbb",
    "m.kcolor[0].aaa", "m.kcolor[1].aaa", "m.kcolor[2].aaa", "m.kcolor[3].aaa",
};
const char* KSEL_A[32] = {
    "255", "223", "191", "159", "128", "96", "64", "32",
    "0", "0", "0", "0", "0", "0", "0", "0",
    "m.kcolor[0].r", "m.kcolor[1].r", "m.kcolor[2].r", "m.kcolor[3].r",
    "m.kcolor[0].g", "m.kcolor[1].g", "m.kcolor[2].g", "m.kcolor[3].g",
    "m.kcolor[0].b", "m.kcolor[1].b", "m.kcolor[2].b", "m.kcolor[3].b",
    "m.kcolor[0].a", "m.kcolor[1].a", "m.kcolor[2].a", "m.kcolor[3].a",
};

struct Comb { int a, b, c, d, bias, op, clamp, scale, dest; };
Comb decode_cc(uint32_t e) {
    return { (int)(e >> 12) & 0xf, (int)(e >> 8) & 0xf, (int)(e >> 4) & 0xf, (int)e & 0xf,
             (int)(e >> 16) & 3, (int)(e >> 18) & 1, (int)(e >> 19) & 1,
             (int)(e >> 20) & 3, (int)(e >> 22) & 3 };
}
Comb decode_ac(uint32_t e) {
    return { (int)(e >> 13) & 7, (int)(e >> 10) & 7, (int)(e >> 7) & 7, (int)(e >> 4) & 7,
             (int)(e >> 16) & 3, (int)(e >> 18) & 1, (int)(e >> 19) & 1,
             (int)(e >> 20) & 3, (int)(e >> 22) & 3 };
}

struct Buf {
    std::string s;
    void w(const char* fmt, ...) {
        char t[512]; va_list ap; va_start(ap, fmt);
        vsnprintf(t, sizeof t, fmt, ap); va_end(ap); s += t;
    }
};

// The regular (non-compare) combiner: (d {bias}{<<scale}) op (lerp(a,b,c){<<scale}{round})>>8 {>>scale}
// Faithful to the GC integer scale-lerp (Dolphin PixelShaderGen WriteTevRegular).
void write_regular(Buf& o, const char* comp, const Comb& k) {
    static const char* bias_l[4]  = { "", " + 128", " - 128", "" };
    static const char* scale_l[4] = { "", " << 1", " << 2", "" };       // left shift (scale 0..2)
    static const char* scale_r[4] = { "", "", "", " >> 1" };            // right shift (Divide2)
    static const char* lerp_bias[2] = { " + 128", " + 127" };           // op add/sub rounding
    const char* op = k.op ? "-" : "+";
    o.w("(((tevin_d.%s%s)%s)", comp, bias_l[k.bias], scale_l[k.scale]);
    o.w(" %s ", op);
    o.w("(((((tevin_a.%s<<8) + (tevin_b.%s-tevin_a.%s)*(tevin_c.%s+(tevin_c.%s>>7)))%s)%s)>>8)",
        comp, comp, comp, comp, comp, scale_l[k.scale], k.scale != 3 ? lerp_bias[k.op] : "");
    o.w(")%s", scale_r[k.scale]);
}

void write_stage(Buf& o, const NgxTevState& st, int n) {
    const NgxTevStage& s = st.stage[n];
    const Comb cc = decode_cc(s.color_env);
    const Comb ac = decode_ac(s.alpha_env);
    o.w("\n  // stage %d\n", n);

    // Raster color (channel). GXChannelID: COLOR0=0, COLOR1=1, COLOR0A0=4, COLOR1A1=5,
    // ZERO=6, COLOR_NULL=0xff. col0 carries the LIT raster colour (N6 native lighting
    // computed per vertex in the producer); col1 ≈ col0; null/zero → 0.
    const bool ras_used = cc.a==10||cc.a==11||cc.b==10||cc.b==11||cc.c==10||cc.c==11||cc.d==10||cc.d==11
                          || ac.a==5||ac.b==5||ac.c==5||ac.d==5;
    if (ras_used) {
        if (s.color_chan == 0 || s.color_chan == 4)       o.w("  rastemp = col0;\n");
        else if (s.color_chan == 1 || s.color_chan == 5)  o.w("  rastemp = col1;\n");
        else                                              o.w("  rastemp = ivec4(0,0,0,0);\n");
    }

    // Texture sample for this stage's texmap (single bound tex / single texcoord).
    const bool tex_used = cc.a==8||cc.a==9||cc.b==8||cc.b==9||cc.c==8||cc.c==9||cc.d==8||cc.d==9
                          || ac.a==4||ac.b==4||ac.c==4||ac.d==4;
    if (tex_used) {
        if (s.texmap < 8) o.w("  textemp = ivec4(round(texture(tex[%u], vUV) * 255.0));\n", s.texmap);
        else              o.w("  textemp = ivec4(255,255,255,255);\n");
    }

    // Konst.
    const bool kon_used = cc.a==14||cc.b==14||cc.c==14||cc.d==14 || ac.a==6||ac.b==6||ac.c==6||ac.d==6;
    if (kon_used)
        o.w("  konsttemp = ivec4(%s, %s);\n", KSEL_C[s.kcsel & 31], KSEL_A[s.kasel & 31]);

    // Inputs (a,b,c masked to 8 bits like the hardware; d unmasked).
    o.w("  tevin_a = ivec4(%s, %s) & ivec4(255);\n", CC_IN[cc.a], CA_IN[ac.a]);
    o.w("  tevin_b = ivec4(%s, %s) & ivec4(255);\n", CC_IN[cc.b], CA_IN[ac.b]);
    o.w("  tevin_c = ivec4(%s, %s) & ivec4(255);\n", CC_IN[cc.c], CA_IN[ac.c]);
    o.w("  tevin_d = ivec4(%s, %s);\n", CC_IN[cc.d], CA_IN[ac.d]);

    // Color combine → dest.rgb.
    o.w("  %s = clamp(", COUT[cc.dest]);
    if (cc.bias != 3) {
        write_regular(o, "rgb", cc);
    } else {
        const int cmp = cc.scale;       // compare_mode
        const int eq  = cc.op;          // 0=GT, 1=EQ
        static const char* gt[4] = {
            "((tevin_a.r > tevin_b.r) ? tevin_c.rgb : ivec3(0))",
            "((idot(tevin_a.rgb,comp16) > idot(tevin_b.rgb,comp16)) ? tevin_c.rgb : ivec3(0))",
            "((idot(tevin_a.rgb,comp24) > idot(tevin_b.rgb,comp24)) ? tevin_c.rgb : ivec3(0))",
            "(max(sign(tevin_a.rgb - tevin_b.rgb), ivec3(0)) * tevin_c.rgb)" };
        static const char* eqt[4] = {
            "((tevin_a.r == tevin_b.r) ? tevin_c.rgb : ivec3(0))",
            "((idot(tevin_a.rgb,comp16) == idot(tevin_b.rgb,comp16)) ? tevin_c.rgb : ivec3(0))",
            "((idot(tevin_a.rgb,comp24) == idot(tevin_b.rgb,comp24)) ? tevin_c.rgb : ivec3(0))",
            "((ivec3(1) - sign(abs(tevin_a.rgb - tevin_b.rgb))) * tevin_c.rgb)" };
        o.w("tevin_d.rgb + %s", eq ? eqt[cmp] : gt[cmp]);
    }
    o.w(cc.clamp ? ", ivec3(0), ivec3(255));\n" : ", ivec3(-1024), ivec3(1023));\n");

    // Alpha combine → dest.a.
    o.w("  %s = clamp(", AOUT[ac.dest]);
    if (ac.bias != 3) {
        write_regular(o, "a", ac);
    } else {
        const int cmp = ac.scale, eq = ac.op;
        static const char* gt[4] = {
            "((tevin_a.r > tevin_b.r) ? tevin_c.a : 0)",
            "((idot(tevin_a.rgb,comp16) > idot(tevin_b.rgb,comp16)) ? tevin_c.a : 0)",
            "((idot(tevin_a.rgb,comp24) > idot(tevin_b.rgb,comp24)) ? tevin_c.a : 0)",
            "((tevin_a.a > tevin_b.a) ? tevin_c.a : 0)" };
        static const char* eqt[4] = {
            "((tevin_a.r == tevin_b.r) ? tevin_c.a : 0)",
            "((idot(tevin_a.rgb,comp16) == idot(tevin_b.rgb,comp16)) ? tevin_c.a : 0)",
            "((idot(tevin_a.rgb,comp24) == idot(tevin_b.rgb,comp24)) ? tevin_c.a : 0)",
            "((tevin_a.a == tevin_b.a) ? tevin_c.a : 0)" };
        o.w("tevin_d.a + %s", eq ? eqt[cmp] : gt[cmp]);
    }
    o.w(ac.clamp ? ", 0, 255);\n" : ", -1024, 1023);\n");
}

}  // namespace

std::string sb_tev_gen_fragment(const NgxTevState& st) {
    Buf o;
    o.w("#version 450\n");
    o.w("layout(location=0) in vec4 vColor;\n");
    o.w("layout(location=1) in vec2 vUV;\n");
    o.w("layout(location=0) out vec4 o;\n");
    o.w("layout(set=0, binding=0) uniform sampler2D tex[8];\n");   // one per GX texmap
    o.w("layout(push_constant) uniform Mat { ivec4 kcolor[4]; ivec4 tevreg[3]; } m;\n");
    o.w("int idot(ivec3 a, ivec3 b){ return a.x*b.x + a.y*b.y + a.z*b.z; }\n");
    o.w("void main() {\n");
    o.w("  ivec3 comp16 = ivec3(1,256,0), comp24 = ivec3(1,256,256*256);\n");
    o.w("  ivec4 prev = ivec4(0,0,0,0);\n");                  // PREV register default
    o.w("  ivec4 c0 = m.tevreg[0], c1 = m.tevreg[1], c2 = m.tevreg[2];\n");
    o.w("  ivec4 col0 = ivec4(round(vColor * 255.0));\n");
    o.w("  ivec4 col1 = col0;\n");                            // 2nd raster channel ≈ color0
    o.w("  ivec4 textemp = ivec4(255), rastemp = ivec4(0), konsttemp = ivec4(0);\n");
    o.w("  ivec4 tevin_a, tevin_b, tevin_c, tevin_d;\n");
    int ns = st.num_stages; if (ns < 1) ns = 1; if (ns > 16) ns = 16;
    for (int n = 0; n < ns; n++) write_stage(o, st, n);
    o.w("\n  o = clamp(vec4(prev) / 255.0, 0.0, 1.0);\n");
    o.w("}\n");
    return o.s;
}

// ── /tevshader self-test ────────────────────────────────────────────────────────
// Generate shaders for a couple of synthetic combiners (GX_MODULATE / GX_REPLACE)
// plus the first few LIVE captured materials, compile each through our own
// glslang wrapper (glsl_compile), and report which compiled. Proves the generator
// emits valid GLSL for the real scene's materials.
#include "glsl_compile.h"

static NgxTevState make_modulate() {
    NgxTevState st{}; st.num_stages = 1;
    // GX_MODULATE: cc a=ZERO(15) b=TEXC(8) c=RASC(10) d=ZERO(15), clamp, dest=prev.
    st.stage[0].color_env = (15u<<12)|(8u<<8)|(10u<<4)|15u | (1u<<19);
    // ac a=ZERO(7) b=TEXA(4) c=RASA(5) d=ZERO(7), clamp, dest=prev.
    st.stage[0].alpha_env = (7u<<13)|(4u<<10)|(5u<<7)|(7u<<4) | (1u<<19);
    st.stage[0].texmap = 0; st.stage[0].texcoord = 0; st.stage[0].color_chan = 4;
    for (auto& k : st.kcolor) { k[0]=k[1]=k[2]=k[3]=255; }
    return st;
}
static NgxTevState make_replace() {
    NgxTevState st{}; st.num_stages = 1;
    // GX_REPLACE: cc out = texColor (a=ZERO b=ZERO c=ZERO d=TEXC(8)), clamp.
    st.stage[0].color_env = (15u<<12)|(15u<<8)|(15u<<4)|8u | (1u<<19);
    st.stage[0].alpha_env = (7u<<13)|(7u<<10)|(7u<<7)|(4u<<4) | (1u<<19);
    st.stage[0].texmap = 0; st.stage[0].color_chan = 0xff;
    return st;
}

int sb_tev_shader_selftest(char* out, int cap) {
    int n = 0;
    auto app = [&](const char* fmt, ...) {
        if (n >= cap) return; va_list ap; va_start(ap, fmt);
        n += vsnprintf(out + n, cap - n, fmt, ap); va_end(ap);
    };
    int compiled = 0, attempted = 0, failed = 0;
    auto check = [&](const char* label, const NgxTevState& st) {
        std::string src = sb_tev_gen_fragment(st);
        attempted++;
        auto spv = sb_compile_fragment_glsl(src);
        const bool ok = !spv.empty();
        app("  %-14s stages=%2u glsl=%zuB spv=%zu -> %s\n", label, st.num_stages, src.size(),
            spv.size(), ok ? "COMPILED" : "COMPILE-FAIL");
        if (!ok) { failed++; app("---- failing GLSL ----\n%s----\n", src.c_str()); }
        else compiled++;
    };

    app("tev_shader self-test:\n");
    check("modulate", make_modulate());
    check("replace",  make_replace());

    int nst = 0; const NgxTevState* sts = ngx_snap_tevstates(&nst);
    app("  live captured materials: %d\n", nst);
    for (int i = 0; i < nst && i < 12; i++) {
        char lbl[24]; snprintf(lbl, sizeof lbl, "live[%d]", i);
        check(lbl, sts[i]);
    }
    app("verdict=%s (compiled=%d/%d, failed=%d)\n",
        failed == 0 ? "OK" : "FAIL", compiled, attempted, failed);
    return failed;
}

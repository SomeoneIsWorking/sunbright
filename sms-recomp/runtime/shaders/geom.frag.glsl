#version 450
// TEV evaluation, driven by uniforms rather than by a generated shader.
//
// Dolphin and aurora generate a shader per TEV configuration. That needs a GLSL->SPIR-V compiler at
// runtime; this port has none, and adding one to draw a frame would be a large dependency for a
// mechanism GX expresses as plain data. So the stages are uploaded as data and evaluated in a loop:
// same arithmetic, no codegen, and every selector keeps its hardware meaning.
//
// Per stage GX computes, for colour and alpha independently:
//     out = (d + sign * ((1-c)*a + c*b) + bias) * scale     (optionally clamped to [0,1])
// and writes it to one of four registers, with the previous stage's output readable as PREV.

layout(location = 0) in vec4 v_col;
layout(location = 1) in vec4 v_uv01;
layout(location = 2) in vec4 v_uv23;
layout(location = 0) out vec4 o_col;

// One sampler per GX texture unit. A stage names BOTH the map it samples and the coordinate it
// samples with, and they are independent selectors — most materials here use more than one of each.
layout(set = 2, binding = 0) uniform sampler2D u_tex0;
layout(set = 2, binding = 1) uniform sampler2D u_tex1;
layout(set = 2, binding = 2) uniform sampler2D u_tex2;
layout(set = 2, binding = 3) uniform sampler2D u_tex3;

// std140: each stage packs its selectors as integers. 16 stages is GX's maximum.
layout(set = 3, binding = 0) uniform TevBlock {
    ivec4 cSel[16];      // a, b, c, d           (GXTevColorArg)
    ivec4 cOp[16];       // bias, sub, clamp, scale
    ivec4 aSel[16];      // a, b, c, d           (GXTevAlphaArg)
    ivec4 aOp[16];       // bias, sub, clamp, scale
    ivec4 dest[16];      // cDest, aDest, texEnable, texmap | texcoord << 8
    vec4  konst[16];     // resolved konst colour for the stage (rgb) + konst alpha (a)
    vec4  regInit[4];    // prev, c0, c1, c2 as the material set them
    ivec4 control;       // x = numStages, y = alphaOp0, z = alphaOp1, w = alphaLogic
    vec4  alphaRef;      // x = ref0, y = ref1 (0..255)
} tev;

// GXCompare against an 8-bit alpha. The comparison is done on the QUANTISED value because that is
// what the hardware compares: a cutout whose texels sit exactly on the reference flips entirely on
// whether the rounding matches, and half a level of drift shows up as a fringe on every leaf edge.
bool alphaCompare(int op, int a, int ref) {
    switch (op) {
    case 0: return false;         // NEVER
    case 1: return a <  ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a >  ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return true;         // ALWAYS
    }
}

vec4 g_reg[4];

vec3 colorArg(int sel, vec4 texc, vec4 rasc, vec4 konst) {
    // GXTevColorArg, in hardware order.
    switch (sel) {
    case 0:  return g_reg[0].rgb;            // CPREV
    case 1:  return vec3(g_reg[0].a);        // APREV
    case 2:  return g_reg[1].rgb;            // C0
    case 3:  return vec3(g_reg[1].a);        // A0
    case 4:  return g_reg[2].rgb;            // C1
    case 5:  return vec3(g_reg[2].a);        // A1
    case 6:  return g_reg[3].rgb;            // C2
    case 7:  return vec3(g_reg[3].a);        // A2
    case 8:  return texc.rgb;                // TEXC
    case 9:  return vec3(texc.a);            // TEXA
    case 10: return rasc.rgb;                // RASC
    case 11: return vec3(rasc.a);            // RASA
    case 12: return vec3(1.0);               // ONE
    case 13: return vec3(0.5);               // HALF
    case 14: return konst.rgb;               // KONST
    default: return vec3(0.0);               // ZERO
    }
}

float alphaArg(int sel, vec4 texc, vec4 rasc, vec4 konst) {
    switch (sel) {
    case 0:  return g_reg[0].a;   // APREV
    case 1:  return g_reg[1].a;   // A0
    case 2:  return g_reg[2].a;   // A1
    case 3:  return g_reg[3].a;   // A2
    case 4:  return texc.a;       // TEXA
    case 5:  return rasc.a;       // RASA
    case 6:  return konst.a;      // KONST
    default: return 0.0;          // ZERO
    }
}

float biasOf(int b) { return b == 1 ? 0.5 : (b == 2 ? -0.5 : 0.0); }
float scaleOf(int s) { return s == 1 ? 2.0 : (s == 2 ? 4.0 : (s == 3 ? 0.5 : 1.0)); }

// GX TEV COMPARE MODE. GXSetTevColorOp (GXTev.c) writes bias = 3 for every op >= GX_TEV_COMP_R8_GT
// and repurposes the two fields around it: the SCALE field carries (op >> 1) & 3, the comparison
// WIDTH, and the subtract bit carries op & 1, which selects == over >. The stage then computes
//     out = d + (cmp(a, b) ? c : 0)
// with no bias and no scale — a MASK, not a blend. Evaluating it as an ordinary blend makes the
// stage pass its texture through where the hardware would have thresholded it to 0 or 1, so every
// material that gates one texture by another (539 of 896 plaza drawables use one) comes out wrong.
//
// The comparison is on the 8-BIT quantised values, packed little-endian across channels for the
// multi-byte widths, exactly as the hardware's comparator sees them.
int u8(float v) { return int(clamp(v, 0.0, 1.0) * 255.0 + 0.5); }

bool tevCmpPacked(int width, bool isEq, vec3 a, vec3 b) {
    int av, bv;
    if (width == 0) {          // R8
        av = u8(a.r);
        bv = u8(b.r);
    } else if (width == 1) {   // GR16
        av = (u8(a.g) << 8) | u8(a.r);
        bv = (u8(b.g) << 8) | u8(b.r);
    } else {                   // BGR24
        av = (u8(a.b) << 16) | (u8(a.g) << 8) | u8(a.r);
        bv = (u8(b.b) << 16) | (u8(b.g) << 8) | u8(b.r);
    }
    return isEq ? (av == bv) : (av > bv);
}

bool cmp1(bool isEq, float a, float b) {
    return isEq ? (u8(a) == u8(b)) : (u8(a) > u8(b));
}

// Sample every unit up front. The alternative — indexing inside the stage loop — puts an implicit
// LOD fetch under control flow that the compiler cannot prove uniform, which is undefined in GLSL.
// Four fetches is the honest cost of doing it correctly; GX itself has four units live.
vec4 sampleUnit(int unit, vec2 uv) {
    if (unit == 1) return texture(u_tex1, uv);
    if (unit == 2) return texture(u_tex2, uv);
    if (unit == 3) return texture(u_tex3, uv);
    return texture(u_tex0, uv);
}

vec2 coordOf(int idx) {
    if (idx == 1) return v_uv01.zw;
    if (idx == 2) return v_uv23.xy;
    if (idx == 3) return v_uv23.zw;
    return v_uv01.xy;
}

void main() {
    vec4 ras = v_col;

    g_reg[0] = tev.regInit[0];
    g_reg[1] = tev.regInit[1];
    g_reg[2] = tev.regInit[2];
    g_reg[3] = tev.regInit[3];

    int n = clamp(tev.control.x, 1, 16);
    for (int i = 0; i < n; ++i) {
        // A stage with its texture disabled must not read the texture: GX feeds it nothing, and
        // sampling anyway would tint untextured stages with whatever was last bound.
        vec4 t = (tev.dest[i].z != 0)
                     ? sampleUnit(tev.dest[i].w & 3, coordOf((tev.dest[i].w >> 8) & 3))
                     : vec4(0.0);
        vec4 k = tev.konst[i];

        vec3 ca = colorArg(tev.cSel[i].x, t, ras, k);
        vec3 cb = colorArg(tev.cSel[i].y, t, ras, k);
        vec3 cc = colorArg(tev.cSel[i].z, t, ras, k);
        vec3 cd = colorArg(tev.cSel[i].w, t, ras, k);
        vec3 cr;
        if (tev.cOp[i].x == 3) {
            // Compare: RGB8 (width 3) compares each channel independently; the narrower widths
            // compare one packed integer and gate all three channels together.
            const int width = tev.cOp[i].w;
            const bool isEq = tev.cOp[i].y != 0;
            if (width == 3)
                cr = cd + vec3(cmp1(isEq, ca.r, cb.r) ? cc.r : 0.0,
                               cmp1(isEq, ca.g, cb.g) ? cc.g : 0.0,
                               cmp1(isEq, ca.b, cb.b) ? cc.b : 0.0);
            else
                cr = cd + (tevCmpPacked(width, isEq, ca, cb) ? cc : vec3(0.0));
        } else {
            cr = cd + (tev.cOp[i].y != 0 ? -1.0 : 1.0) * (mix(ca, cb, cc)) + biasOf(tev.cOp[i].x);
            cr *= scaleOf(tev.cOp[i].w);
        }
        if (tev.cOp[i].z != 0) cr = clamp(cr, 0.0, 1.0);

        float aa = alphaArg(tev.aSel[i].x, t, ras, k);
        float ab = alphaArg(tev.aSel[i].y, t, ras, k);
        float ac = alphaArg(tev.aSel[i].z, t, ras, k);
        float ad = alphaArg(tev.aSel[i].w, t, ras, k);
        float ar;
        if (tev.aOp[i].x == 3) {
            // The alpha unit shares the comparator with the colour unit: only A8 (width 3) compares
            // the alpha pair — the narrower widths compare the COLOUR inputs and gate the alpha
            // result with them, which is how a material keys its alpha off an RGB threshold.
            const int width = tev.aOp[i].w;
            const bool isEq = tev.aOp[i].y != 0;
            const bool hit = (width == 3) ? cmp1(isEq, aa, ab) : tevCmpPacked(width, isEq, ca, cb);
            ar = ad + (hit ? ac : 0.0);
        } else {
            ar = ad + (tev.aOp[i].y != 0 ? -1.0 : 1.0) * (mix(aa, ab, ac)) + biasOf(tev.aOp[i].x);
            ar *= scaleOf(tev.aOp[i].w);
        }
        if (tev.aOp[i].z != 0) ar = clamp(ar, 0.0, 1.0);

        g_reg[tev.dest[i].x].rgb = cr;
        g_reg[tev.dest[i].y].a   = ar;
    }
    vec4 outc = clamp(g_reg[0], 0.0, 1.0);

    // ALPHA TEST, before the blend. Two comparisons combined by a logic op — GX can express a band
    // (ref0 <= a <= ref1) as well as a plain cutout, so both are evaluated rather than only the
    // first. A failing texel is DISCARDED: it writes neither colour nor depth, which is the whole
    // point for foliage drawn as opaque quads.
    int a8 = int(outc.a * 255.0 + 0.5);
    bool c0 = alphaCompare(tev.control.y, a8, int(tev.alphaRef.x));
    bool c1 = alphaCompare(tev.control.z, a8, int(tev.alphaRef.y));
    bool pass;
    switch (tev.control.w) {
    case 0:  pass = c0 && c1; break;
    case 1:  pass = c0 || c1; break;
    case 2:  pass = c0 != c1; break;
    default: pass = c0 == c1; break;
    }
    if (!pass) discard;

    o_col = outc;
}

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
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

layout(set = 2, binding = 0) uniform sampler2D u_tex;

// std140: each stage packs its selectors as integers. 16 stages is GX's maximum.
layout(set = 3, binding = 0) uniform TevBlock {
    ivec4 cSel[16];      // a, b, c, d           (GXTevColorArg)
    ivec4 cOp[16];       // bias, sub, clamp, scale
    ivec4 aSel[16];      // a, b, c, d           (GXTevAlphaArg)
    ivec4 aOp[16];       // bias, sub, clamp, scale
    ivec4 dest[16];      // cDest, aDest, texEnable, unused
    vec4  konst[16];     // resolved konst colour for the stage (rgb) + konst alpha (a)
    vec4  regInit[4];    // prev, c0, c1, c2 as the material set them
    ivec4 control;       // x = numStages
} tev;

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

void main() {
    vec4 tex = texture(u_tex, v_uv);
    vec4 ras = v_col;

    g_reg[0] = tev.regInit[0];
    g_reg[1] = tev.regInit[1];
    g_reg[2] = tev.regInit[2];
    g_reg[3] = tev.regInit[3];

    int n = clamp(tev.control.x, 1, 16);
    for (int i = 0; i < n; ++i) {
        // A stage with its texture disabled must not read the texture: GX feeds it nothing, and
        // sampling anyway would tint untextured stages with whatever was last bound.
        vec4 t = (tev.dest[i].z != 0) ? tex : vec4(0.0);
        vec4 k = tev.konst[i];

        vec3 ca = colorArg(tev.cSel[i].x, t, ras, k);
        vec3 cb = colorArg(tev.cSel[i].y, t, ras, k);
        vec3 cc = colorArg(tev.cSel[i].z, t, ras, k);
        vec3 cd = colorArg(tev.cSel[i].w, t, ras, k);
        vec3 cr = cd + (tev.cOp[i].y != 0 ? -1.0 : 1.0) * (mix(ca, cb, cc)) + biasOf(tev.cOp[i].x);
        cr *= scaleOf(tev.cOp[i].w);
        if (tev.cOp[i].z != 0) cr = clamp(cr, 0.0, 1.0);

        float aa = alphaArg(tev.aSel[i].x, t, ras, k);
        float ab = alphaArg(tev.aSel[i].y, t, ras, k);
        float ac = alphaArg(tev.aSel[i].z, t, ras, k);
        float ad = alphaArg(tev.aSel[i].w, t, ras, k);
        float ar = ad + (tev.aOp[i].y != 0 ? -1.0 : 1.0) * (mix(aa, ab, ac)) + biasOf(tev.aOp[i].x);
        ar *= scaleOf(tev.aOp[i].w);
        if (tev.aOp[i].z != 0) ar = clamp(ar, 0.0, 1.0);

        g_reg[tev.dest[i].x].rgb = cr;
        g_reg[tev.dest[i].y].a   = ar;
    }
    o_col = clamp(g_reg[0], 0.0, 1.0);
}

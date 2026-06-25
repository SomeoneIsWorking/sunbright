// gx_imm_glyph_test.cpp — regression test for the GXEnd-less glyph-quad capture bug.
//
// JUTResFont::drawChar_scale emits a GX_QUADS glyph with NO GXEnd (a HW no-op on GC: a
// primitive auto-terminates after the GXBegin-declared vertex count). Each vertex is
// streamed as pos THEN colour THEN texcoord. An earlier capture flushed the primitive on
// the nverts-th GXPosition — i.e. BEFORE the final vertex's colour+texcoord arrived — so
// the glyph's top-left corner got the running (previous) colour and uv=(0,0) instead of
// its real corner UV. The top-left triangle of every glyph then sampled the wrong texels
// → glyphs rendered faint/half-smeared ("Select data." was barely visible).
//
// Fix: defer the GXEnd-less flush to the next GXBegin / GXEnd / present-take, after all
// attributes of the last vertex are in. This drives the exact drawChar sequence and asserts
// all four corners keep their own colour AND texcoord.

#include "gx_imm_xform.h"
#include <cstdio>
#include <cmath>

using sb::render::SbImmVtx;
using sb::render::SbImmBatch;

extern "C" {
void sb_gx_imm_begin(int prim, int vtxfmt, int nverts);
void sb_gx_imm_pos(float x, float y, float z);
void sb_gx_imm_color_u32(unsigned c);
void sb_gx_imm_texcoord_f2(float s, float t);
int  sb_gx_imm_take_batches(const SbImmVtx** verts, const SbImmBatch** batches, int* nbatch);
}

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}
static bool feq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// Emit one glyph quad in EXACT drawChar order: pos -> colour -> texcoord per vertex, no GXEnd.
// Corner UVs (u1,v1)=BL, (u2,v1)=BR, (u2,v2)=TR, (u1,v2)=TL; colours c0..c3.
static void emit_glyph(float u1, float v1, float u2, float v2,
                       unsigned c0, unsigned c1, unsigned c2, unsigned c3) {
    sb_gx_imm_begin(/*GX_QUADS*/0x80, /*vtxfmt0*/0, /*nverts*/4);
    sb_gx_imm_pos(0, 0, 0); sb_gx_imm_color_u32(c0); sb_gx_imm_texcoord_f2(u1, v1);  // BL
    sb_gx_imm_pos(1, 0, 0); sb_gx_imm_color_u32(c1); sb_gx_imm_texcoord_f2(u2, v1);  // BR
    sb_gx_imm_pos(1, 1, 0); sb_gx_imm_color_u32(c2); sb_gx_imm_texcoord_f2(u2, v2);  // TR
    sb_gx_imm_pos(0, 1, 0); sb_gx_imm_color_u32(c3); sb_gx_imm_texcoord_f2(u1, v2);  // TL
    // intentionally NO sb_gx_imm_end() — matches drawChar's HW-noop-omitted GXEnd.
}

int main() {
    // Two consecutive GXEnd-less glyph quads (so the FIRST is flushed by the SECOND's
    // GXBegin, the SECOND by the present-take) — the real printReturn per-char loop.
    const float u1 = 0.10f, v1 = 0.20f, u2 = 0.30f, v2 = 0.40f;
    emit_glyph(u1, v1, u2, v2, 0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff);
    const float U1 = 0.50f, V1 = 0.60f, U2 = 0.70f, V2 = 0.80f;
    emit_glyph(U1, V1, U2, V2, 0x112233ff, 0x445566ff, 0x778899ff, 0xaabbccff);

    const SbImmVtx* verts = nullptr;
    const SbImmBatch* batches = nullptr; int nb = 0;
    int nv = sb_gx_imm_take_batches(&verts, &batches, &nb);

    // 2 quads -> 2 tris each -> 12 verts. GX_QUADS triangulates (0,1,2)(0,2,3): the TL
    // corner (v3) appears only in the second triangle, index 5.
    chk(nv == 12, "two glyph quads -> 12 triangulated verts");
    if (nv == 12) {
        // First quad: tri0=(BL,BR,TR), tri1=(BL,TR,TL). TL is verts[5].
        const SbImmVtx& tl0 = verts[5];
        chk(feq(tl0.u, u1) && feq(tl0.v, v2), "quad0 top-left keeps its texcoord (was uv=(0,0))");
        chk(feq(tl0.r, 1) && feq(tl0.g, 1) && feq(tl0.b, 1) && feq(tl0.a, 1),
            "quad0 top-left keeps its own colour (white)");
        // Second quad TL is verts[11].
        const SbImmVtx& tl1 = verts[11];
        chk(feq(tl1.u, U1) && feq(tl1.v, V2), "quad1 top-left keeps its texcoord");
        // 0xaabbcc -> r=0xaa/255, g=0xbb/255, b=0xcc/255
        chk(feq(tl1.r, 0xaa/255.0f) && feq(tl1.g, 0xbb/255.0f) && feq(tl1.b, 0xcc/255.0f),
            "quad1 top-left keeps its own colour");
        // Sanity: the bottom-left (verts[0]) corner UV is correct too.
        chk(feq(verts[0].u, u1) && feq(verts[0].v, v1), "quad0 bottom-left texcoord");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

#include "gx_fifo_2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <lucent/log.h>

#include "cpu_state.h"
#include "scene.h"

namespace {

// SBR_FIFO_2D captures HUD/menu geometry from direct FIFO draws. Raw-GX gauges, per-glyph text,
// screen fills, and J2D paths all serialize their real geometry here, while no object-level seam
// can cover all of them. The stream projection is the gate because SDK-captured projection state
// is not synchronized with the command position being decoded.
unsigned long g_2dSeen = 0, g_2dOrtho = 0, g_2dEmitted = 0;
// Every decline has a counted reason so the report can prove its breakdown is complete instead of
// selecting work from whichever one-shot warning happened to appear first.
unsigned long g_2dDeclPrim = 0, g_2dDeclAttr = 0, g_2dDeclMtxIdx = 0, g_2dDeclPosFmt = 0,
              g_2dDeclClr = 0, g_2dDeclCount = 0, g_2dDeclEmpty = 0;
unsigned long g_2dDeclPrimOp[32] = {};
unsigned long g_2dDeclPosFmtSeen[8] = {};
unsigned long g_2dDeclAttrPos[4] = {}, g_2dDeclAttrT0[4] = {};
unsigned long g_2dIdxNearCapture = 0, g_2dIdxFarFromCapture = 0;
unsigned long g_2dDeclArrayMiss = 0;
unsigned long g_2dDeclMtxUnset = 0;
unsigned long g_2dInVol[2] = {}, g_2dPartVol[2] = {}, g_2dOutVol[2] = {};
unsigned long g_2dCollapsed[2] = {};
unsigned long g_2dTexMtxIdxDraws = 0;

bool fifo2DEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        const char* value = std::getenv("SBR_FIFO_2D");
        enabled = (value != nullptr && value[0] != '\0' && value[0] != '0') ? 1 : 0;
    }
    return enabled == 1;
}

bool decodeDraw(const GxFifo2DDrawContext& context, u32 op, const u8* vp, u32 verts,
                const GxFifoVat& v) {
    const u32 prim = op & 0xF8;
    // The 2D paths draw QUADS (J2DPicture, glyphs, gauge) and the occasional strip/fan for fills.
    if (prim != 0x80 && prim != 0x90 && prim != 0x98 && prim != 0xA0 && prim != 0xA8) {
        ++g_2dDeclPrim;
        ++g_2dDeclPrimOp[(prim >> 3) & 31];
        // WHAT are they? A count of 22,776 declined line draws is a number, not a work item — it
        // says nothing about whether they are the HUD, a debug overlay, or something invisible.
        // Sample the first few with the state that identifies them: vertex count, and the texture
        // bound to unit 0 (0 = untextured, which a wireframe would be).
        if (g_2dDeclPrim <= 6)
            lucent::info("fifo2d",
                         "declined prim 0x{:02x}: {} vert(s), unit0 image3 0x{:08x}, "
                         "vsize {} — sample {} of the first 6",
                         prim, verts, context.unit0Image3, gxFifoVertexSize(v), g_2dDeclPrim);
        return false;
    }
    const u32 pos_mode = (v.vcd_lo >> 9) & 3;
    const u32 c0_mode = (v.vcd_lo >> 13) & 3;
    const u32 t0_mode = v.vcd_hi & 3;
    // INDEXED attributes are now resolved, not declined. Measured before implementing: of 5,510
    // indexed ortho draws in a 300-present plaza run, ZERO were within four draw commands of a
    // J3DShape::draw capture — so none of them is geometry the J3D seam already holds, and
    // decoding them adds to the scene rather than double-counting into it. The comment that used
    // to sit here asserted the opposite ("J2D never uses them, so their appearance means the gate
    // caught a draw it was never meant to capture"); the distance-to-capture measurement falsifies
    // it, and this is what replaces it.
    const bool pos_indexed = (pos_mode == 2 || pos_mode == 3);
    const bool t0_indexed = (t0_mode == 2 || t0_mode == 3);
    if ((pos_mode != 1 && !pos_indexed) || (c0_mode != 0 && c0_mode != 1) ||
        (t0_mode != 0 && t0_mode != 1 && !t0_indexed)) {
        ++g_2dDeclAttr;
        ++g_2dDeclAttrPos[pos_mode & 3];
        ++g_2dDeclAttrT0[t0_mode & 3];
        // The comment above this function asserts J2D never uses indexed attributes, so a draw
        // arriving here is one the gate caught that it was never meant to. That is a CLAIM, and
        // whether these 7,410 are HUD the capture is missing or 3D geometry the ortho gate is
        // wrongly admitting decides whether decoding them is work worth doing. Vertex count and
        // primitive separate the two: J2D quads are 4-6 vertices, world geometry is not.
        // WHOSE draw is it? context.drawsSinceCapture counts draw commands since the last
        // J3DShape::draw capture. A draw that the J3D seam already captured sits within a few
        // commands of one; a draw nothing captured sits far from any. That decides the question
        // decoding cannot: whether these are HUD the capture is MISSING, or geometry it already
        // has — in which case decoding them here would double-count it into the scene.
        if (context.drawsSinceCapture <= 4)
            ++g_2dIdxNearCapture;
        else
            ++g_2dIdxFarFromCapture;
        if (g_2dDeclAttr <= 6)
            lucent::info("fifo2d",
                         "declined indexed: prim 0x{:02x}, {} vert(s), unit0 image3 "
                         "0x{:08x}, {} draw(s) since the last J3D capture — sample {} "
                         "of the first 6",
                         prim, verts, context.unit0Image3, context.drawsSinceCapture, g_2dDeclAttr);
        return false;
    }
    // PosNrmMatIdx and TexMtxIdx0-7 are per-vertex INDEX BYTES that precede the position in each
    // vertex. They used to decline the draw, under a comment asserting J2D never emits them; the
    // stream says otherwise (see context.positionMatrices). They are handled in the vertex loop
    // below.
    const bool has_pnidx = (v.vcd_lo & 1) != 0;
    u32 idx_prefix = has_pnidx ? 1u : 0u;
    for (int ti = 0; ti < 8; ++ti)
        if (v.vcd_lo & (1u << (1 + ti)))
            ++idx_prefix;
    const bool has_texmtxidx = idx_prefix > (has_pnidx ? 1u : 0u);
    const u32 pos_cnt = ((v.fmt0 >> 0) & 1) ? 3 : 2;
    const u32 pos_fmt = (v.fmt0 >> 1) & 7;
    const u32 pos_shift = (v.fmt0 >> 4) & 0x1F;
    const u32 c0_comp = (v.fmt0 >> 14) & 7;
    const u32 t0_elem = (v.fmt0 >> 21) & 1;
    const u32 t0_fmt = (v.fmt0 >> 22) & 7;
    const u32 t0_shift = (v.fmt0 >> 25) & 0x1F;
    // s16 / u16 / f32. u16 was excluded on the same "2D only emits these" reasoning as the gates
    // above, and is 870 draws a run. It differs from s16 only in the sign of the decode.
    if (pos_fmt != 2 && pos_fmt != 3 && pos_fmt != 4) {
        ++g_2dDeclPosFmt;
        ++g_2dDeclPosFmtSeen[pos_fmt & 7];
        return false;
    }
    if (c0_mode == 1 && c0_comp != 5) { // RGBA8 only (setup2D's declared format)
        ++g_2dDeclClr;
        return false;
    }
    if (verts < 3 || verts > 512) {
        ++g_2dDeclCount;
        return false;
    }

    const u32 vsize = gxFifoVertexSize(v);
    const float posDiv = (pos_fmt == 2 || pos_fmt == 3) ? (float)(1u << pos_shift) : 1.0f;
    const float texDiv = (t0_fmt == 2 || t0_fmt == 3) ? (float)(1u << t0_shift) : 1.0f;

    // STATIC, not stack. These are ~117KB and ~39KB; as locals they put ~160KB on the stack of a
    // FIFO-parse callback, which is a stack overflow waiting to happen and did in fact coincide
    // with the whole frame collapsing to the clear colour. The parser is single-threaded by
    // construction (the one-runtime doctrine), so static is safe here and the sizes stay off the
    // stack.
    static SbrGeomVert out[512 * 3];
    static SbrGeomVert vtx[512];
    u32 nOut = 0;
    for (u32 k = 0; k < verts; ++k) {
        const u8* q = vp + (size_t)k * vsize;
        SbrGeomVert g{};

        // The per-vertex index bytes come first, PNMTXIDX before the texture-matrix indices.
        // Texture-matrix indices are consumed but not yet applied: this decoder does not implement
        // texgen matrices, so the UVs it produces for such a vertex are the raw ones. That is a
        // KNOWN gap, reported by the gate summary, not a silent approximation.
        const float* M = context.positionMatrices;
        if (has_pnidx) {
            const u32 mi = q[0];
            if (mi >= 61 || !context.positionMatrixRowsSet[mi]) {
                ++g_2dDeclMtxUnset;
                return false;
            }
            M = &context.positionMatrices[(size_t)mi * 4];
        }
        q += idx_prefix;

        // Resolve an indexed attribute to a pointer into its array. GX array ids: 0 POS, 1 NRM,
        // 2 CLR0, 3 CLR1, 4..11 TEX0..TEX7, 12..15 XF_A..XF_D (the matrix arrays). Returns nullptr
        // when the array was never registered or the element would run past MEM1 — the caller then
        // declines the whole draw rather than decoding whatever bytes happen to be there, because a
        // plausible-but-wrong vertex is this project's most-repeated failure and is far worse than
        // a missing one.
        const auto array_elem = [&](u32 arr, u32 idx) -> const u8* {
            const u32 base = context.arrayBase[arr & 15], stride = context.arrayStride[arr & 15];
            if (base == 0 || stride == 0)
                return nullptr;
            const u32 off = (base & 0x01FFFFFFu) + idx * stride;
            if (off + stride > 0x01800000u)
                return nullptr;
            return context.ram + off;
        };
        const auto read_index = [&](u32 mode, const u8*& cur) -> u32 {
            if (mode == 2) {
                const u32 v2 = cur[0];
                cur += 1;
                return v2;
            }
            const u32 v2 = ((u32)cur[0] << 8) | cur[1];
            cur += 2;
            return v2;
        };

        // POS
        float px, py, pz = 0.0f;
        const u8* pq = q; // where the position bytes actually live
        if (pos_indexed) {
            const u32 idx = read_index(pos_mode, q);
            pq = array_elem(0, idx);
            if (pq == nullptr) {
                ++g_2dDeclArrayMiss;
                return false;
            }
        }
        {
            const u8* q = pq; // shadow, so the decoders below read from the array
            if (pos_fmt == 3) {
                px = (float)(s16)((q[0] << 8) | q[1]) / posDiv;
                py = (float)(s16)((q[2] << 8) | q[3]) / posDiv;
                if (pos_cnt == 3)
                    pz = (float)(s16)((q[4] << 8) | q[5]) / posDiv;
            } else if (pos_fmt == 2) {
                px = (float)(u16)((q[0] << 8) | q[1]) / posDiv;
                py = (float)(u16)((q[2] << 8) | q[3]) / posDiv;
                if (pos_cnt == 3)
                    pz = (float)(u16)((q[4] << 8) | q[5]) / posDiv;
            } else {
                union {
                    u32 u;
                    float f;
                } c;
                c.u = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
                px = c.f;
                c.u = ((u32)q[4] << 24) | ((u32)q[5] << 16) | ((u32)q[6] << 8) | q[7];
                py = c.f;
                if (pos_cnt == 3) {
                    c.u = ((u32)q[8] << 24) | ((u32)q[9] << 16) | ((u32)q[10] << 8) | q[11];
                    pz = c.f;
                }
            }
        }
        if (!pos_indexed)
            q += pos_cnt * (pos_fmt == 4 ? 4u : 2u);
        // The vertex's own position matrix (PNMTX0 when the stream carries no index).
        g.x = M[0] * px + M[1] * py + M[2] * pz + M[3];
        g.y = M[4] * px + M[5] * py + M[6] * pz + M[7];
        g.z = M[8] * px + M[9] * py + M[10] * pz + M[11];
        g.nz = 1.0f;
        // CLR0
        if (c0_mode == 1) {
            g.rgba = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
            q += 4;
        } else {
            g.rgba = 0xFFFFFFFFu;
        }
        // TEX0 — direct or indexed, resolved the same way POS is.
        if (t0_mode == 1 || t0_indexed) {
            const u8* tq = q;
            if (t0_indexed) {
                const u32 idx = read_index(t0_mode, q);
                tq = array_elem(4, idx); // GX array id 4 == TEX0
                if (tq == nullptr) {
                    ++g_2dDeclArrayMiss;
                    return false;
                }
            }
            const u8* q = tq; // shadow, as for POS
            const u32 comps = t0_elem ? 2 : 1;
            float st[2] = {0, 0};
            for (u32 ccc = 0; ccc < comps; ++ccc) {
                if (t0_fmt == 2 || t0_fmt == 3) { // u16/s16
                    st[ccc] = (float)(u16)((q[0] << 8) | q[1]) / texDiv;
                    q += 2;
                } else if (t0_fmt == 4) { // f32
                    union {
                        u32 u;
                        float f;
                    } c;
                    c.u = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
                    st[ccc] = c.f;
                    q += 4;
                } else { // u8/s8
                    st[ccc] = (float)q[0] / texDiv;
                    q += 1;
                }
            }
            for (int t = 0; t < 4; ++t) {
                g.uv[t][0] = st[0];
                g.uv[t][1] = st[1];
            }
        }
        vtx[k] = g;
    }
    // Triangulate: quads -> 2 tris each; strips/fans -> tri lists.
    if (prim == 0x80) { // QUADS
        for (u32 k = 0; k + 3 < verts; k += 4) {
            out[nOut++] = vtx[k];
            out[nOut++] = vtx[k + 1];
            out[nOut++] = vtx[k + 2];
            out[nOut++] = vtx[k];
            out[nOut++] = vtx[k + 2];
            out[nOut++] = vtx[k + 3];
        }
    } else if (prim == 0x90) { // TRIANGLES
        for (u32 k = 0; k + 2 < verts; k += 3) {
            out[nOut++] = vtx[k];
            out[nOut++] = vtx[k + 1];
            out[nOut++] = vtx[k + 2];
        }
    } else if (prim == 0x98) { // TRIANGLESTRIP
        for (u32 k = 2; k < verts; ++k) {
            out[nOut++] = vtx[k - 2];
            out[nOut++] = vtx[(k & 1) ? k : k - 1];
            out[nOut++] = vtx[(k & 1) ? k - 1 : k];
        }
    } else if (prim == 0xA8) { // LINES
        // A GX line has no area, and this frontend only knows how to hand triangles downstream, so
        // each segment becomes a quad one unit wide about its own axis. That is the right width in
        // THIS space rather than an arbitrary choice: these vertices arrive with PNMTX0 already
        // applied and an orthographic projection in force, so a unit here is a screen pixel, which
        // is exactly the width the hardware rasterises an unwidened line at.
        //
        // Measured before it was written: 22,776 of 31,356 declined ortho draws in a 300-present
        // plaza run were 0xA8, every sample 4 vertices with a real texture bound on unit 0 — two
        // textured segments, ~76 per frame. That is scene geometry, not a debug overlay, and it was
        // the single largest reason the 2D capture saw only 26% of what the game draws in 2D.
        for (u32 k = 0; k + 1 < verts; k += 2) {
            const SbrGeomVert& a = vtx[k];
            const SbrGeomVert& b = vtx[k + 1];
            float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            // A degenerate segment has no direction to offset along. Skipping it is correct — the
            // hardware draws nothing for a zero-length line either — and it must not become a
            // triangle, which would be a visible dot the game never drew.
            if (!(len > 1e-6f))
                continue;
            const float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
            SbrGeomVert a0 = a, a1 = a, b0 = b, b1 = b;
            a0.x -= nx;
            a0.y -= ny;
            a1.x += nx;
            a1.y += ny;
            b0.x -= nx;
            b0.y -= ny;
            b1.x += nx;
            b1.y += ny;
            out[nOut++] = a0;
            out[nOut++] = b0;
            out[nOut++] = b1;
            out[nOut++] = a0;
            out[nOut++] = b1;
            out[nOut++] = a1;
        }
    } else { // TRIANGLEFAN
        for (u32 k = 2; k < verts; ++k) {
            out[nOut++] = vtx[0];
            out[nOut++] = vtx[k - 1];
            out[nOut++] = vtx[k];
        }
    }
    // Triangulated to nothing. Reached when every segment of a LINES draw is degenerate, or a
    // strip/fan carries fewer vertices than one triangle needs. Counted, because the residual line
    // below caught exactly this: 14 declined draws that no reason accounted for. An uncounted
    // early return is how a breakdown quietly stops adding up.
    if (nOut == 0) {
        ++g_2dDeclEmpty;
        return false;
    }

    // STABLE identity, not a content hash: texture + vertex count + quantised screen position from
    // PNMTX0's translation. A counter digit keeps this key while its glyph changes, so the geometry
    // entry is UPDATED rather than a new one minted every frame. Content-keying minted 387k entries
    // in one run, and since g_geom is a vector whose elements are referenced by const&, that growth
    // reallocated and dangled live references — which is what collapsed the frame.
    u64 h = 1469598103934665603ULL;
    const auto mix = [&h](u64 v) { h = (h ^ v) * 1099511628211ULL; };
    mix(context.textures[0].addr);
    mix(nOut);
    mix((u64)(s32)(context.positionMatrices[3] * 4.0f));
    mix((u64)(s32)(context.positionMatrices[7] * 4.0f));
    mix((u64)(s32)(out[0].x * 64.0f));
    mix((u64)(s32)(out[0].y * 64.0f));

    SbrDrawable dr{};
    dr.streamPos = context.streamPosition;
    dr.key = h;
    dr.geom = sbr_scene_update_geometry(h, out, (int)nOut);
    if (dr.geom == 0)
        return false;
    dr.depth = *context.depth;
    for (unsigned m = 0; m < 8; ++m)
        dr.tex[m] = context.textures[m];
    dr.tev = *context.tev;
    dr.xf = *context.xf;
    for (int j = 0; j < 16; ++j)
        dr.proj[j] = context.projection[j];
    dr.is2d = 1;
    dr.mtx[0] = dr.mtx[5] = dr.mtx[10] = 1.0f; // PNMTX0 already applied per vertex
    sbr_scene_add(dr);
    if (has_texmtxidx)
        ++g_2dTexMtxIdxDraws; // counted only on a draw that WAS captured
    { // Clip-space residency of what we just decoded, per the note on g_2dInVol.
        unsigned inside = 0;
        for (u32 k = 0; k < verts; ++k) {
            const float nx2 = context.projection[0] * vtx[k].x + context.projection[3];
            const float ny2 = context.projection[5] * vtx[k].y + context.projection[7];
            if (nx2 >= -1.05f && nx2 <= 1.05f && ny2 >= -1.05f && ny2 <= 1.05f)
                ++inside;
        }
        const int cls = has_pnidx ? 1 : 0;
        float lox = vtx[0].x, hix = lox, loy = vtx[0].y, hiy = loy;
        for (u32 k = 1; k < verts; ++k) {
            lox = std::min(lox, vtx[k].x);
            hix = std::max(hix, vtx[k].x);
            loy = std::min(loy, vtx[k].y);
            hiy = std::max(hiy, vtx[k].y);
        }
        if ((hix - lox) < 1e-3f && (hiy - loy) < 1e-3f)
            ++g_2dCollapsed[cls];
        if (inside == verts)
            ++g_2dInVol[cls];
        else if (inside != 0)
            ++g_2dPartVol[cls];
        else
            ++g_2dOutVol[cls];
    }
    return true;
}

} // namespace

void gxFifo2DHandleDraw(const GxFifo2DDrawContext& context, u32 opcode, const u8* vertices,
                        u32 vertexCount, const GxFifoVat& vat) {
    if (!fifo2DEnabled()) {
        return;
    }
    ++g_2dSeen;
    if (!context.projectionIsOrthographic) {
        return;
    }
    ++g_2dOrtho;
    if (decodeDraw(context, opcode, vertices, vertexCount, vat)) {
        ++g_2dEmitted;
    }
}

// This report is independent of the native GPU renderer: it describes FIFO parsing. It prints an
// enabled-but-zero state because silence cannot distinguish an empty population from an unwired
// instrument.
void sbr_gxfifo_report_2d_gate() {
    if (!fifo2DEnabled())
        return;
    if (g_2dSeen == 0) {
        lucent::warn("gxfifo",
                     "2D gate: SBR_FIFO_2D is on but the gate examined ZERO draw primitives. That "
                     "is not 'no HUD geometry' — it is the gate never running. Either no draw "
                     "command reached the parser (check the gxfifo draw count) or the gate sits "
                     "behind a branch this configuration does not take.");
        return;
    }
    lucent::info(
        "gxfifo",
        "2D gate: {} draw(s) examined, {} under an ORTHOGRAPHIC projection, {} decoded "
        "into drawables.{}",
        g_2dSeen, g_2dOrtho, g_2dEmitted,
        g_2dOrtho == 0
            ? "   <-- ZERO under ortho while draws WERE examined: the projection signal is "
              "what is broken, not the decoder. The SDK-captured projection may not be "
              "current at FIFO-parse time."
            : (g_2dEmitted == 0
                   ? "   <-- ortho draws found but NONE decoded: the decoder declined "
                     "every one; the breakdown below says which shape it could not handle."
                   : ""));

    const unsigned long declined = g_2dOrtho - g_2dEmitted;
    if (declined == 0)
        return;
    // The BREAKDOWN, so the next piece of work is chosen by size rather than by which warning
    // happened to print first. Each line is a distinct decoder gap; together they must account for
    // every declined draw, and the residual is printed so an unaccounted remainder cannot hide.
    lucent::info("gxfifo", "  of {} ortho draw(s) DECLINED ({:.1f}% of ortho):", declined,
                 100.0 * (double)declined / (double)g_2dOrtho);
    const char* kPrimName[32] = {};
    kPrimName[0x80 >> 3] = "QUADS";
    kPrimName[0x88 >> 3] = "QUADS2";
    kPrimName[0x90 >> 3] = "TRIANGLES";
    kPrimName[0x98 >> 3] = "TRIANGLESTRIP";
    kPrimName[0xA0 >> 3] = "TRIANGLEFAN";
    kPrimName[0xA8 >> 3] = "LINES";
    kPrimName[0xB0 >> 3] = "LINESTRIP";
    kPrimName[0xB8 >> 3] = "POINTS";
    if (g_2dDeclPrim != 0) {
        lucent::Line l;
        l.add("    {:>8} unhandled primitive —", g_2dDeclPrim);
        for (unsigned i = 0; i < 32; ++i)
            if (g_2dDeclPrimOp[i] != 0)
                l.add(" 0x{:02x}{} x{}", i << 3, kPrimName[i] ? kPrimName[i] : "?",
                      g_2dDeclPrimOp[i]);
        l.flush(lucent::Level::Info, "gxfifo");
    }
    if (g_2dDeclAttr != 0) {
        static const char* kMode[4] = {"none", "direct", "index8", "index16"};
        lucent::Line l;
        l.add("    {:>8} non-direct attributes — pos:", g_2dDeclAttr);
        for (unsigned i = 0; i < 4; ++i)
            if (g_2dDeclAttrPos[i] != 0)
                l.add(" {}x{}", kMode[i], g_2dDeclAttrPos[i]);
        l.add("   tex0:");
        for (unsigned i = 0; i < 4; ++i)
            if (g_2dDeclAttrT0[i] != 0)
                l.add(" {}x{}", kMode[i], g_2dDeclAttrT0[i]);
        l.flush(lucent::Level::Info, "gxfifo");
    }
    if (g_2dDeclAttr != 0)
        lucent::info("gxfifo",
                     "             of those, {} were within 4 draws of a J3D capture "
                     "(the J3D seam ALREADY has them — decoding here would DOUBLE-COUNT) "
                     "and {} were not (geometry genuinely missing from the capture)",
                     g_2dIdxNearCapture, g_2dIdxFarFromCapture);
    if (g_2dDeclPosFmt != 0) {
        static const char* kFmt[8] = {"u8", "s8", "u16", "s16", "f32", "?", "?", "?"};
        lucent::Line l;
        l.add("    {:>8} unhandled position format —", g_2dDeclPosFmt);
        for (unsigned i = 0; i < 8; ++i)
            if (g_2dDeclPosFmtSeen[i] != 0)
                l.add(" {}x{}", kFmt[i], g_2dDeclPosFmtSeen[i]);
        l.flush(lucent::Level::Info, "gxfifo");
    }
    if (g_2dDeclMtxIdx != 0)
        lucent::info("gxfifo", "    {:>8} carried PosNrmMatIdx", g_2dDeclMtxIdx);
    if (g_2dDeclMtxUnset != 0)
        lucent::info("gxfifo",
                     "    {:>8} indexed a position matrix the stream never loaded (or an "
                     "out-of-range row) — declined rather than transformed by zeros, "
                     "which would collapse the draw onto a point and still look decoded",
                     g_2dDeclMtxUnset);
    for (int cls = 0; cls < 2; ++cls) {
        const unsigned long tot = g_2dInVol[cls] + g_2dPartVol[cls] + g_2dOutVol[cls];
        if (tot == 0) {
            lucent::info("gxfifo",
                         "  clip-space residency, {}: NO DRAWS OF THIS CLASS. The "
                         "comparison below has only one side and proves nothing.",
                         cls ? "per-vertex matrix index" : "no matrix index");
            continue;
        }
        lucent::info("gxfifo",
                     "  clip-space residency, {:<24}: {:>6} fully inside ({:.1f}%), "
                     "{:>6} straddling, {:>6} entirely outside",
                     cls ? "per-vertex matrix index" : "no matrix index", g_2dInVol[cls],
                     100.0 * (double)g_2dInVol[cls] / (double)tot, g_2dPartVol[cls],
                     g_2dOutVol[cls]);
        lucent::info("gxfifo",
                     "        of which {} had NO EXTENT (every vertex on one point). "
                     "That is the shape a wrong matrix makes, and for an orthographic "
                     "projection it lands in a corner and scores as resident, so the "
                     "percentage above cannot see it.",
                     g_2dCollapsed[cls]);
    }
    lucent::info("gxfifo",
                 "    (the two rows are each other's control: the second class is the one "
                 "the matrix work newly enabled, and a wrong matrix would put it "
                 "somewhere the first is not. It does NOT check that the geometry is the "
                 "RIGHT geometry, only that it lands on screen.)");
    // UNCONDITIONAL. This is a gap disclosure, not a decline, and printing it only when nonzero
    // would make "checked, and there are none" indistinguishable from "never looked".
    lucent::info("gxfifo",
                 "    known gap, checked: {} DECODED draw(s) carried per-vertex "
                 "TexMtxIdx. Their index bytes are consumed so the vertex stride is "
                 "right, but no texgen matrix is applied, so any such draw's UVs are the "
                 "raw attribute values and may be wrong.",
                 g_2dTexMtxIdxDraws);
    if (g_2dDeclClr != 0)
        lucent::info("gxfifo", "    {:>8} non-RGBA8 colour0", g_2dDeclClr);
    if (g_2dDeclCount != 0)
        lucent::info("gxfifo", "    {:>8} vertex count outside [3,512]", g_2dDeclCount);
    if (g_2dDeclArrayMiss != 0)
        lucent::info("gxfifo",
                     "    {:>8} indexed, but the attribute array was unregistered or the "
                     "element ran past MEM1 — declined rather than decoded from whatever "
                     "bytes were there",
                     g_2dDeclArrayMiss);
    if (g_2dDeclEmpty != 0)
        lucent::info("gxfifo",
                     "    {:>8} triangulated to nothing (all segments degenerate, or "
                     "too few vertices for one triangle)",
                     g_2dDeclEmpty);
    const long acc =
        (long)(g_2dDeclPrim + g_2dDeclAttr + g_2dDeclMtxIdx + g_2dDeclPosFmt + g_2dDeclClr +
               g_2dDeclCount + g_2dDeclEmpty + g_2dDeclArrayMiss + g_2dDeclMtxUnset);
    if ((long)declined != acc)
        lucent::warn("gxfifo",
                     "    the reasons above sum to {} but {} draws were declined — {} "
                     "are UNACCOUNTED, so this breakdown is not complete and sizing work "
                     "from it would undercount.",
                     acc, declined, (long)declined - acc);
}

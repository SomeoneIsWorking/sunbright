// dev_gxfifo.cpp — the GX write-gather pipe at 0xCC008000.
//
// Everything the game draws goes through here. GX is not a function-call API at the metal:
// the SDK's inline macros store command bytes and vertex data straight to this address, so
// the command stream — not the SDK entry points — is the only place the geometry exists.
// That is why rendering has to start with a FIFO parser rather than overriding GX functions.
//
// This first stage FRAMES the stream: it decodes the top-level opcodes and tracks the CP
// registers needed to know how long a draw's vertex payload is. Nothing is rendered yet;
// the point is to make what the game is drawing measurable, which it currently is not (3.4 M
// writes per run, all discarded).
//
// Command encoding (GameCube CP):
//   0x00           NOP
//   0x08 rr vvvvvvvv        CP register write
//   0x10 aaaa hhhh d…       XF write: (count-1)<<16 | addr, then count 32-bit words
//   0x61 rrvvvvvv           BP register write (opcode byte + 24-bit payload)
//   0x80..0xB8 | vat        draw primitive: 16-bit vertex count, then vertex data
//   0x40 aaaaaaaa ssssssss  call display list (address, size)
//   0x48                    invalidate vertex cache (no payload)
//   0x20/0x28/0x30/0x38     indexed XF load (one u32)

#include <unordered_map>
#include "mmio.h"
#include "native_render.h"
#include "scene.h"
#include "state_oracle.h"

// The texture state the material display lists have written, per texmap. See the BP handler.
static SbrTexture g_fifoTex[8];
static SbrTevState g_tev;
static SbrXfState g_xf;

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>
#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <vector>

extern u8* g_ram_base;

// Declared here rather than by including gx_texture.h: that header pulls in intrinsics.h, whose
// PPC helpers collide with aurora's ppc_math.h in this translation unit.
bool gx_decode_texture(u32 addr, uint32_t w, uint32_t h, uint32_t format, uint32_t tlutAddr,
                       uint8_t* out);

namespace {

constexpr u32 FIFO_BASE = 0xCC008000;

// The stream arrives in pieces of 1/2/4/8 bytes; commands straddle those writes, so it has
// to be reassembled before it can be framed.
std::vector<u8> g_buf;

// The stream handed to aurora. Identical to the guest's, except that CP array-base writes
// are rewritten (see below) — aurora cannot use the guest's 32-bit address directly.
std::vector<u8> g_out;
unsigned long g_xfbCopies = 0;   // copy-to-XFB triggers seen in the stream (BP 0x52 bit 14)
std::vector<u8> g_last;   // the stream of the frame just presented (see gxfifo_last_frame)



// Per-VAT vertex descriptors, enough to compute a vertex's byte size.
struct Vat { u32 vcd_lo = 0, vcd_hi = 0, fmt0 = 0, fmt1 = 0, fmt2 = 0; };
Vat g_vat[8];
u32 g_vcd_lo = 0, g_vcd_hi = 0;

// EFB copy rectangle, tracked from BP 0x49/0x4A so a copy can be described to aurora in its
// own terms (see emit_copy_state).
u32 g_copy_left = 0, g_copy_top = 0, g_copy_w = 0, g_copy_h = 0;

// BP 0x4B: destination address of a copy, in 32-byte units. Only meaningful for a copy to a
// TEXTURE — a display copy goes to the XFB, which aurora keys separately. Without this the
// texture-copy destination is never described and aurora resolves into whatever pointer it
// last held.
u32 g_copy_dest = 0;

// BP 0x4E: vertical copy scale, 1.8 fixed point (0x100 == 1:1). Together with the horizontal
// half-scale bit of the copy command this is how a full EFB is resolved down into a smaller
// texture — the sea reflection copies 640x448 of EFB into a 320x224 texture this way.
u32 g_copy_yscale = 0x100;

// Per-texmap image registers. image0 carries dimensions and format; image3 carries the
// texel address (in 32-byte units). Aurora records both but takes the actual texel POINTER
// from a GXTexObj supplied through its own extension — the same split as vertex array bases,
// and for the same reason: a raw 32-bit value cannot be a host pointer.
struct TexSlot {
    u32 image0 = 0, image3 = 0;
    bool have0 = false, have3 = false;
    // Last values actually sent. The game rewrites these registers on every material bind,
    // so emitting unconditionally floods aurora with redundant texture loads — measured as a
    // severe slowdown (retrace 240 -> 180 over a much longer run).
    u32 sent0 = 0xFFFFFFFF, sent3 = 0xFFFFFFFF;
};
TexSlot g_tex[8];

struct Stats {
    u64 bytes = 0, nops = 0, cp = 0, xf = 0, bp = 0, draws = 0, verts = 0, unknown = 0;
} g_stats;

unsigned long g_bpWrites[256] = {};
long g_drawIndex = 0;
// Draw commands seen in the stream since the last query. The scene frontend captures only
// J3DShape::draw, so this is the ceiling a FIFO-vertex frontend would reach — the difference
// between the two is the coverage gap, in draws, rather than a guess about it.
long g_drawsSinceQuery = 0;
// Draw commands since the last J3DShape::draw capture. Reset by the capture seam, read at frame
// end: the residual is the block of draws that NO captured drawable accounts for. The HUD is drawn
// after the 3D scene, so this isolates the unattributed population instead of bounding it — the
// ~33x draws-per-drawable ratio could not, since one drawable legitimately expands to many draws.
long g_drawsSinceCapture = 0;
long g_maxDrawsAfterCapture = 0;
// Where each texture unit was last bound, and what it held before — the provenance a divergence
// report needs. Kept beside the unit state so it cannot drift from it.
uint32_t g_fifoTexBindPos[8] = {};
uint32_t g_scisRaw[2] = {};
// Raster state as the COMMAND STREAM describes it (BP 0x40/0x41), which is where J3D actually puts
// it. Kept separate from the SDK-captured copy until the renderer is switched over deliberately.
SbrDepthState g_fifoZ{1, 3, 1, 0, 4, 5, 1, 1, 0, {0, 0, 640, 448}};

// PNMTX0 as the stream last loaded it (XF addr 0x0000, 12 floats row-major 3x4). J2D positions its
// quads with GXLoadPosMtxImm into PNMTX0 and IDENTITY projection-side transforms beyond it, so this
// matrix is the whole placement of a 2D draw. Latched from the same XF write the hardware sees.
float g_pnmtx0[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

// The projection AS THE STREAM SETS IT (XF 0x1020: 6 params then the type word). This must come from
// the FIFO, not from the SDK-captured projection: the SDK copy is not synchronised with the stream
// position being parsed, so gating 2D capture on it classified 3D draws — indexed attributes, s8
// positions — as orthographic and flooded the scene with hundreds of thousands of bogus drawables,
// blanking the frame. That is the THIRD instance of the SDK-vs-FIFO trap in this renderer, after the
// raster state (wrong on 43% of draws) and the texObj slot (133 phantom mismatches).
float g_fifoProj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
bool  g_fifoProjOrtho = false;

// SBR_FIFO_2D=1 — capture 2D geometry from the FIFO's own direct-mode draws (Fable-reviewed
// design, 2026-07-30). The J2D/HUD reaches GX through at least five paths (tree-walked pictures,
// the 7-arg J2DPicture::draw, raw-GX gauge draws, per-glyph text, screen fills); synthesising
// quads from pane objects can never cover the raw-GX and per-glyph paths, while every one of them
// serialises its geometry HERE exactly. Gated on the projection being orthographic at the draw.
// Gate telemetry: how many draws the 2D gate examined, how many were under an ortho projection,
// and how many decoded. If ortho stays 0 while the HUD draws exist, the PROJECTION SIGNAL is what
// is broken — the SDK-captured projection may not be current at FIFO parse time.
unsigned long g_2dSeen = 0, g_2dOrtho = 0, g_2dEmitted = 0;

bool fifo2d_on() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_FIFO_2D");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}
uint32_t g_fifoTexPrevAddr[8] = {};


// BP register shadow + the one-shot write mask (register 0xFE). See the BP handler.
u32 g_bpCache[256] = {};
u32 g_bpMask = 0x00FFFFFFu;

// Monotonic bind counter — see SbrTexture::bindSeq.
u32 g_bindSeq = 0;

// Attribute presence is 2 bits per attribute: 0 none, 1 direct, 2 index8, 3 index16.
u32 attr_size(u32 mode, u32 direct_size) {
    switch (mode) {
    case 0: return 0;
    case 1: return direct_size;
    case 2: return 1;
    case 3: return 2;
    default: return 0;
    }
}

u32 component_bytes(u32 fmt) {
    switch (fmt) {   // 0 u8, 1 s8, 2 u16, 3 s16, 4 f32
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    case 4: return 4;
    default: return 0;
    }
}

// Direct colour size by CP component format, not by assumption:
//   0 RGB565 (2)  1 RGB8 (3)  2 RGBX8 (4)  3 RGBA4 (2)  4 RGBA6 (3)  5 RGBA8 (4)
u32 colour_bytes(u32 comp) {
    switch (comp) {
    case 0: return 2;
    case 1: return 3;
    case 2: return 4;
    case 3: return 2;
    case 4: return 3;
    case 5: return 4;
    default: return 0;
    }
}

// VAT_A packs texcoord 0; VAT_B packs 1-4 and VAT_C packs 5-7, 9 bits each
// (1 bit element count, 3 bits format, 5 bits fractional shift).
u32 texcoord_bytes(const Vat& v, int i) {
    u32 elem, fmt;
    if (i == 0) { elem = (v.fmt0 >> 21) & 1; fmt = (v.fmt0 >> 22) & 7; }
    else if (i <= 4) {
        const u32 sh = (u32)(i - 1) * 9;
        elem = (v.fmt1 >> sh) & 1; fmt = (v.fmt1 >> (sh + 1)) & 7;
    } else {
        // VAT_C starts with TEX4's 5-bit frac, so TEX5 begins at bit 5 — not bit 0.
        // Getting this wrong made texcoord-heavy vertices the wrong size and desynced the
        // stream handed to aurora (its "unsupported primitive type 136", intermittently).
        const u32 sh = 5 + (u32)(i - 5) * 9;
        elem = (v.fmt2 >> sh) & 1; fmt = (v.fmt2 >> (sh + 1)) & 7;
    }
    return (elem ? 2u : 1u) * component_bytes(fmt);
}

// Size of one vertex under the current VCD/VAT. Indexed attributes are 1 or 2 bytes
// regardless of format; only direct attributes need the format decoded.
// Decode one direct-mode 2D draw into scene vertices and hand it to the renderer. Supports the
// attribute formats the 2D paths actually use (setup2D: POS s16 xyz shift 0 / CLR0 RGBA8 / TEX0
// u16 shift 15; text and gauges add f32 and u8 variants) and FAILS LOUD on anything else — a
// silently mis-decoded vertex places geometry plausibly-but-wrongly, which is this project's
// most-repeated failure mode. Indexed attributes are declined loudly too: J2D never uses them,
// so their appearance means the gate caught a draw it was never meant to capture.
bool decode_2d_draw(u32 op, const u8* vp, u32 verts, const Vat& v);

u32 vertex_size(u32 vat_idx) {
    const Vat& v = g_vat[vat_idx & 7];
    u32 n = 0;

    // VCD lo: bit0 PosNrmMatIdx, bits1-8 TexMtxIdx0-7 (1 byte each when set),
    // bits9-10 Position, 11-12 Normal, 13-14 Color0, 15-16 Color1.
    if (v.vcd_lo & 1) n += 1;
    for (int i = 0; i < 8; i++) if (v.vcd_lo & (1u << (1 + i))) n += 1;

    const u32 pos_mode = (v.vcd_lo >> 9) & 3;
    const u32 nrm_mode = (v.vcd_lo >> 11) & 3;
    const u32 c0_mode  = (v.vcd_lo >> 13) & 3;
    const u32 c1_mode  = (v.vcd_lo >> 15) & 3;

    // VAT_A: pos elements bit0 / format bits1-3.
    const u32 pos_cnt = ((v.fmt0 >> 0) & 1) ? 3 : 2;
    n += attr_size(pos_mode, pos_cnt * component_bytes((v.fmt0 >> 1) & 7));

    // Normals carry a GC quirk that must match aurora's calculate_last_vtx_size exactly:
    // bit 9 selects NBT (normal+binormal+tangent, 9 components instead of 3) and bit 31
    // selects NBT3, where an INDEXED normal costs THREE indices rather than one. Ignoring
    // this made every J3D lit model's vertices the wrong length, so the bytes copied for a
    // draw were wrong and the stream handed to aurora desynced — surfacing as aurora's
    // "unsupported primitive type 136" (0x88 is not a primitive at all; its own comments
    // note that a desync shows up exactly this way).
    const bool nbt3 = ((v.fmt0 >> 31) & 1) != 0;
    const bool nbt  = nbt3 || ((v.fmt0 >> 9) & 1) != 0;
    const u32 nrm_comps = nbt ? 9u : 3u;
    if (nrm_mode == 1)      n += nrm_comps * component_bytes((v.fmt0 >> 10) & 7);
    else if (nrm_mode == 2) n += nbt3 ? 3u : 1u;
    else if (nrm_mode == 3) n += nbt3 ? 6u : 2u;

    // VAT_A: col0 elements bit13 / comp bits14-16, col1 elements bit17 / comp bits18-20.
    n += attr_size(c0_mode, colour_bytes((v.fmt0 >> 14) & 7));
    n += attr_size(c1_mode, colour_bytes((v.fmt0 >> 18) & 7));

    // VCD hi: bits0-15 are TexCoord0-7 modes, 2 bits each.
    for (int i = 0; i < 8; i++)
        n += attr_size((v.vcd_hi >> (i * 2)) & 3, texcoord_bytes(v, i));
    return n;
}

bool decode_2d_draw(u32 op, const u8* vp, u32 verts, const Vat& v) {
    const u32 prim = op & 0xF8;
    // The 2D paths draw QUADS (J2DPicture, glyphs, gauge) and the occasional strip/fan for fills.
    if (prim != 0x80 && prim != 0x90 && prim != 0x98 && prim != 0xA0) {
        static bool w1 = false;
        if (!w1) { w1 = true; lucent::warn("fifo2d", "declined: primitive 0x{:02x} not handled", prim); }
        return false;
    }
    const u32 pos_mode = (v.vcd_lo >> 9) & 3;
    const u32 c0_mode  = (v.vcd_lo >> 13) & 3;
    const u32 t0_mode  = v.vcd_hi & 3;
    if (pos_mode != 1 || (c0_mode != 0 && c0_mode != 1) || (t0_mode != 0 && t0_mode != 1)) {
        static bool w2 = false;
        if (!w2) { w2 = true; lucent::warn("fifo2d", "declined: non-direct attrs pos={} c0={} t0={}", pos_mode, c0_mode, t0_mode); }
        return false;
    }
    if (v.vcd_lo & 1) {   // PosNrmMatIdx: J2D never uses it; capturing such a draw is a gate bug
        static bool w3 = false;
        if (!w3) { w3 = true; lucent::warn("fifo2d", "declined: PosNrmMatIdx present"); }
        return false;
    }
    const u32 pos_cnt = ((v.fmt0 >> 0) & 1) ? 3 : 2;
    const u32 pos_fmt = (v.fmt0 >> 1) & 7;
    const u32 pos_shift = (v.fmt0 >> 4) & 0x1F;
    const u32 c0_comp = (v.fmt0 >> 14) & 7;
    const u32 t0_elem = (v.fmt0 >> 21) & 1;
    const u32 t0_fmt  = (v.fmt0 >> 22) & 7;
    const u32 t0_shift = (v.fmt0 >> 25) & 0x1F;
    if (pos_fmt != 3 && pos_fmt != 4) {   // s16 / f32 are what 2D actually emits
        static bool w4 = false;
        if (!w4) { w4 = true; lucent::warn("fifo2d", "declined: pos fmt {}", pos_fmt); }
        return false;
    }
    if (c0_mode == 1 && c0_comp != 5) {   // RGBA8 only (setup2D's declared format)
        static bool w5 = false;
        if (!w5) { w5 = true; lucent::warn("fifo2d", "declined: clr0 comp {}", c0_comp); }
        return false;
    }
    if (verts < 3 || verts > 512) return false;

    const u32 vsize = vertex_size(0xFF & (op & 7));
    const float posDiv = (pos_fmt == 3) ? (float)(1u << pos_shift) : 1.0f;
    const float texDiv = (t0_fmt == 2 || t0_fmt == 3) ? (float)(1u << t0_shift) : 1.0f;

    // STATIC, not stack. These are ~117KB and ~39KB; as locals they put ~160KB on the stack of a
    // FIFO-parse callback, which is a stack overflow waiting to happen and did in fact coincide with
    // the whole frame collapsing to the clear colour. The parser is single-threaded by construction
    // (the one-runtime doctrine), so static is safe here and the sizes stay off the stack.
    static SbrGeomVert out[512 * 3];
    static SbrGeomVert vtx[512];
    u32 nOut = 0;
    for (u32 k = 0; k < verts; ++k) {
        const u8* q = vp + (size_t)k * vsize;
        SbrGeomVert g{};
        // POS
        float px, py, pz = 0.0f;
        if (pos_fmt == 3) {
            px = (float)(s16)((q[0] << 8) | q[1]) / posDiv;
            py = (float)(s16)((q[2] << 8) | q[3]) / posDiv;
            if (pos_cnt == 3) pz = (float)(s16)((q[4] << 8) | q[5]) / posDiv;
            q += pos_cnt * 2;
        } else {
            union { u32 u; float f; } c;
            c.u = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3]; px = c.f;
            c.u = ((u32)q[4] << 24) | ((u32)q[5] << 16) | ((u32)q[6] << 8) | q[7]; py = c.f;
            if (pos_cnt == 3) {
                c.u = ((u32)q[8] << 24) | ((u32)q[9] << 16) | ((u32)q[10] << 8) | q[11];
                pz = c.f;
            }
            q += pos_cnt * 4;
        }
        // PNMTX0: the 2D position matrix (usually identity or a screen translation).
        g.x = g_pnmtx0[0] * px + g_pnmtx0[1] * py + g_pnmtx0[2] * pz + g_pnmtx0[3];
        g.y = g_pnmtx0[4] * px + g_pnmtx0[5] * py + g_pnmtx0[6] * pz + g_pnmtx0[7];
        g.z = g_pnmtx0[8] * px + g_pnmtx0[9] * py + g_pnmtx0[10] * pz + g_pnmtx0[11];
        g.nz = 1.0f;
        // CLR0
        if (c0_mode == 1) {
            g.rgba = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
            q += 4;
        } else {
            g.rgba = 0xFFFFFFFFu;
        }
        // TEX0
        if (t0_mode == 1) {
            const u32 comps = t0_elem ? 2 : 1;
            float st[2] = {0, 0};
            for (u32 ccc = 0; ccc < comps; ++ccc) {
                if (t0_fmt == 2 || t0_fmt == 3) {   // u16/s16
                    st[ccc] = (float)(u16)((q[0] << 8) | q[1]) / texDiv;
                    q += 2;
                } else if (t0_fmt == 4) {           // f32
                    union { u32 u; float f; } c;
                    c.u = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
                    st[ccc] = c.f;
                    q += 4;
                } else {                            // u8/s8
                    st[ccc] = (float)q[0] / texDiv;
                    q += 1;
                }
            }
            for (int t = 0; t < 4; ++t) { g.uv[t][0] = st[0]; g.uv[t][1] = st[1]; }
        }
        vtx[k] = g;
    }
    // Triangulate: quads -> 2 tris each; strips/fans -> tri lists.
    if (prim == 0x80) {                             // QUADS
        for (u32 k = 0; k + 3 < verts; k += 4) {
            out[nOut++] = vtx[k]; out[nOut++] = vtx[k + 1]; out[nOut++] = vtx[k + 2];
            out[nOut++] = vtx[k]; out[nOut++] = vtx[k + 2]; out[nOut++] = vtx[k + 3];
        }
    } else if (prim == 0x90) {                      // TRIANGLES
        for (u32 k = 0; k + 2 < verts; k += 3) {
            out[nOut++] = vtx[k]; out[nOut++] = vtx[k + 1]; out[nOut++] = vtx[k + 2];
        }
    } else if (prim == 0x98) {                      // TRIANGLESTRIP
        for (u32 k = 2; k < verts; ++k) {
            out[nOut++] = vtx[k - 2]; out[nOut++] = vtx[(k & 1) ? k : k - 1];
            out[nOut++] = vtx[(k & 1) ? k - 1 : k];
        }
    } else {                                        // TRIANGLEFAN
        for (u32 k = 2; k < verts; ++k) {
            out[nOut++] = vtx[0]; out[nOut++] = vtx[k - 1]; out[nOut++] = vtx[k];
        }
    }
    if (nOut == 0) return false;

    // STABLE identity, not a content hash: texture + vertex count + quantised screen position from
    // PNMTX0's translation. A counter digit keeps this key while its glyph changes, so the geometry
    // entry is UPDATED rather than a new one minted every frame. Content-keying minted 387k entries
    // in one run, and since g_geom is a vector whose elements are referenced by const&, that growth
    // reallocated and dangled live references — which is what collapsed the frame.
    u64 h = 1469598103934665603ULL;
    const auto mix = [&h](u64 v) { h = (h ^ v) * 1099511628211ULL; };
    mix(g_fifoTex[0].addr);
    mix(nOut);
    mix((u64)(s32)(g_pnmtx0[3] * 4.0f));
    mix((u64)(s32)(g_pnmtx0[7] * 4.0f));
    mix((u64)(s32)(out[0].x * 64.0f));
    mix((u64)(s32)(out[0].y * 64.0f));

    SbrDrawable dr{};
    dr.streamPos = (u32)g_out.size();
    dr.key = h;
    dr.geom = sbr_scene_update_geometry(h, out, (int)nOut);
    if (dr.geom == 0) return false;
    dr.depth = g_fifoZ;
    for (unsigned m = 0; m < 8; ++m) dr.tex[m] = g_fifoTex[m];
    dr.tev = g_tev;
    dr.xf  = g_xf;
    for (int j = 0; j < 16; ++j) dr.proj[j] = g_fifoProj[j];
    dr.is2d = 1;
    dr.mtx[0] = dr.mtx[5] = dr.mtx[10] = 1.0f;   // PNMTX0 already applied per vertex
    sbr_scene_add(dr);
    return true;
}

void put_u8 (std::vector<u8>& v, u8 x)  { v.push_back(x); }
void put_u16(std::vector<u8>& v, u16 x) { v.push_back((u8)(x >> 8)); v.push_back((u8)x); }
void put_u32(std::vector<u8>& v, u32 x) { for (int i = 3; i >= 0; i--) v.push_back((u8)(x >> (i * 8))); }
void put_u64(std::vector<u8>& v, u64 x) { for (int i = 7; i >= 0; i--) v.push_back((u8)(x >> (i * 8))); }

// GX_AURORA (0x50) + subcommand, then a 64-bit host pointer, 32-bit size, 1-byte LE flag.
void emit_arraybase(u32 attr, u32 guest_addr) {
    const u32 off = guest_addr & 0x01FFFFFFu;
    if (off >= 0x01800000u) {
        lucent::debug("gxfifo", "array base 0x{:08x} is outside MEM1 — dropped", guest_addr);
        return;
    }
    put_u8 (g_out, 0x50);
    put_u16(g_out, (u16)(GX_AURORA_LOAD_ARRAYBASE + attr));
    put_u64(g_out, (u64)(uintptr_t)(g_ram_base + off));
    // Size 0 is aurora's "trust" registration: it uploads the AUTO-DERIVED extent, i.e. up
    // to the highest index actually referenced (tracked in draw_prim), instead of a fixed
    // span. Passing a real byte count makes it upload exactly that many bytes —
    // `0x01800000 - off` (up to 24 MB, "the array cannot run past MEM1") was technically
    // true and catastrophic: every indexed array pushed megabytes into the 48 MB storage
    // buffer, which is what overflowed it.
    put_u32(g_out, 0);
    put_u8 (g_out, 0);                   // big-endian, as the guest wrote it
}

// Aurora's CP handles the copy-to-XFB trigger (BP 0x52 bit 14), but only after the copy
// source and destination have been described through its own extensions — the raw BP
// registers carry EFB coordinates and a guest destination address it cannot use directly.
// Without this its present source is never created, and the frame it hands back is 1x1.
// `cmd` is the BP 0x52 payload; `to_xfb` is its bit 14, already decoded by the caller.
//
// BOTH kinds of copy must be described, not just the display copy. A copy to a texture is how
// the game builds render-to-texture content — at file-select the sea's reflection is a 640x448
// EFB region resolved into a 320x224 texture. Describing only the display copy left aurora
// resolving texture copies against the previous display copy's extent and format, and into
// whatever destination pointer it last held, since GX_AURORA_LOAD_COPY_DEST was never sent.
void emit_copy_state(u32 cmd, bool to_xfb) {
    if (g_copy_w == 0 || g_copy_h == 0) return;
    // Every distinct TEXTURE copy destination, once. A render-to-texture target that this port
    // never writes back into guest memory decodes as ZEROS, which is indistinguishable from a
    // legitimately black texture at the sampler — so the set of copy destinations is the list of
    // addresses whose blackness is expected rather than a decode bug.
    if (!to_xfb) {
        static std::unordered_map<u32, bool> seen;
        const u32 a = (g_copy_dest << 5) | 0x80000000u;
        if (!seen[a]) {
            seen[a] = true;
            lucent::info("gxfifo", "EFB texture copy -> 0x{:08x} ({}x{})", a, g_copy_w, g_copy_h);
        }
    }

    // The copy scales the EFB region down into the destination: bit 9 halves horizontally,
    // and BP 0x4E scales vertically as 1.8 fixed point.
    const bool half_scale   = (cmd >> 9) & 1;
    const bool scale_invert = (cmd >> 10) & 1;
    u32 dst_w = half_scale ? g_copy_w / 2 : g_copy_w;
    u32 dst_h;
    if (to_xfb) {
        // A display copy scales height by BP 0x4E, 1.8 fixed point (inverted by bit 10).
        const double ys = scale_invert ? 256.0 / (double)(g_copy_yscale ? g_copy_yscale : 0x100)
                                       : (double)g_copy_yscale / 256.0;
        dst_h = (u32)((double)g_copy_h * ys);
    } else {
        // A texture copy has no separate vertical scale: the half-scale bit halves BOTH
        // dimensions. Confirmed against this game rather than assumed — the sea reflection
        // copies a 640x448 EFB region with half-scale set, and the texture it then binds is
        // 320x224. Treating 0x4E as the height scale here produced 320x448, which no texture
        // in the scene matches.
        dst_h = half_scale ? g_copy_h / 2 : g_copy_h;
    }
    if (dst_w == 0) dst_w = 1;
    if (dst_h == 0) dst_h = 1;
    // Hand a TEXTURE copy to the renderer so it can resolve the EFB region into a real GPU texture
    // registered under the destination address. A display (XFB) copy is the presented frame and has
    // no texture consumer, so it is not forwarded.
    if (!to_xfb && g_copy_dest != 0)
        sbr_scene_note_efb_copy((g_copy_dest << 5) | 0x80000000u, (int)g_copy_left, (int)g_copy_top,
                                (int)g_copy_w, (int)g_copy_h, (int)dst_w, (int)dst_h);

    // The copy format is NOT the raw bits 3-6. Hardware packs it so that the low bit selects
    // the upper half of the format space:  fmt = field/2 + (field & 1) * 8.  Verified against
    // this game's own copies: field 8 decodes to RGB565, and the sea reflection texture the
    // game then binds is indeed RGB565 (format 4); field 10 decodes to RGB5A3. Reading the
    // field directly instead gave the nonsensical "R8"/"B8" single-channel formats.
    //
    // For 0-6 the result coincides with the GXTexFmt numbering aurora wants (I4, I8, IA4, IA8,
    // RGB565, RGB5A3, RGBA8).
    const u32 field = (cmd >> 3) & 0xF;
    u32 fmt = field / 2 + (field & 1) * 8;

    // A display copy goes through the XFB, where the format field is not the texture format —
    // the copy is YUV-converted and aurora wants the XFB as RGBA8. Decoding it as a texture
    // format yields GX_TF_I4, 4-bit intensity, which renders the whole frame greyscale.
    if (to_xfb) {
        fmt = 6;    // GX_TF_RGBA8
    } else if (fmt == 8) {
        // Copy format R8 (Dolphin EFBCopyFormat 8): a single 8-bit channel written in the same
        // 8x4 tiled 8-bpp layout as a GX I8 texture, and the game binds it AS I8. Translating it to
        // RGBA8 both quadrupled the byte size (so the sampler walked the wrong stride) and dropped
        // the intensity into the red channel only — the heat-haze distortion source samples this,
        // so it showed a stale/garbled capture (the "haze ghosting"). I8 is the faithful mapping.
        fmt = 1;   // GX_TF_I8
    } else if (fmt > 6 && fmt != 14) {
        // The other single/dual-channel copy formats (A8, G8, B8, RG8, GB8) have no GXTexFmt
        // equivalent and nothing observed uses them; say so rather than resolve to a wrong format.
        lucent::warn("gxfifo", "EFB copy format {} (field {}, BP 0x52 = 0x{:06x}) has no "
                               "GXTexFmt equivalent — this single/dual-channel copy format is not "
                               "translated yet", fmt, field, cmd);
        fmt = 6;
    }

    {   // One line per distinct copy shape, so the copy set of a scene is visible at a glance.
        static u32 last_w = 0, last_h = 0, last_fmt = 0xFF; static bool last_xfb = false;
        if (dst_w != last_w || dst_h != last_h || fmt != last_fmt || to_xfb != last_xfb) {
            last_w = dst_w; last_h = dst_h; last_fmt = fmt; last_xfb = to_xfb;
            lucent::debug("gxfifo", "EFB copy -> {} : src {}x{} at ({},{}) -> dst {}x{} fmt {}",
                          to_xfb ? "XFB" : "texture", g_copy_w, g_copy_h, g_copy_left,
                          g_copy_top, dst_w, dst_h, fmt);
            lucent::debug("gxfifo", "  copy scale: yscale=0x{:x} half={} -> dst {}x{}",
                          g_copy_yscale, half_scale ? 1 : 0, dst_w, dst_h);
        }
    }

    put_u8 (g_out, 0x50);
    put_u16(g_out, GX_AURORA_LOAD_COPY_SRC);
    put_u32(g_out, g_copy_left);
    put_u32(g_out, g_copy_top);
    put_u32(g_out, g_copy_w);
    put_u32(g_out, g_copy_h);

    put_u8 (g_out, 0x50);
    put_u16(g_out, GX_AURORA_LOAD_COPY_DST);
    put_u32(g_out, dst_w);
    put_u32(g_out, dst_h);
    put_u32(g_out, fmt);
    put_u8 (g_out, 0);      // not a wide (double-strided) copy

    // The display copy is keyed on aurora's own kDisplayCopyDest and needs no destination.
    // A texture copy does: without it aurora resolves into a stale pointer.
    if (!to_xfb) {
        const u32 phys = g_copy_dest << 5;
        if (phys == 0 || phys >= 0x01800000u) {
            lucent::error("gxfifo", "EFB texture copy destination 0x{:08x} (BP 0x4B = "
                                    "0x{:06x}) is not in MEM1 — the copy has nowhere to land",
                          phys, g_copy_dest);
            std::abort();
        }
        {   // Aurora resolves a sampled texture to an EFB-copy result by looking the texobj's
            // texel POINTER up in its copy-texture map, which is keyed by this destination.
            // If the two disagree the copy still happens and the bind still happens, but the
            // draw samples raw MEM1 instead of the copy — silently, and looking exactly like
            // a broken texture.
            static u32 last = 0;
            if (phys != last) {
                last = phys;
                lucent::debug("gxfifo", "EFB copy destination phys 0x{:08x} ({}x{})", phys,
                              dst_w, dst_h);
            }
        }
        put_u8 (g_out, 0x50);
        put_u16(g_out, GX_AURORA_LOAD_COPY_DEST);
        put_u64(g_out, (u64)(uintptr_t)(g_ram_base + phys));
    }
}

// GX_AURORA_LOAD_TEXOBJ payload, from aurora's parser: u8 map, u64 data, u32 w, u32 h,
// u32 fmt, u32 tlut, u8 hasMips, u32 texObjId, u32 texDataVersion.
void emit_texobj(u32 map) {
    // SBR_NO_TEXOBJ=1 (diagnostic): stop describing textures to aurora, to bisect whether a
    // staging overflow comes from texture uploads or from geometry.
    static const bool disabled = std::getenv("SBR_NO_TEXOBJ") != nullptr;
    if (disabled) return;

    TexSlot& t = g_tex[map & 7];
    if (!t.have0 || !t.have3) return;
    if (t.image0 == t.sent0 && t.image3 == t.sent3) return;   // unchanged bind
    t.sent0 = t.image0; t.sent3 = t.image3;

    const u32 w   = (t.image0 & 0x3FF) + 1;
    const u32 h   = ((t.image0 >> 10) & 0x3FF) + 1;
    const u32 fmt = (t.image0 >> 20) & 0xF;
    const u32 phys = (t.image3 & 0x00FFFFFFu) << 5;   // image3 is in 32-byte units
    if (phys >= 0x01800000u) {
        lucent::debug("gxfifo", "texture {} address 0x{:08x} is outside MEM1", map, phys);
        return;
    }


    {   // Counterpart of the copy-destination log above: the pointer a bind presents to
        // aurora must equal the destination a copy registered, or the copy is never sampled.
        if (w == 320 && h == 224) {
            static u32 last = 0;
            if (phys != last) {
                last = phys;
                lucent::debug("gxfifo", "bind of a 320x224 texture: phys 0x{:08x} fmt {}", phys,
                              fmt);
            }
        }
    }
    // GX loads texels into TMEM when a unit is BOUND; the main-memory buffer is the game's to
    // reuse afterwards. Aurora models that by uploading here, at the bind. This port decodes
    // lazily at frame end instead, so a buffer the game has since reused decodes to whatever is
    // there NOW. Measure the difference rather than assume it: decode at the bind and report the
    // brightness, so it can be compared with the late decode of the same address.
    if (std::getenv("SBR_BIND_DECODE_LOG") != nullptr) {
        const u32 gaddr = phys | 0x80000000u;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        if (gx_decode_texture(gaddr, w, h, fmt, 0, rgba.data())) {
            uint64_t sum = 0;
            for (size_t i = 0; i < rgba.size(); i += 4) sum += rgba[i] + rgba[i+1] + rgba[i+2];
            static std::unordered_map<u32, bool> seen;
            if (!seen[gaddr]) {
                seen[gaddr] = true;
                lucent::info("gxfifo", "AT BIND 0x{:08x} {}x{} fmt{} decodes to mean {:.1f}",
                             gaddr, w, h, fmt, (double)sum / (double)(rgba.size() / 4 * 3));
            }
        }
    }
    put_u8 (g_out, 0x50);
    put_u16(g_out, (u16)GX_AURORA_LOAD_TEXOBJ);
    put_u8 (g_out, (u8)(map & 7));
    put_u64(g_out, (u64)(uintptr_t)(g_ram_base + phys));
    put_u32(g_out, w);
    put_u32(g_out, h);
    put_u32(g_out, fmt);
    {   // Which texture formats are we describing? A scene rendering entirely in greys
        // would be explained by everything decoding as an intensity format.
        static u32 seen = 0;
        if (!(seen & (1u << fmt))) {
            seen |= 1u << fmt;
            lucent::debug("gxfifo", "texture format {} first seen ({}x{})", fmt, w, h);
        }
    }
    put_u32(g_out, 0);           // TLUT index; palettised formats need LOAD_TLUT too
    put_u8 (g_out, 0);           // mips are gated by the real TexMode1 register, not here
    // texObjId is aurora's texture cache key — a zero id makes every bind a cache miss and
    // re-upload (the known 33x perf cliff). The texel address is stable and unique per
    // texture, so it serves as the identity.
    put_u32(g_out, phys ? phys : 1u);
    put_u32(g_out, 0);           // data version: bumped only when texel bytes change
}

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u32 be16(const u8* p) { return (u32)p[0] << 8 | p[1]; }

// Frame as much of the buffer as is complete. Returns bytes consumed.
size_t parse(const u8* p, size_t n, int depth = 0);

// Bytes the next incomplete command needs before parsing can make progress again.
size_t g_need = 0;

// Per-frame display-list expansion tally.
u64 g_dl_calls = 0, g_dl_bytes = 0;

// A display list lives in guest memory and is executed inline by the CP. Aurora cannot
// follow the guest pointer (it ignores raw 32-bit addresses for the same reason it ignores
// raw array bases), so the contents are parsed and emitted into the flat output stream
// instead — aurora sees the commands, not a pointer to them.
//
// This matters more than it looks: J3D bakes per-shape geometry into display lists, so
// without this almost all real drawing is invisible to the parser (it showed up as
// "unrecognised opcode 0x48 — framing lost", which discarded the rest of the batch).
void inline_display_list(u32 guest_addr, u32 size, int depth) {
    if (depth > 4) {
        lucent::debug("gxfifo", "display-list nesting deeper than 4 — not following");
        return;
    }
    const u32 off = guest_addr & 0x01FFFFFFu;
    if (off + size > 0x01800000u || size == 0) {
        lucent::debug("gxfifo", "display list 0x{:08x} +0x{:x} is outside MEM1", guest_addr,
                      size);
        return;
    }
    // g_need describes the OUTER stream's incomplete command; a nested list parses a
    // complete buffer and must not disturb it.
    g_dl_calls++; g_dl_bytes += size;
    const size_t saved_need = g_need;
    parse(g_ram_base + off, size, depth + 1);
    g_need = saved_need;
}

size_t parse(const u8* p, size_t n, int depth) {
    size_t i = 0;
    while (i < n) {
        const u8 op = p[i];

        if (op == 0x00) { g_out.insert(g_out.end(), p + i, p + i + 1);
            g_stats.nops++; i += 1; continue; }

        if (op == 0x08) {                       // CP register write
            if (n - i < 6) { g_need = 6; break; }
            const u8  reg = p[i + 1];
            const u32 val = be32(p + i + 2);
            if (reg == 0x50) { g_vcd_lo = val; for (auto& v : g_vat) v.vcd_lo = val; }
            else if (reg == 0x60) { g_vcd_hi = val; for (auto& v : g_vat) v.vcd_hi = val; }
            else if (reg >= 0x70 && reg <= 0x77) g_vat[reg - 0x70].fmt0 = val;
            else if (reg >= 0x80 && reg <= 0x87) g_vat[reg - 0x80].fmt1 = val;
            else if (reg >= 0x90 && reg <= 0x97) g_vat[reg - 0x90].fmt2 = val;

            if (reg >= 0xA0 && reg <= 0xAF) {
                // Vertex array base. Aurora IGNORES the raw CP write, because in the decomp
                // world these 32 bits are a truncated HOST pointer. Here they are a genuine
                // GUEST address into memory we own, so resolve it and hand aurora the real
                // pointer through its own extension (GX_AURORA, opcode 0x50).
                emit_arraybase(reg - 0xA0, val);
            } else {
                g_out.insert(g_out.end(), p + i, p + i + 6);
            }
            g_stats.cp++; i += 6; continue;
        }

        if (op == 0x10) {                       // XF write
            if (n - i < 5) { g_need = 5; break; }
            const u32 count = ((be16(p + i + 1)) & 0xFFFF) + 1;
            // TexGen config lives at XF 0x1040-0x104F; log what the guest actually writes so
            // an "unconfigured tcg" in aurora can be attributed to the game or to us.
            const size_t len = 1 + 4 + count * 4;
            if (n - i < len) { g_need = len; break; }
            {   // XF 0x1020 is the projection (6 params + type). Aurora only latches it when
                // it arrives as a single write of >= 7 words starting exactly at 0x1020, so
                // record what the guest actually emits — the count and address here decide
                // whether the projection is ever applied at all.
                const u32 addr = be32(p + i + 1) & 0xFFFF;
                // XF 0x0000-0x000B is PNMTX0 (row-major 3x4). GXLoadPosMtxImm writes all 12.
                if (addr < 0x000C) {
                    const u32 cnt = ((be32(p + i + 1) >> 16) & 0xF) + 1;
                    for (u32 w = 0; w < cnt && (addr + w) < 12; ++w) {
                        union { u32 u; float f; } cv;
                        cv.u = be32(p + i + 5 + w * 4);
                        g_pnmtx0[addr + w] = cv.f;
                    }
                }
                if (addr == 0x1020) {
                    // 6 params + type. type 1 == GX_ORTHOGRAPHIC.
                    const u32 cnt = ((be32(p + i + 1) >> 16) & 0xF) + 1;
                    if (cnt >= 7) {
                        float pr[6];
                        for (int w = 0; w < 6; ++w) {
                            union { u32 u; float f; } cv;
                            cv.u = be32(p + i + 5 + w * 4);
                            pr[w] = cv.f;
                        }
                        const u32 type = be32(p + i + 5 + 6 * 4);
                        g_fifoProjOrtho = (type == 1);
                        for (int j = 0; j < 16; ++j) g_fifoProj[j] = 0.0f;
                        if (g_fifoProjOrtho) {
                            g_fifoProj[0] = pr[0]; g_fifoProj[3]  = pr[1];
                            g_fifoProj[5] = pr[2]; g_fifoProj[7]  = pr[3];
                            g_fifoProj[10] = pr[4]; g_fifoProj[11] = pr[5];
                            g_fifoProj[15] = 1.0f;
                        } else {
                            g_fifoProj[0] = pr[0]; g_fifoProj[2]  = pr[1];
                            g_fifoProj[5] = pr[2]; g_fifoProj[6]  = pr[3];
                            g_fifoProj[10] = pr[4]; g_fifoProj[11] = pr[5];
                            g_fifoProj[14] = -1.0f;
                        }
                    }
                    static u32 seen = 0;
                    if (seen < 4) {
                        ++seen;
                        lucent::debug("gxfifo", "XF projection write: addr 0x{:04x} count {}",
                                      addr, count);
                    }
                }
            }
            // COLOUR CHANNELS AND LIGHTS live in XF memory, and (like TEV) J3D writes them from
            // baked display lists, so this is the only place they can be observed.
            //   0x0600..0x067F  eight light objects, 16 words each
            //   0x100A..0x100D  ambient colours (chan 0/1), then material colours
            //   0x100E..0x1011  colour and alpha channel control
            {
                const u32 addr = be32(p + i + 1) & 0xFFFF;
                for (u32 w = 0; w < count; ++w) {
                    const u32 a = addr + w;
                    const u32 v = be32(p + i + 5 + w * 4);
                    if (a >= 0x0600 && a < 0x0680) {
                        const unsigned li = (a - 0x0600) >> 4;
                        const unsigned wi = (a - 0x0600) & 15;
                        SbrLight& L = g_xf.light[li];
                        auto f = [&](u32 bits) { float o; __builtin_memcpy(&o, &bits, 4); return o; };
                        switch (wi) {
                        case 3:  // RGBA8 colour
                            L.color[0] = (float)((v >> 24) & 0xFF) / 255.0f;
                            L.color[1] = (float)((v >> 16) & 0xFF) / 255.0f;
                            L.color[2] = (float)((v >> 8) & 0xFF) / 255.0f;
                            L.color[3] = (float)(v & 0xFF) / 255.0f;
                            break;
                        case 4: L.cosAtt[0] = f(v); break;
                        case 5: L.cosAtt[1] = f(v); break;
                        case 6: L.cosAtt[2] = f(v); break;
                        case 7: L.distAtt[0] = f(v); break;
                        case 8: L.distAtt[1] = f(v); break;
                        case 9: L.distAtt[2] = f(v); break;
                        case 10: L.pos[0] = f(v); break;
                        case 11: L.pos[1] = f(v); break;
                        case 12: L.pos[2] = f(v); break;
                        case 13: L.dir[0] = f(v); break;
                        case 14: L.dir[1] = f(v); break;
                        case 15: L.dir[2] = f(v); break;
                        default: break;
                        }
                    } else if (a >= 0x100A && a <= 0x100D) {
                        float* c = (a <= 0x100B) ? g_xf.ambient[a - 0x100A]
                                                 : g_xf.material[a - 0x100C];
                        c[0] = (float)((v >> 24) & 0xFF) / 255.0f;
                        c[1] = (float)((v >> 16) & 0xFF) / 255.0f;
                        c[2] = (float)((v >> 8) & 0xFF) / 255.0f;
                        c[3] = (float)(v & 0xFF) / 255.0f;
                    } else if (a >= 0x100E && a <= 0x1011) {
                        SbrChanCtrl& c = g_xf.chan[a - 0x100E];
                        c.matSrcVertex = v & 1;
                        c.enableLight  = (v >> 1) & 1;
                        c.lightMask    = ((v >> 2) & 0xF) | (((v >> 11) & 0xF) << 4);
                        c.ambSrcVertex = (v >> 6) & 1;
                        c.diffuseFn    = (v >> 7) & 3;
                        c.attnEnable   = (v >> 9) & 1;
                        c.attnSpot     = (v >> 10) & 1;
                    } else if (a == 0x1009) {
                        g_xf.numChans = v & 3;
                    } else if (a >= 0x0078 && a < 0x00F0) {
                        // XF matrix memory is addressed in ROWS of four floats, so texture matrix
                        // n occupies 12 consecutive words starting at 0x78 + n*12. GXLoadTexMtxImm
                        // writes them here directly, which is how J3D's animated texture SRTs and
                        // its environment-map matrices reach the hardware.
                        const unsigned slot = (a - 0x0078) / 12;
                        const unsigned off  = (a - 0x0078) % 12;
                        float f; __builtin_memcpy(&f, &v, 4);
                        if (slot < 10) {
                            g_xf.texMtx[slot][off] = f;
                            g_xf.texMtxWritten |= 1u << slot;
                        }
                    } else if (a == 0x1018 || a == 0x1019) {
                        // MatrixIndexA/B: which matrix each texgen uses, six bits each, in GX
                        // matrix-id units (30..57 are the ten texture matrices, 60 is identity).
                        const unsigned base = (a == 0x1018) ? 0u : 4u;
                        const unsigned n    = (a == 0x1018) ? 4u : 4u;
                        for (unsigned t = 0; t < n; ++t) {
                            const unsigned bit = (a == 0x1018) ? (6 + t * 6) : (t * 6);
                            const unsigned id  = (v >> bit) & 0x3F;
                            g_xf.texGen[base + t].mtxSlot =
                                (id >= 30 && id <= 57) ? (uint8_t)((id - 30) / 3) : (uint8_t)0xFF;
                        }
                    } else if (a >= 0x1040 && a <= 0x1047) {
                        SbrTexGen& tg = g_xf.texGen[a - 0x1040];
                        tg.projection = (v >> 1) & 1;
                        tg.inputForm  = (v >> 2) & 3;
                        tg.type       = (v >> 4) & 7;
                        tg.sourceRow  = (v >> 7) & 0x1F;
                    }
                }
            }

            g_out.insert(g_out.end(), p + i, p + i + len);
            g_stats.xf++; i += len; continue;
        }

        if (op == 0x61) {                       // BP register write
            if (n - i < 5) { g_need = 5; break; }
            const u8  reg = p[i + 1];
            const u32 raw = ((u32)p[i + 2] << 16) | ((u32)p[i + 3] << 8) | p[i + 4];

            // BP WRITE MASK (register 0xFE). GX's BP is not write-only: a write to 0xFE arms a
            // 24-bit mask for the NEXT register write, and only the masked bits are updated — the
            // rest KEEP their previous value. The mask then resets. The game uses this constantly:
            // GDGeometry.c's GDSetGenMode2 arms 0x07FC3F before writing GENMODE, and GDSetCullMode
            // arms 0xC000 and writes a value whose other 22 bits are ZERO.
            //
            // Applying the raw payload instead — which this parser did — clobbers every field
            // outside the mask. For GENMODE that silently rewrites numTevStages and numTexGens on
            // any masked write, which is exactly where the per-draw oracle showed this port and
            // aurora diverging (mine 5 stages vs aurora 6, 2 vs 5, 3 vs 1 on sampled draws).
            //
            // Aurora has implemented this all along (command_processor.cpp: merged =
            // (cached & ~mask) | (value & mask)); its own comment notes a genMode write that sets
            // only bit 15 merging with a cached bit 14. That is why it renders and this did not.
            if (reg == 0xFE) {
                g_bpMask = raw & 0x00FFFFFFu;
                g_out.insert(g_out.end(), p + i, p + i + 5);
                ++g_bpWrites[reg];
                g_stats.bp++; i += 5; continue;
            }
            const u32 val = (g_bpCache[reg] & ~g_bpMask) | (raw & g_bpMask);
            g_bpMask = 0x00FFFFFFu;
            g_bpCache[reg] = val;

            // TEV STATE. Same reasoning as the texture binding below: J3D bakes its TEV setup into
            // per-material display lists, so the SDK entry points are not called and the BP
            // registers those lists write are the only complete source.
            //
            //  0x00 GENMODE            : numTevStages-1 in bits 10..13, numTexGens in bits 0..3
            //  0x28..0x2F RAS1_TREF    : two stages each — texmap, texcoord, colour channel
            //  0xC0+2i TEV_COLOR_ENV   : stage i colour combiner (a,b,c,d,bias,sub,clamp,scale,dest)
            //  0xC1+2i TEV_ALPHA_ENV   : stage i alpha combiner, plus the ras/tex swap selectors
            //  0xE0..0xE7 TEV_REGISTER : the four colour registers (prev, c0, c1, c2)
            //  0xF6..0xFD TEV_KSEL     : konst colour/alpha selectors, two stages each
            if (reg == 0x00) {
                g_tev.numTexGens  = val & 0xF;
                g_tev.numStages   = ((val >> 10) & 0xF) + 1;
                // numChans ALSO lives in GENMODE (bits 4..6). GDSetGenMode2 writes both this and
                // XF 0x1009, but GDSetCullMode's masked GENMODE writes and any path that touches
                // GENMODE alone update only here — aurora reads it from GENMODE, so this parser
                // does too and the XF write is the redundant twin.
                g_xf.numChans     = (val >> 4) & 7;
                // GENMODE bits 14-15 carry the cull mode, and GX's FRONT/BACK are swapped relative
                // to the rasteriser's (aurora does the same swap, command_processor.cpp:881-893).
                // Ported rather than guessed: getting the sense backwards culls exactly the faces
                // that should survive, which looks like a completely different bug.
                {
                    const uint32_t hw = (val >> 14) & 3;
                    g_fifoZ.cull = (uint8_t)(hw == 1 ? 2 : hw == 2 ? 1 : hw);
                }
            } else if (reg >= 0x28 && reg <= 0x2F) {
                g_tev.trefSeq[reg - 0x28] = g_bindSeq;
                const unsigned s0 = (unsigned)(reg - 0x28) * 2;
                g_tev.stage[s0].texmap     = val & 7;
                g_tev.stage[s0].texcoord   = (val >> 3) & 7;
                g_tev.stage[s0].texEnable  = (val >> 6) & 1;
                g_tev.stage[s0].rasChannel = (val >> 7) & 7;
                g_tev.stage[s0 + 1].texmap     = (val >> 12) & 7;
                g_tev.stage[s0 + 1].texcoord   = (val >> 15) & 7;
                g_tev.stage[s0 + 1].texEnable  = (val >> 18) & 1;
                g_tev.stage[s0 + 1].rasChannel = (val >> 19) & 7;
            } else if (reg >= 0xC0 && reg <= 0xDF) {
                const unsigned i = (unsigned)(reg - 0xC0) >> 1;
                if ((reg & 1) == 0) {
                    SbrTevStage& t = g_tev.stage[i];
                    t.cD = val & 0xF;       t.cC = (val >> 4) & 0xF;
                    t.cB = (val >> 8) & 0xF; t.cA = (val >> 12) & 0xF;
                    t.cBias = (val >> 16) & 3; t.cSub = (val >> 18) & 1;
                    t.cClamp = (val >> 19) & 1; t.cScale = (val >> 20) & 3;
                    t.cDest = (val >> 22) & 3;
                } else {
                    SbrTevStage& t = g_tev.stage[i];
                    t.aD = (val >> 4) & 7;  t.aC = (val >> 7) & 7;
                    t.aB = (val >> 10) & 7; t.aA = (val >> 13) & 7;
                    t.aBias = (val >> 16) & 3; t.aSub = (val >> 18) & 1;
                    t.aClamp = (val >> 19) & 1; t.aScale = (val >> 20) & 3;
                    t.aDest = (val >> 22) & 3;
                    // Swap-mode selectors ride the alpha combiner word (GXSetTevSwapMode).
                    t.swapRas = val & 3;
                    t.swapTex = (val >> 2) & 3;
                }
            } else if (reg >= 0xE0 && reg <= 0xE7) {
                // 0xE0..0xE7 carry BOTH the TEV colour registers and the KONST registers; bit 23
                // selects which. Treating every write as a colour register left konst reading
                // whatever the colour registers held, which blew characters out to white.
                //
                // Each register is written as two halves: the even (low) write carries R and A, the
                // odd (high) write carries B and G. Values are signed 11-bit (S10) over 255, so
                // they legitimately exceed 1.0 and must not be clamped here.
                const unsigned idx = (unsigned)(reg - 0xE0) >> 1;
                const bool high = ((reg - 0xE0) & 1) != 0;
                const bool isKonst = ((val >> 23) & 1) != 0;
                auto s10 = [](u32 v) { return (float)(int32_t)((v & 0x7FF) << 21 >> 21) / 255.0f; };
                float (*dst)[4] = isKonst ? g_tev.konstReg : g_tev.reg;
                if (!high) {
                    // R sits in bits 0..10 and A in bits 12..22 — GXSetTevColor[S10]/GXSetTevKColor
                    // (decomp/sms GXTev.c) pack r at bit 0 and a at bit 12, and aurora reads the
                    // same way. This parser had the two SWAPPED, so every TEV register and every
                    // KONST carried its alpha in the red component and vice versa.
                    dst[idx][0] = s10(val & 0x7FF);          // R
                    dst[idx][3] = s10((val >> 12) & 0x7FF);  // A
                } else {
                    dst[idx][2] = s10(val & 0x7FF);          // B
                    dst[idx][1] = s10((val >> 12) & 0x7FF);  // G
                }
            } else if (reg == 0xF3) {
                // TEV_ALPHAFUNC: ref0 bits 0..7, ref1 bits 8..15, comp0 bits 16..18,
                // comp1 bits 19..21, logic bits 22..23. The two comparisons are combined by the
                // logic op, which is how GX expresses a band as well as a simple cutout.
                g_tev.alphaRef0  = (uint8_t)(val & 0xFF);
                g_tev.alphaRef1  = (uint8_t)((val >> 8) & 0xFF);
                g_tev.alphaOp0   = (uint8_t)((val >> 16) & 7);
                g_tev.alphaOp1   = (uint8_t)((val >> 19) & 7);
                g_tev.alphaLogic = (uint8_t)((val >> 22) & 3);
            } else if (reg >= 0xF6 && reg <= 0xFD) {
                const unsigned s0 = (unsigned)(reg - 0xF6) * 2;
                g_tev.stage[s0].kC     = (val >> 4) & 0x1F;
                g_tev.stage[s0].kA     = (val >> 9) & 0x1F;
                g_tev.stage[s0 + 1].kC = (val >> 14) & 0x1F;
                g_tev.stage[s0 + 1].kA = (val >> 19) & 0x1F;
                // Bits 0..3 of the same registers carry the swap TABLE, two components per write
                // (GXSetTevSwapModeTable): even register red/green, odd register blue/alpha.
                const unsigned tbl = (unsigned)(reg - 0xF6) / 2;
                if (((reg - 0xF6) & 1) == 0) {
                    g_tev.swapTable[tbl][0] = (uint8_t)(val & 3);
                    g_tev.swapTable[tbl][1] = (uint8_t)((val >> 2) & 3);
                } else {
                    g_tev.swapTable[tbl][2] = (uint8_t)(val & 3);
                    g_tev.swapTable[tbl][3] = (uint8_t)((val >> 2) & 3);
                }
            }

            // TEXTURE BINDING. J3D bakes its material texture loads into per-material display
            // lists and replays them, so GXLoadTexObj is almost never called (J3DSys::reinitTexture
            // is its only caller in J3D, for a 4x4 null texture). The registers those lists write
            // ARE the binding, and reading them here is independent of J3D's class structure —
            // which the tev-block subclasses make awkward to walk.
            //
            // TX_SETIMAGE0 (0x88+i, and 0xA8+i for texmaps 4-7): width-1 bits 0..9,
            // height-1 bits 10..19, format bits 20..23.
            // TX_SETIMAGE3 (0x94+i / 0xB4+i): image base address in 32-byte units.
            // TX_SETTLUT   (0x98+i / 0xB8+i): TLUT base in 32-byte units, bits 0..9 of the entry.
            // TX_SETMODE0 (0x80+i / 0xA0+i): wrap S bits 0..1, wrap T bits 2..3, mag filter bit 4,
            // min filter bits 5..7. The min filter's value encodes the mip mode as well; only
            // whether it is LINEAR matters here, since this path uploads a single level.
            if ((reg >= 0x80 && reg <= 0x83) || (reg >= 0xA0 && reg <= 0xA3)) {
                SbrTexture& t = g_fifoTex[(reg >= 0xA0) ? (4 + reg - 0xA0) : (reg - 0x80)];
                t.wrapS     = (uint8_t)(val & 3);
                t.wrapT     = (uint8_t)((val >> 2) & 3);
                t.magLinear = (uint8_t)((val >> 4) & 1);
                t.minLinear = (uint8_t)(((val >> 5) & 7) != 0);
            }
            if (reg >= 0x88 && reg <= 0x8B) {
                SbrTexture& t = g_fifoTex[reg - 0x88];
                t.width  = (val & 0x3FF) + 1;
                t.height = ((val >> 10) & 0x3FF) + 1;
                t.format = (val >> 20) & 0xF;
            } else if (reg >= 0xA8 && reg <= 0xAB) {
                SbrTexture& t = g_fifoTex[4 + (reg - 0xA8)];
                t.width  = (val & 0x3FF) + 1;
                t.height = ((val >> 10) & 0x3FF) + 1;
                t.format = (val >> 20) & 0xF;
            } else if (reg >= 0x94 && reg <= 0x97) {
                const unsigned m = reg - 0x94;
                const uint32_t na = ((val & 0x00FFFFFF) << 5) | 0x80000000u;
                if (na != g_fifoTex[m].addr) {
                    g_fifoTexPrevAddr[m] = g_fifoTex[m].addr;
                    g_fifoTexBindPos[m] = (uint32_t)g_out.size();
                }
                g_fifoTex[reg - 0x94].addr = na;
                g_fifoTex[reg - 0x94].bindSeq = ++g_bindSeq;
            } else if (reg >= 0xB4 && reg <= 0xB7) {
                g_fifoTex[4 + (reg - 0xB4)].addr = ((val & 0x00FFFFFF) << 5) | 0x80000000u;
                g_fifoTex[4 + (reg - 0xB4)].bindSeq = ++g_bindSeq;
            } else if (reg >= 0x98 && reg <= 0x9B) {
                // TMEM offset of the palette, in 32-byte units. The TLUT's MAIN-MEMORY address is
                // written separately by the TLUT load (BP 0x64/0x65); this port does not track that
                // yet, so C4/C8 textures decline rather than decode against a wrong palette.
                g_fifoTex[reg - 0x98].tlut = 0;
            }
            // 0x40 ZMode and 0x41 cmode0 — the RASTER state, ported from the hardware encoding
            // rather than taken from the SDK. J3D writes these through display lists (measured:
            // 2.6M ZMode and 4.5M cmode0 writes per frame), so the GXSetZMode/GXSetBlendMode
            // overrides this port was reading are stale for essentially every J3D draw. That is
            // the same defect shape as reading the SDK texObj slot instead of the BP image base,
            // and it lands on the state that decides whether a draw COVERS what is behind it.
            //
            //   0x40: bit0 test | bits1-3 func | bit4 write
            //   0x41: bit0 blendEn | bit1 logicEn | bit2 dither | bit3 colorUpdate |
            //         bit4 alphaUpdate | bits5-7 dstFactor | bits8-10 srcFactor | bit11 subtract |
            //         bits12-15 logicOp.  Mode precedence is subtract > blend > logic > none.
            if (reg == 0x40) {
                g_fifoZ.test  = (uint8_t)(val & 1);
                g_fifoZ.func  = (uint8_t)((val >> 1) & 7);
                g_fifoZ.write = (uint8_t)((val >> 4) & 1);
            } else if (reg == 0x41) {
                const bool blendEn  = (val & 1) != 0;
                const bool logicEn  = ((val >> 1) & 1) != 0;
                const bool subtract = ((val >> 11) & 1) != 0;
                g_fifoZ.colorUpdate = (uint8_t)((val >> 3) & 1);
                g_fifoZ.alphaUpdate = (uint8_t)((val >> 4) & 1);
                g_fifoZ.dstFac = (uint8_t)((val >> 5) & 7);
                g_fifoZ.srcFac = (uint8_t)((val >> 8) & 7);
                g_fifoZ.blend  = subtract ? 3 : (blendEn ? 1 : (logicEn ? 2 : 0));
            }
            // 0x20/0x21 SCISSOR. Both registers are needed to form the rect, so it is recomputed
            // whenever either arrives. The -342 bias and the inclusive right/bottom are the
            // hardware's, ported from aurora's decode (command_processor.cpp:940-951) rather than
            // guessed; getting the bias wrong silently shifts every clip by a constant.
            if (reg == 0x20 || reg == 0x21) {
                g_scisRaw[reg - 0x20] = val;
                const int32_t tp = (int32_t)(g_scisRaw[0] & 0x7FF) - 342;
                const int32_t lf = (int32_t)((g_scisRaw[0] >> 12) & 0x7FF) - 342;
                const int32_t bm = (int32_t)(g_scisRaw[1] & 0x7FF) - 342;
                const int32_t rt = (int32_t)((g_scisRaw[1] >> 12) & 0x7FF) - 342;
                g_fifoZ.scissor[0] = (int16_t)lf;
                g_fifoZ.scissor[1] = (int16_t)tp;
                g_fifoZ.scissor[2] = (int16_t)std::max(rt - lf + 1, 0);
                g_fifoZ.scissor[3] = (int16_t)std::max(bm - tp + 1, 0);
            }
            // 0x49: EFB copy top-left (10 bits each). 0x4A: width-1 / height-1.
            if (reg == 0x49) { g_copy_left = val & 0x3FF; g_copy_top = (val >> 10) & 0x3FF; }
            else if (reg == 0x4A) { g_copy_w = (val & 0x3FF) + 1; g_copy_h = ((val >> 10) & 0x3FF) + 1; }
            // 0x4B: copy destination address (32-byte units). 0x4E: vertical copy scale.
            else if (reg == 0x4B) { g_copy_dest = val & 0x00FFFFFFu; }
            // 0x4F/0x50: the EFB clear colour (AR / GB). A copy with the clear bit set leaves
            // the EFB filled with this, so anything grabbed before much is drawn returns it.
            else if (reg == 0x4F || reg == 0x50) {
                static u32 ar = 0xFFFFFFFF, gb = 0xFFFFFFFF;
                if (reg == 0x4F) ar = val; else gb = val;
                static u32 last_ar = 0, last_gb = 0;
                if (ar != last_ar || gb != last_gb) {
                    last_ar = ar; last_gb = gb;
                    lucent::debug("gxfifo", "EFB clear colour: a={} r={} g={} b={}",
                                  (ar >> 8) & 0xFF, ar & 0xFF, (gb >> 8) & 0xFF, gb & 0xFF);
                }
            }
            else if (reg == 0x4E) { g_copy_yscale = val & 0x00FFFFFFu; }
            // 0x52 triggers a copy. Bit 14 selects copy-to-XFB (the presented frame); with it
            // clear the copy targets a TEXTURE, which is equally real — render-to-texture
            // content like the sea's reflection is built this way. Handling only the display
            // copy silently dropped every texture copy.
            else if (reg == 0x52) {
                // Count copy-to-XFB triggers. The XFB copy is what makes a rendered EFB become the
                // PRESENTED image; a pass that renders without one is invisible however much
                // geometry it emitted. Counting them per pass is the difference between "the
                // sub-frame drew nothing" and "the sub-frame drew and was never copied out".
                if (val & (1u << 14)) ++g_xfbCopies;
                emit_copy_state(val, (val & (1u << 14)) != 0);
            }
            // Texture image registers. Maps 0-3 use the 0x8x/0x9x ids, maps 4-7 the 0xAx/0xBx
            // ids. A texobj is emitted once both halves of a slot are known.
            else if (reg >= 0x88 && reg <= 0x8B) { u32 m = reg - 0x88; g_tex[m].image0 = val; g_tex[m].have0 = true; emit_texobj(m); }
            else if (reg >= 0xA8 && reg <= 0xAB) { u32 m = reg - 0xA8 + 4; g_tex[m].image0 = val; g_tex[m].have0 = true; emit_texobj(m); }
            else if (reg >= 0x94 && reg <= 0x97) { u32 m = reg - 0x94; g_tex[m].image3 = val; g_tex[m].have3 = true; emit_texobj(m);
            }
            else if (reg >= 0xB4 && reg <= 0xB7) { u32 m = reg - 0xB4 + 4; g_tex[m].image3 = val; g_tex[m].have3 = true; emit_texobj(m); }

            g_out.insert(g_out.end(), p + i, p + i + 5);
            // Per-register write counts. The texture-unit staleness question is exactly "how often
            // does the game bind each unit", and that is answerable here rather than by inference:
            // TX_SETIMAGE3 is 0x94+m for units 0-3, so those four counts ARE the per-unit bind
            // rate. If they are comparable while the observed textures are not, the binds are
            // being seen and lost downstream; if they differ, the units genuinely are not rebound.
            ++g_bpWrites[reg];
            g_stats.bp++; i += 5; continue;
        }

        // Indexed XF loads: GX_LOAD_INDX_A/B/C/D select the PosMtx / NrmMtx / TexMtx /
        // Light arrays. One u32 payload: index<<16 | (len-1)<<12 | xfAddr. J3D emits these
        // constantly for per-shape matrix loads, and NOT knowing them was the desync: the
        // parser hit "unrecognised opcode" and dropped the rest of the batch — silently
        // discarding the remainder of any display list that contained one, since
        // inline_display_list ignores the return value.
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            if (n - i < 5) { g_need = 5; break; }
            // Sanity-check the payload: index<<16 | (len-1)<<12 | xfAddr. Real destinations
            // are XF matrix memory (pos 0x000-0x077, tex 0x078-0x0EF, nrm 0x400-0x459,
            // post-tex 0x500-0x5EF). A destination outside those means this parser emitted
            // a mis-framed word, not that aurora lacks a feature — worth distinguishing,
            // because aurora reports both the same way.
            const u32 w  = be32(p + i + 1);
            const u32 da = w & 0x0FFF;
            const bool sane = da < 0x0F0 || (da >= 0x400 && da < 0x45A) ||
                              (da >= 0x500 && da < 0x5F0);
            if (!sane)
                lucent::warn("gxfifo", "indexed XF load op 0x{:02x} dstAddr=0x{:03x} len={} "
                                       "is outside XF matrix memory — likely mis-framed",
                             op, da, ((w >> 12) & 0xF) + 1);
            g_out.insert(g_out.end(), p + i, p + i + 5);
            i += 5; continue;
        }

        // GX_CMD_CALL_DL is 0x40 and GX_CMD_INVL_VC is 0x48 — NOT the other way round.
        // Having these swapped consumed 9 bytes for a 1-byte command and left every real
        // display-list call unrecognised, which is what kept desyncing the stream after the
        // indexed-XF fix (mis-framed indexed loads with nonsense destinations like 0xf10).
        if (op == 0x40) {                       // call display list: address + size
            if (n - i < 9) { g_need = 9; break; }
            inline_display_list(be32(p + i + 1), be32(p + i + 5), depth);
            i += 9; continue;
        }

        if (op == 0x48) {                       // invalidate vertex cache: no payload
            g_out.insert(g_out.end(), p + i, p + i + 1);
            i += 1; continue;
        }

        if (op >= 0x80 && op <= 0xBF) {         // draw primitive
            if (n - i < 3) { g_need = 3; break; }
            // Record THIS side's state for the per-draw comparison against aurora, which derives
            // its own from the same bytes a moment later. See state_oracle.h.
            ++g_drawIndex;
            ++g_drawsSinceQuery;
            if (++g_drawsSinceCapture > g_maxDrawsAfterCapture)
                g_maxDrawsAfterCapture = g_drawsSinceCapture;
            if (sbr_state_diff_enabled()) {
                SbrDrawState st{};
                st.pos = (uint32_t)g_out.size();   // where this draw's command byte lands
                sbr_draw_state_fill(st, g_tev, g_xf);
                // Raster state as the STREAM describes it, packed to match aurora's side. This is
                // the check that decides whether the 43% SDK/FIFO disagreement is this port's bug.
                for (int j = 0; j < 4; ++j) st.scissor[j] = g_fifoZ.scissor[j];
                st.cull = g_fifoZ.cull;
                st.raster = (uint16_t)((g_fifoZ.test & 1) | ((g_fifoZ.write & 1) << 1) |
                                       ((g_fifoZ.func & 7) << 2));
                st.blend  = (uint16_t)((g_fifoZ.blend & 7) | ((g_fifoZ.srcFac & 15) << 3) |
                                       ((g_fifoZ.dstFac & 15) << 7));
                for (unsigned m = 0; m < 8; ++m) {
                    st.unitId[m]  = g_fifoTex[m].addr & 0x01FFFFFFu;
                    st.bindPos[m] = g_fifoTexBindPos[m];
                    st.prevId[m]  = g_fifoTexPrevAddr[m] & 0x01FFFFFFu;
                }
                sbr_state_oracle_mine(st);
            }
            const u32 verts = be16(p + i + 1);
            const u32 vsize = vertex_size(op & 7);
            const size_t len = 3 + (size_t)verts * vsize;
            if (vsize == 0) break;                  // VAT not seen yet
            if (n - i < len) { g_need = len; break; }
            // 2D capture: a draw made under an ORTHOGRAPHIC projection is HUD/menu geometry the
            // J3D capture seam never sees. Decode it straight from the stream bytes.
            if (fifo2d_on()) {
                ++g_2dSeen;
                if (g_fifoProjOrtho) {
                    ++g_2dOrtho;
                    if (decode_2d_draw(op, p + i + 3, verts, g_vat[op & 7])) ++g_2dEmitted;
                }
            }
            // The decomp runtime submits ~3,166 vertices across ~334 draws for this same
            // scene (SB_DRAW_STATS), i.e. roughly 10 vertices per draw. A draw claiming
            // thousands is therefore a mis-frame reading data as a command, even when the
            // span happens to land on a valid-looking opcode afterwards.
            if (verts > 4096) {
                lucent::warn("gxfifo",
                             "implausible draw: op=0x{:02x} verts={} vsize={} — preceding "
                             "bytes {:02x} {:02x} {:02x} {:02x}",
                             op, verts, vsize,
                             i >= 4 ? p[i - 4] : 0, i >= 3 ? p[i - 3] : 0,
                             i >= 2 ? p[i - 2] : 0, i >= 1 ? p[i - 1] : 0);
            }

            // Self-check: after a correctly sized draw the next byte must be a plausible
            // opcode. If it is not, THIS draw's vertex size is wrong — report the exact
            // VCD/VAT that produced it rather than letting aurora fail later with a
            // misleading "unsupported primitive" somewhere unrelated.
            if (i + len < n) {
                const u8 nx = p[i + len];
                const bool ok = nx == 0x00 || nx == 0x08 || nx == 0x10 || nx == 0x40 ||
                                nx == 0x48 || nx == 0x50 || nx == 0x61 ||
                                nx == 0x20 || nx == 0x28 || nx == 0x30 || nx == 0x38 ||
                                (nx >= 0x80 && nx <= 0xBF);
                if (!ok) {
                    const Vat& v = g_vat[op & 7];
                    lucent::warn("gxfifo",
                                 "vertex size {} looks wrong for vat{}: next byte 0x{:02x} "
                                 "after {} verts. vcd_lo=0x{:08x} vcd_hi=0x{:08x} "
                                 "fmt0=0x{:08x} fmt1=0x{:08x} fmt2=0x{:08x}",
                                 vsize, op & 7, nx, verts, v.vcd_lo, v.vcd_hi,
                                 v.fmt0, v.fmt1, v.fmt2);
                }
            }
            g_out.insert(g_out.end(), p + i, p + i + len);
            g_stats.draws++; g_stats.verts += verts;
            i += len; continue;
        }

        // Anything else means the stream framing is wrong; stop rather than resync blindly
        // on data that would look like opcodes.
        g_stats.unknown++;
        lucent::debug("gxfifo", "unrecognised opcode 0x{:02x} — framing lost", op);
        return n;   // drop the rest of this batch
    }
    if (i == n) g_need = 0;   // fully consumed; nothing outstanding
    return i;
}

void fifo_write(u32 ea, unsigned width, u32 value) {
    (void)ea;
    u8 tmp[4];
    for (unsigned k = 0; k < width; k++) tmp[k] = (u8)(value >> (8 * (width - 1 - k)));
    g_buf.insert(g_buf.end(), tmp, tmp + width);
    g_stats.bytes += width;

    // Only re-parse once the outstanding command can actually complete. Without this a
    // single large draw (58k vertices is ~1.7 MB) makes every subsequent 4-byte write
    // re-scan the whole buffer from the start — quadratic, and it dominated the frame time.
    if (g_buf.size() >= 4096 && g_buf.size() >= g_need) {
        const size_t used = parse(g_buf.data(), g_buf.size());
        g_buf.erase(g_buf.begin(), g_buf.begin() + used);
        if (g_buf.size() > 1u << 20) g_buf.clear();   // framing lost; do not grow unbounded
    }
}

u32 fifo_read(u32 ea, unsigned width) {
    lucent::error("gxfifo", "read{} from the write-gather pipe @ 0x{:08x} — it is write-only",
                  width * 8, ea);
    std::abort();
}

} // namespace

// Parse this frame's stream and rotate it into g_last (with the previous frame in g_prev). Does
// NOT send anything to aurora — the frame seam decides what to send and present. The build/send
// split is a useful separation in its own right; it no longer serves any interpolation consumer.
void sbr_gxfifo_pop_stream_reset();

void gxfifo_build() {
    // Drain whatever is still buffered. Parsing only ran once 4096 bytes had accumulated, so a
    // frame's trailing commands could sit unparsed and be emitted in the NEXT frame's stream.
    while (!g_buf.empty()) {
        const size_t used = parse(g_buf.data(), g_buf.size());
        if (used == 0) break;
        g_buf.erase(g_buf.begin(), g_buf.begin() + used);
    }
    if (g_out.empty()) return;

    lucent::debug("gxfifo", "frame stream {} KB ({} DL expansions, {} KB inlined)",
                  g_out.size() >> 10, g_dl_calls, g_dl_bytes >> 10);
    g_dl_calls = 0; g_dl_bytes = 0;

    // Same frame boundary the state oracle pairs on: this is where a frame's stream is closed.
    sbr_state_oracle_mine_frame_end();
    g_last.swap(g_out);
    g_out.clear();
    // The new stream carries no label yet, so the next one must be written even if it repeats the
    // value the last frame ended on (see emit_draw_pop).
    sbr_gxfifo_pop_stream_reset();
}

void gxfifo_send(const std::vector<u8>& s) {
    if (!s.empty()) aurora_fifo_replay(s.data(), (u32)s.size(), /*bigEndian=*/1);
}

// The non-interpolated path: build this frame and send it. Behaviour unchanged from before the
// build/send split.
void gxfifo_flush() {
    gxfifo_build();
    gxfifo_send(g_last);
}

void gxfifo_send_last() { gxfifo_send(g_last); }
const std::vector<u8>& gxfifo_last_frame() { return g_last; }

void gxfifo_stats(u64& draws, u64& verts, u64& bytes) {
    draws = g_stats.draws; verts = g_stats.verts; bytes = g_stats.bytes;
}

void gxfifo_device_init() {
    g_buf.reserve(1 << 16);
    g_out.reserve(1 << 20);
    // The gather pipe is a single address the CPU stores to repeatedly; the block is
    // 0xCC008000-0xCC008020 on hardware.
    mmio_register(MmioDevice{FIFO_BASE, FIFO_BASE + 0x20, "gxfifo", &fifo_read, &fifo_write});
}

// Parse whatever the game has written but not yet had parsed. The incremental path above only
// runs once 4096 bytes have accumulated (a deliberate anti-quadratic guard), so a material's
// texture registers can still be sitting unparsed while its shapes draw — which made every shape
// in a tick see the same stale binding (measured: 11 distinct textures across 929 drawables).
//
// This is the SAME parse the 4096-byte path performs, just without the size threshold, so it has
// no ordering consequence for aurora: those bytes were going to be parsed at exactly this point in
// the stream regardless.
// SBR_FIFO_STALL=1: catch the drain going quadratic. `used == 0` means the buffer holds an
// incomplete command, and since this runs once per J3DShape::draw, every following shape then
// re-parses the same bytes from the start. That is invisible from outside — the game does not
// crash, it just stops making progress — so the pathological case has to announce itself rather
// than be inferred from a stack sample. Reports the buffer size, what the parser is waiting for,
// and how long the no-progress run is; a healthy drain never prints.
void gxfifo_drain_pending() {
    if (!g_buf.empty() && g_buf.size() >= g_need) {
        const size_t before = g_buf.size();
        const size_t used = parse(g_buf.data(), g_buf.size());
        g_buf.erase(g_buf.begin(), g_buf.begin() + used);
        static const bool watch = std::getenv("SBR_FIFO_STALL") != nullptr;
        if (watch) {
            static long stuck = 0, reported = 0;
            if (used == 0) {
                if (++stuck % 200 == 0 && reported < 40) {
                    ++reported;
                    lucent::info("fifostall",
                                 "drain made NO progress {} times in a row: buf={} KB need={} "
                                 "out={} KB — every further drain re-parses this buffer",
                                 stuck, before >> 10, g_need, g_out.size() >> 10);
                }
            } else {
                stuck = 0;
            }
        }
    }
}

// The texture currently bound to a texmap, as the game's own display lists described it to the
// hardware. This is the binding the native renderer must use.
SbrTexture sbr_gx_fifo_texture(unsigned texmap) {
    return g_fifoTex[texmap & 7];
}

// The TEV configuration the material display lists have written.
// Where the parser currently is in THIS frame's stream. The J3D capture seam snapshots parser
// state from the CPU side, at a moment that is not obviously the same moment as the draw it is
// labelling; recording the position lets that assumption be checked instead of trusted.
uint32_t sbr_gxfifo_stream_pos() { return (uint32_t)g_out.size(); }

// FNV-1a over a byte range of the emitted stream. The 60fps arc needs to bisect "the interpolated
// state never reached the emitter" from "the emitter got it and the difference was lost later", and
// every measurement so far has been of game STATE, never of the artifact. Hashing the bytes a
// sub-frame emitted answers it directly: two alphas that produce the same stream cannot produce
// different pixels, whatever the state said.
//
// A range, not the whole buffer, because a sub-frame is a suffix of a much larger tick stream and a
// whole-buffer hash would be dominated by bytes neither alpha could affect.
unsigned long long sbr_gxfifo_stream_hash(uint32_t from, uint32_t to) {
    if (to > g_out.size()) to = (uint32_t)g_out.size();
    unsigned long long h = 1469598103934665603ULL;
    for (uint32_t i = from; i < to; ++i) { h ^= g_out[i]; h *= 1099511628211ULL; }
    return h;
}

// Copy-to-XFB triggers parsed so far. See the BP 0x52 site: this is what turns a rendered EFB into
// the image that reaches the display.
unsigned long sbr_gxfifo_xfb_copies() { return g_xfbCopies; }

// Tag every draw that follows with `tag`, until the next tag — aurora's GX_AURORA_DRAW_TAG.
//
// This is the identity interpolated 60fps pairs on. It is emitted from OUR side because aurora
// cannot derive one: a content hash is impossible for indexed geometry (its vertex buffer holds
// INDICES, with the attributes in a separate storage buffer), and a draw ordinal is worse than
// useless because draw merging is state-dependent, so the same scene yields different ordinals
// when a state write lands differently.
//
// Pending guest FIFO bytes are drained FIRST. Without that, the guest writes that are still sitting
// in g_buf get parsed after this tag and would be attributed to the object being tagged — the tag
// would sit at the wrong point in the stream, which is the whole thing it exists to get right.
// Hand aurora this tick's view matrix (j3dSys.mViewMtx, US 0x804045DC, offset 0 — a GC Mtx, 3 rows
// of 4 big-endian floats).
//
// Interpolation needs it because J3D concatenates the camera into every draw matrix in viewCalc, so
// nothing that reaches the hardware has a camera in it that aurora could isolate. Without this,
// draws that cannot be paired across ticks stay on the CURRENT viewpoint while paired ones move to
// the in-between one, and the frame is rendered from two viewpoints at once — measured as a 15x
// jump in inter-frame energy, i.e. worse than not interpolating at all.
//
// Emitted at the END of the tick's stream, deliberately: that is the camera the tick's draws were
// actually built with. Emitting at the start would hand over the previous tick's value.
void sbr_gxfifo_view_matrix() {
    constexpr u32 kJ3DSys = 0x804045DC;   // mViewMtx is the first member
    const u32 off = kJ3DSys & 0x01FFFFFFu;
    if (g_ram_base == nullptr || off + 48 > 0x01800000u) return;
    gxfifo_drain_pending();
    put_u8 (g_out, 0x50);
    put_u16(g_out, (u16)GX_AURORA_VIEW_MTX);
    // Copied verbatim: guest floats are big-endian and this stream is big-endian, so a swap here
    // and a swap back in the parser would only be two chances to get it wrong.
    g_out.insert(g_out.end(), g_ram_base + off, g_ram_base + off + 48);
}

// The tag currently in force, mirrored on this side so a seam can ask "is what I am about to emit
// going to be attributable at all?" without parsing the stream back.
static uint64_t g_pendingTagState = 0;
uint64_t sbr_gxfifo_pending_tag() { return g_pendingTagState; }

// The audit label for the draws that follow (GX_AURORA_DRAW_POP). Not an identity: it says WHICH
// SYSTEM emitted a draw, so the interpolation report can be per-population instead of one global
// percentage that cannot separate a correctly-snapping HUD from stuttering world geometry.
//
// The label in force is mirrored on this side (like the tag above) for two consumers: the graphics
// registry needs to know whether a draw is ALREADY claimed by a hand-written seam before it
// attributes it to an emitter site, and the redundant-write skip below needs the previous value.
// The skip matters because the registry labels at GXBegin, which is called about a million times a
// run: re-emitting the same label token would drain and re-parse the fifo every time for a byte
// aurora already has.
// The LOGICAL label (what a seam has asked for) and the label actually written into this frame's
// stream are tracked separately. They differ at a frame boundary: a stream that was built but never
// replayed would leave aurora's latch at some other value, so each frame re-states its first label
// instead of trusting a mirror of a stream nobody consumed.
static u8 g_pendingPop = 0;
static bool g_pendingPopAuto = false;
static int g_popEmitted = -1;   // -1 = nothing written to this frame's stream yet
static bool g_pendingExact = false;
static int g_exactEmitted = -1;

u8 sbr_gxfifo_pending_pop() { return g_pendingPop; }
bool sbr_gxfifo_pending_pop_auto() { return g_pendingPopAuto; }

static void emit_draw_pop(u8 pop, bool automatic) {
    g_pendingPop = pop;
    g_pendingPopAuto = automatic;
    if ((int)pop == g_popEmitted) return;
    g_popEmitted = (int)pop;
    gxfifo_drain_pending();
    put_u8(g_out, 0x50);
    put_u16(g_out, (u16)GX_AURORA_DRAW_POP);
    put_u8(g_out, pop);
}

void sbr_gxfifo_draw_pop(u8 pop) { emit_draw_pop(pop, /*automatic=*/false); }

void sbr_gxfifo_pop_stream_reset() {
    g_popEmitted = -1;
    g_pendingPop = 0;
    g_pendingPopAuto = false;
    g_exactEmitted = -1;
    g_pendingExact = false;
}

// "Present the draws that follow EXACTLY on an interpolated frame." For geometry that is in screen
// space under a PERSPECTIVE projection — an identity position matrix with eye-space vertices — which
// the ortho test cannot see and the camera delta must not touch. See GX_AURORA_DRAW_EXACT.
void sbr_gxfifo_draw_exact(bool on) {
    g_pendingExact = on;
    if ((int)on == g_exactEmitted) return;
    g_exactEmitted = (int)on;
    gxfifo_drain_pending();
    put_u8 (g_out, 0x50);
    put_u16(g_out, (u16)GX_AURORA_DRAW_EXACT);
    put_u8 (g_out, on ? 1 : 0);
}

// The registry's label. Distinguished from the hand-written one so that an AUTO label can be
// replaced by the next site's auto label, while a seam's deliberate label is never stepped on:
// a curated population that set its label around a whole subtree must keep it for that subtree.
void sbr_gxfifo_draw_pop_auto(u8 pop) { emit_draw_pop(pop, /*automatic=*/true); }

void sbr_gxfifo_draw_tag(uint64_t tag) {
    g_pendingTagState = tag;
    gxfifo_drain_pending();
    put_u8 (g_out, 0x50);
    put_u16(g_out, (u16)GX_AURORA_DRAW_TAG);
    put_u64(g_out, tag);
}

const SbrTevState& sbr_gx_fifo_tev() { return g_tev; }

// The colour-channel and light state the display lists have written.
const SbrXfState& sbr_gx_fifo_xf() { return g_xf; }

// How many times each BP register was written. Reported on demand so the texture-binding rate per
// unit is a measurement rather than an inference.
void sbr_gxfifo_report_bp_writes() {
    for (unsigned m = 0; m < 4; ++m)
        lucent::info("gxfifo", "  unit {}: TX_SETIMAGE0 (0x{:02x}) {} writes, TX_SETIMAGE3 "
                               "(0x{:02x}) {} writes, TX_SETMODE0 (0x{:02x}) {} writes",
                     m, 0x88 + m, g_bpWrites[0x88 + m], 0x94 + m, g_bpWrites[0x94 + m],
                     0x80 + m, g_bpWrites[0x80 + m]);
    lucent::info("gxfifo", "  GENMODE (0x00) {} writes, RAS1_TREF 0x28 {} / 0x29 {} / 0x2a {}",
                 g_bpWrites[0x00], g_bpWrites[0x28], g_bpWrites[0x29], g_bpWrites[0x2a]);
    // Texmaps 4-7 BIND rates. Whether any LIVE stage NAMES them is NOT answerable here: the
    // RAS1_TREF registers cover stages 0-15 and keep stale values from earlier materials beyond
    // the current numStages, so counting writes here counted stages that are never evaluated and
    // reported ~24% use of maps 4-7 where the true figure is zero. The trustworthy count is the
    // state oracle's per-unit line, which counts over stages < numStages at DRAW time.
    for (unsigned m = 4; m < 8; ++m)
        lucent::info("gxfifo", "  unit {}: TX_SETIMAGE0 (0x{:02x}) {} writes, TX_SETIMAGE3 "
                               "(0x{:02x}) {} writes, TX_SETMODE0 (0x{:02x}) {} writes",
                     m, 0xA8 + m - 4, g_bpWrites[0xA8 + m - 4], 0xB4 + m - 4,
                     g_bpWrites[0xB4 + m - 4], 0xA0 + m - 4, g_bpWrites[0xA0 + m - 4]);
    // Whether the BP write MASK is used at all. If this is zero the mask handling is inert and any
    // divergence it was supposed to explain is elsewhere — worth knowing before believing the fix.
    lucent::info("gxfifo", "  BP write mask (0xFE) {} writes", g_bpWrites[0xFE]);
    if (g_2dSeen != 0)
        lucent::info("gxfifo", "  2D gate: {} draws seen, {} under ortho, {} emitted", g_2dSeen,
                     g_2dOrtho, g_2dEmitted);
    // BP 0x40 (ZMode) and 0x41 (cmode0/blend). Aurora parses both; this parser does not, and takes
    // its z/blend state from the SDK GXSetZMode/GXSetBlendMode overrides instead. If J3D sets them
    // through display lists these counts are nonzero and that SDK state is stale — the same defect
    // shape as reading the SDK texObj slot instead of the BP image base.
    lucent::info("gxfifo", "  ZMode (0x40) {} writes, cmode0/blend (0x41) {} writes  <- NOT parsed "
                           "here; renderer z/blend comes from the SDK path",
                 g_bpWrites[0x40], g_bpWrites[0x41]);
    // TMEM — RESOLVED BY READING THE GAME'S OWN WRITER, not by inference. Two binders exist and
    // neither invalidates the "latest SETIMAGE0 + latest SETIMAGE3 per unit" model:
    //
    //   - J3D (the path that binds essentially every material here): J3DTevs.cpp `loadTexNo` writes
    //     TX_SETIMAGE0, TX_SETIMAGE3, TX_SETMODE0/1 and — only for CI formats — the TLUT pair. It
    //     NEVER writes TX_SETIMAGE1/2, so TMEM regions keep whatever GXInit left.
    //   - The SDK (GXTexture.c `GXLoadTexObjPreLoaded`) writes six registers in the fixed order
    //     mode0, mode1, image0, image1, image2, image3. image1/image2 come from the tex REGION and
    //     describe TMEM CACHING (where the hardware stages the texels), not which image is sampled.
    //
    // So SETIMAGE3 stays the correct bind stamp for a unit, and TMEM is inert for a port that
    // samples main memory directly. Counted anyway so the claim stays falsifiable.
    for (unsigned m = 0; m < 4; ++m)
        lucent::info("gxfifo", "  unit {}: TX_SETIMAGE1 (0x{:02x}) {} writes, TX_SETIMAGE2 "
                               "(0x{:02x}) {} writes, TX_SETTLUT (0x{:02x}) {} writes",
                     m, 0x8C + m, g_bpWrites[0x8C + m], 0x90 + m, g_bpWrites[0x90 + m],
                     0x98 + m, g_bpWrites[0x98 + m]);
}




// The raster state the display lists have written. See the BP 0x40/0x41 handler.
SbrDepthState sbr_gx_fifo_zmode() { return g_fifoZ; }


// Draw commands seen since the previous call. See g_drawsSinceQuery.
long sbr_gxfifo_take_draw_count() {
    const long n = g_drawsSinceQuery;
    g_drawsSinceQuery = 0;
    return n;
}


// Called by the capture seam: a J3D shape was captured here, so the run of unattributed draws ends.
void sbr_gxfifo_note_capture() { g_drawsSinceCapture = 0; }

// Draws since the last capture, and the longest such run this frame. Resets the peak.
void sbr_gxfifo_take_uncaptured(long* trailing, long* longestRun) {
    *trailing = g_drawsSinceCapture;
    *longestRun = g_maxDrawsAfterCapture;
    g_maxDrawsAfterCapture = 0;
}

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

#include "mmio.h"
#include "native_render.h"

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

namespace {

constexpr u32 FIFO_BASE = 0xCC008000;

// The stream arrives in pieces of 1/2/4/8 bytes; commands straddle those writes, so it has
// to be reassembled before it can be framed.
std::vector<u8> g_buf;

// The stream handed to aurora. Identical to the guest's, except that CP array-base writes
// are rewritten (see below) — aurora cannot use the guest's 32-bit address directly.
std::vector<u8> g_out;
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
                if (addr == 0x1020) {
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
            const u32 val = ((u32)p[i + 2] << 16) | ((u32)p[i + 3] << 8) | p[i + 4];

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
            } else if (reg >= 0x28 && reg <= 0x2F) {
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
                    dst[idx][3] = s10(val & 0x7FF);          // A
                    dst[idx][0] = s10((val >> 12) & 0x7FF);  // R
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
                g_fifoTex[reg - 0x94].addr = ((val & 0x00FFFFFF) << 5) | 0x80000000u;
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
            else if (reg == 0x52) emit_copy_state(val, (val & (1u << 14)) != 0);
            // Texture image registers. Maps 0-3 use the 0x8x/0x9x ids, maps 4-7 the 0xAx/0xBx
            // ids. A texobj is emitted once both halves of a slot are known.
            else if (reg >= 0x88 && reg <= 0x8B) { u32 m = reg - 0x88; g_tex[m].image0 = val; g_tex[m].have0 = true; emit_texobj(m); }
            else if (reg >= 0xA8 && reg <= 0xAB) { u32 m = reg - 0xA8 + 4; g_tex[m].image0 = val; g_tex[m].have0 = true; emit_texobj(m); }
            else if (reg >= 0x94 && reg <= 0x97) { u32 m = reg - 0x94; g_tex[m].image3 = val; g_tex[m].have3 = true; emit_texobj(m); }
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
            const u32 verts = be16(p + i + 1);
            const u32 vsize = vertex_size(op & 7);
            const size_t len = 3 + (size_t)verts * vsize;
            if (vsize == 0) break;                  // VAT not seen yet
            if (n - i < len) { g_need = len; break; }
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
// NOT send anything to aurora — the frame seam decides what to send and present, which is what lets
// 60fps interpolation present a blended stream BEFORE the real one.
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

    g_last.swap(g_out);
    g_out.clear();
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
void gxfifo_drain_pending() {
    if (!g_buf.empty() && g_buf.size() >= g_need) {
        const size_t used = parse(g_buf.data(), g_buf.size());
        g_buf.erase(g_buf.begin(), g_buf.begin() + used);
    }
}

// The texture currently bound to a texmap, as the game's own display lists described it to the
// hardware. This is the binding the native renderer must use.
SbrTexture sbr_gx_fifo_texture(unsigned texmap) {
    return g_fifoTex[texmap & 7];
}

// The TEV configuration the material display lists have written.
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
}

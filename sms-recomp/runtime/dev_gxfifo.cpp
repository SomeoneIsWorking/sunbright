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
//   0x48 / 0x50 / 0x58      display-list call and friends

#include "mmio.h"

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



// Per-VAT vertex descriptors, enough to compute a vertex's byte size.
struct Vat { u32 vcd_lo = 0, vcd_hi = 0, fmt0 = 0, fmt1 = 0, fmt2 = 0; };
Vat g_vat[8];
u32 g_vcd_lo = 0, g_vcd_hi = 0;

// EFB copy rectangle, tracked from BP 0x49/0x4A so the display copy can be described to
// aurora in its own terms (see emit_copy_state).
u32 g_copy_left = 0, g_copy_top = 0, g_copy_w = 0, g_copy_h = 0;

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
    put_u32(g_out, 0x01800000u - off);   // the array cannot extend past MEM1
    put_u8 (g_out, 0);                   // big-endian, as the guest wrote it
}

// Aurora's CP handles the copy-to-XFB trigger (BP 0x52 bit 14), but only after the copy
// source and destination have been described through its own extensions — the raw BP
// registers carry EFB coordinates and a guest destination address it cannot use directly.
// Without this its present source is never created, and the frame it hands back is 1x1.
void emit_copy_state() {
    if (g_copy_w == 0 || g_copy_h == 0) return;

    put_u8 (g_out, 0x50);
    put_u16(g_out, GX_AURORA_LOAD_COPY_SRC);
    put_u32(g_out, g_copy_left);
    put_u32(g_out, g_copy_top);
    put_u32(g_out, g_copy_w);
    put_u32(g_out, g_copy_h);

    put_u8 (g_out, 0x50);
    put_u16(g_out, GX_AURORA_LOAD_COPY_DST);
    put_u32(g_out, g_copy_w);
    put_u32(g_out, g_copy_h);
    put_u32(g_out, 0);      // GX_TF_I4 slot is unused for a display copy
    put_u8 (g_out, 0);      // not a wide (double-strided) copy
}

// GX_AURORA_LOAD_TEXOBJ payload, from aurora's parser: u8 map, u64 data, u32 w, u32 h,
// u32 fmt, u32 tlut, u8 hasMips, u32 texObjId, u32 texDataVersion.
void emit_texobj(u32 map) {
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

    put_u8 (g_out, 0x50);
    put_u16(g_out, (u16)GX_AURORA_LOAD_TEXOBJ);
    put_u8 (g_out, (u8)(map & 7));
    put_u64(g_out, (u64)(uintptr_t)(g_ram_base + phys));
    put_u32(g_out, w);
    put_u32(g_out, h);
    put_u32(g_out, fmt);
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
            const u32 xfaddr = be16(p + i + 3);
            // TexGen config lives at XF 0x1040-0x104F; log what the guest actually writes so
            // an "unconfigured tcg" in aurora can be attributed to the game or to us.
            if (xfaddr >= 0x1040 && xfaddr <= 0x104F && n - i >= 1 + 4 + 4)
                lucent::debug("gxfifo", "seq XF texgen[{}] srcRow={}",
                              xfaddr - 0x1040, (be32(p + i + 5) >> 7) & 0x1F);
            if (xfaddr == 0x103F && n - i >= 1 + 4 + 4)
                lucent::debug("gxfifo", "seq XF numTexGens = {}", be32(p + i + 5));
            const size_t len = 1 + 4 + count * 4;
            if (n - i < len) { g_need = len; break; }
            g_out.insert(g_out.end(), p + i, p + i + len);
            g_stats.xf++; i += len; continue;
        }

        if (op == 0x61) {                       // BP register write
            if (n - i < 5) { g_need = 5; break; }
            const u8  reg = p[i + 1];
            const u32 val = ((u32)p[i + 2] << 16) | ((u32)p[i + 3] << 8) | p[i + 4];

            // 0x49: EFB copy top-left (10 bits each). 0x4A: width-1 / height-1.
            if (reg == 0x49) { g_copy_left = val & 0x3FF; g_copy_top = (val >> 10) & 0x3FF; }
            else if (reg == 0x00) lucent::debug("gxfifo", "seq BP genMode numTexGens = {}", val & 0xF);
            else if (reg == 0x4A) { g_copy_w = (val & 0x3FF) + 1; g_copy_h = ((val >> 10) & 0x3FF) + 1; }
            // 0x52 bit 14 selects copy-to-XFB, which is what produces the presented frame.
            else if (reg == 0x52 && (val & (1u << 14))) emit_copy_state();
            // Texture image registers. Maps 0-3 use the 0x8x/0x9x ids, maps 4-7 the 0xAx/0xBx
            // ids. A texobj is emitted once both halves of a slot are known.
            else if (reg >= 0x88 && reg <= 0x8B) { u32 m = reg - 0x88; g_tex[m].image0 = val; g_tex[m].have0 = true; emit_texobj(m); }
            else if (reg >= 0xA8 && reg <= 0xAB) { u32 m = reg - 0xA8 + 4; g_tex[m].image0 = val; g_tex[m].have0 = true; emit_texobj(m); }
            else if (reg >= 0x94 && reg <= 0x97) { u32 m = reg - 0x94; g_tex[m].image3 = val; g_tex[m].have3 = true; emit_texobj(m); }
            else if (reg >= 0xB4 && reg <= 0xB7) { u32 m = reg - 0xB4 + 4; g_tex[m].image3 = val; g_tex[m].have3 = true; emit_texobj(m); }

            g_out.insert(g_out.end(), p + i, p + i + 5);
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
            g_out.insert(g_out.end(), p + i, p + i + 5);
            i += 5; continue;
        }

        if (op == 0x48) {                       // call display list
            if (n - i < 9) { g_need = 9; break; }
            const u32 addr = be32(p + i + 1);
            const u32 size = be32(p + i + 5);
            inline_display_list(addr, size, depth);
            i += 9; continue;
        }

        if (op >= 0x80 && op <= 0xBF) {         // draw primitive
            if (n - i < 3) { g_need = 3; break; }
            const u32 verts = be16(p + i + 1);
            const u32 vsize = vertex_size(op & 7);
            const size_t len = 3 + (size_t)verts * vsize;
            if (vsize == 0) break;                  // VAT not seen yet
            if (n - i < len) { g_need = len; break; }
            // Self-check: after a correctly sized draw the next byte must be a plausible
            // opcode. If it is not, THIS draw's vertex size is wrong — report the exact
            // VCD/VAT that produced it rather than letting aurora fail later with a
            // misleading "unsupported primitive" somewhere unrelated.
            if (i + len < n) {
                const u8 nx = p[i + len];
                const bool ok = nx == 0x00 || nx == 0x08 || nx == 0x10 || nx == 0x48 ||
                                nx == 0x61 || (nx >= 0x80 && nx <= 0xBF);
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

// Hand the frame's command stream to aurora. Called once per presented frame.
void gxfifo_flush() {
    // Drain whatever is still buffered before presenting. Parsing only ran once 4096 bytes
    // had accumulated, so a frame's trailing commands could sit unparsed and be emitted in
    // the NEXT frame's stream — aurora would then draw this frame with last frame's state.
    // At a frame boundary the guest has finished writing, so parse what is there.
    while (!g_buf.empty()) {
        const size_t used = parse(g_buf.data(), g_buf.size());
        if (used == 0) break;                       // needs bytes the guest has not written
        g_buf.erase(g_buf.begin(), g_buf.begin() + used);
    }

    if (g_out.empty()) return;
    aurora_fifo_replay(g_out.data(), (u32)g_out.size(), /*bigEndian=*/1);
    g_out.clear();
}

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

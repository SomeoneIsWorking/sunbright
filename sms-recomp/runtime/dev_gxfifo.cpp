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

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr u32 FIFO_BASE = 0xCC008000;

// The stream arrives in pieces of 1/2/4/8 bytes; commands straddle those writes, so it has
// to be reassembled before it can be framed.
std::vector<u8> g_buf;

// Per-VAT vertex descriptors, enough to compute a vertex's byte size.
struct Vat { u32 vcd_lo = 0, vcd_hi = 0, fmt0 = 0, fmt1 = 0, fmt2 = 0; };
Vat g_vat[8];
u32 g_vcd_lo = 0, g_vcd_hi = 0;

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
        const u32 sh = (u32)(i - 5) * 9;
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

    // VAT_A: pos elements bit0 / format bits1-3, normal format bits10-12.
    const u32 pos_cnt = ((v.fmt0 >> 0) & 1) ? 3 : 2;
    n += attr_size(pos_mode, pos_cnt * component_bytes((v.fmt0 >> 1) & 7));
    n += attr_size(nrm_mode, 3 * component_bytes((v.fmt0 >> 10) & 7));

    // VAT_A: col0 elements bit13 / comp bits14-16, col1 elements bit17 / comp bits18-20.
    n += attr_size(c0_mode, colour_bytes((v.fmt0 >> 14) & 7));
    n += attr_size(c1_mode, colour_bytes((v.fmt0 >> 18) & 7));

    // VCD hi: bits0-15 are TexCoord0-7 modes, 2 bits each.
    for (int i = 0; i < 8; i++)
        n += attr_size((v.vcd_hi >> (i * 2)) & 3, texcoord_bytes(v, i));
    return n;
}

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u32 be16(const u8* p) { return (u32)p[0] << 8 | p[1]; }

// Frame as much of the buffer as is complete. Returns bytes consumed.
size_t parse(const u8* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        const u8 op = p[i];

        if (op == 0x00) { g_stats.nops++; i += 1; continue; }

        if (op == 0x08) {                       // CP register write
            if (n - i < 6) break;
            const u8  reg = p[i + 1];
            const u32 val = be32(p + i + 2);
            if (reg == 0x50) { g_vcd_lo = val; for (auto& v : g_vat) v.vcd_lo = val; }
            else if (reg == 0x60) { g_vcd_hi = val; for (auto& v : g_vat) v.vcd_hi = val; }
            else if (reg >= 0x70 && reg <= 0x77) g_vat[reg - 0x70].fmt0 = val;
            else if (reg >= 0x80 && reg <= 0x87) g_vat[reg - 0x80].fmt1 = val;
            else if (reg >= 0x90 && reg <= 0x97) g_vat[reg - 0x90].fmt2 = val;
            g_stats.cp++; i += 6; continue;
        }

        if (op == 0x10) {                       // XF write
            if (n - i < 5) break;
            const u32 count = ((be16(p + i + 1)) & 0xFFFF) + 1;
            const size_t len = 1 + 4 + count * 4;
            if (n - i < len) break;
            g_stats.xf++; i += len; continue;
        }

        if (op == 0x61) {                       // BP register write
            if (n - i < 5) break;
            g_stats.bp++; i += 5; continue;
        }

        if (op >= 0x80 && op <= 0xBF) {         // draw primitive
            if (n - i < 3) break;
            const u32 verts = be16(p + i + 1);
            const u32 vsize = vertex_size(op & 7);
            const size_t len = 3 + (size_t)verts * vsize;
            if (vsize == 0 || n - i < len) break;   // wait for more, or VAT not seen yet
            g_stats.draws++; g_stats.verts += verts;
            i += len; continue;
        }

        // Anything else means the stream framing is wrong; stop rather than resync blindly
        // on data that would look like opcodes.
        g_stats.unknown++;
        lucent::debug("gxfifo", "unrecognised opcode 0x{:02x} — framing lost", op);
        return n;   // drop the rest of this batch
    }
    return i;
}

void fifo_write(u32 ea, unsigned width, u32 value) {
    (void)ea;
    u8 tmp[4];
    for (unsigned k = 0; k < width; k++) tmp[k] = (u8)(value >> (8 * (width - 1 - k)));
    g_buf.insert(g_buf.end(), tmp, tmp + width);
    g_stats.bytes += width;

    if (g_buf.size() >= 4096) {
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

void gxfifo_stats(u64& draws, u64& verts, u64& bytes) {
    draws = g_stats.draws; verts = g_stats.verts; bytes = g_stats.bytes;
}

void gxfifo_device_init() {
    g_buf.reserve(1 << 16);
    // The gather pipe is a single address the CPU stores to repeatedly; the block is
    // 0xCC008000-0xCC008020 on hardware.
    mmio_register(MmioDevice{FIFO_BASE, FIFO_BASE + 0x20, "gxfifo", &fifo_read, &fifo_write});
}

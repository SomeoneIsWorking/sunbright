// ngx_decode — see ngx_decode.h. Dolphin-free GX GP-FIFO command decoder.
//
// Every constant here is the GameCube GX spec (cross-checked against
// externals/dolphin VideoCommon at implementation time):
//   framing  : OpcodeDecoding.h  detail::RunCommand
//   CP regs  : CPMemory.{h,cpp}  LoadCPReg + VCD/VAT bitfields
//   vtx size : VertexLoader_{Position,Normal,Color,TextCoord}.h size tables
//   BP ids   : BPMemory.h  (PE_TOKEN 0x47, PE_TOKEN_INT 0x48, TRIGGER_EFB_COPY 0x52)
// No Dolphin headers are pulled in; this is a clean-room native reimplementation
// validated by lockstep parity against the Dolphin-based oracle (gxp_parse_frame).

#include "ngx_decode.h"

namespace {

// Big-endian field reads (gather-pipe bytes are written big-endian by the guest).
inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
inline u32 be24(const u8* p) { return (u32(p[0]) << 16) | (u32(p[1]) << 8) | p[2]; }
inline u16 be16(const u8* p) { return u16((u32(p[0]) << 8) | p[1]); }

// Bytes per component for a ComponentFormat (UByte0/Byte1=1, UShort2/Short3=2,
// Float4 and invalid-floats 5/6/7=4). Matches the per-format element size baked
// into Dolphin's size tables.
inline u32 elem_bytes(u32 fmt) {
    switch (fmt) {
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    default:        return 4;   // Float + InvalidFloat5/6/7
    }
}

// VertexComponentFormat class: NotPresent=0, Direct=1, Index8=2, Index16=3.
inline u32 pos_size(u32 cls, u32 fmt, u32 elem /*0=XY,1=XYZ*/) {
    switch (cls) {
    case 1: return elem_bytes(fmt) * (elem ? 3u : 2u);  // Direct
    case 2: return 1;                                    // Index8
    case 3: return 2;                                    // Index16
    default: return 0;                                   // NotPresent
    }
}
inline u32 tex_size(u32 cls, u32 fmt, u32 elem /*0=S,1=ST*/) {
    switch (cls) {
    case 1: return elem_bytes(fmt) * (elem ? 2u : 1u);  // Direct
    case 2: return 1;
    case 3: return 2;
    default: return 0;
    }
}
inline u32 color_size(u32 cls, u32 fmt /*ColorFormat 0..5*/) {
    static const u32 direct[6] = {2, 3, 4, 2, 3, 4};    // RGB565,RGB888,RGB888x,RGBA4444,RGBA6666,RGBA8888
    if (fmt > 5) return 0;                              // oracle returns 0 for invalid color format
    switch (cls) {
    case 1: return direct[fmt];                         // Direct
    case 2: return 1;
    case 3: return 2;
    default: return 0;
    }
}
inline u32 normal_size(u32 cls, u32 idx3, u32 ntb /*0=N,1=NTB*/, u32 fmt) {
    switch (cls) {
    case 1: return elem_bytes(fmt) * (ntb ? 9u : 3u);          // Direct (idx3 irrelevant)
    case 2: return (idx3 && ntb) ? 3u : 1u;                    // Index8  (3 indices when NTB+idx3)
    case 3: return (idx3 && ntb) ? 6u : 2u;                    // Index16
    default: return 0;                                         // NotPresent
    }
}

// Per-index tex coord format/elements, threading g0/g1/g2 exactly as
// VAT::GetTexFormat / GetTexElements do.
inline u32 tex_fmt(const NgxCP& cp, unsigned vat, int i) {
    switch (i) {
    case 0: return (cp.vat[vat][0] >> 22) & 7;
    case 1: return (cp.vat[vat][1] >> 1)  & 7;
    case 2: return (cp.vat[vat][1] >> 10) & 7;
    case 3: return (cp.vat[vat][1] >> 19) & 7;
    case 4: return (cp.vat[vat][1] >> 28) & 7;
    case 5: return (cp.vat[vat][2] >> 6)  & 7;
    case 6: return (cp.vat[vat][2] >> 15) & 7;
    default:return (cp.vat[vat][2] >> 24) & 7;   // 7
    }
}
inline u32 tex_elem(const NgxCP& cp, unsigned vat, int i) {
    switch (i) {
    case 0: return (cp.vat[vat][0] >> 21) & 1;
    case 1: return (cp.vat[vat][1] >> 0)  & 1;
    case 2: return (cp.vat[vat][1] >> 9)  & 1;
    case 3: return (cp.vat[vat][1] >> 18) & 1;
    case 4: return (cp.vat[vat][1] >> 27) & 1;
    case 5: return (cp.vat[vat][2] >> 5)  & 1;
    case 6: return (cp.vat[vat][2] >> 14) & 1;
    default:return (cp.vat[vat][2] >> 23) & 1;   // 7
    }
}

// Apply a CP register load (the fields that affect parsing/vertex size + the
// vertex-array base/stride state the native mesh builder resolves).
void load_cp(NgxCP& cp, u8 sub, u32 val) {
    if (sub >= 0xA0 && sub <= 0xAF) { cp.array_base[sub - 0xA0] = val; return; }   // ARRAY_BASE
    if (sub >= 0xB0 && sub <= 0xBF) { cp.array_stride[sub - 0xB0] = val; return; } // ARRAY_STRIDE
    switch (sub & 0xF0) {
    case 0x50: cp.vcd_lo = val; break;          // VCD_LO
    case 0x60: cp.vcd_hi = val; break;          // VCD_HI
    case 0x70: cp.vat[sub & 7][0] = val; break;  // VAT_A
    case 0x80: cp.vat[sub & 7][1] = val; break;  // VAT_B
    case 0x90: cp.vat[sub & 7][2] = val; break;  // VAT_C
    default: break;  // MATINDEX_A/B, unknowns: no effect on framing
    }
}

NgxCP g_ngx_cp;   // persistent across frames; guest threads are nthr-serialized

// Decode one command. Returns its byte size, 0 on truncation (need more bytes),
// sets `unknown` on a fatal unknown opcode (framing failure). Mirrors
// OpcodeDecoder::detail::RunCommand + the Analyzer callbacks.
typedef void (*PrimCb)(const NgxPrim&, void*);

u32 run_command(const u8* d, u32 avail, NgxCP& cp, GxFrameInfo& out, u32 offset, bool& unknown,
                PrimCb cb, void* user) {
    if (avail < 1) return 0;
    const u8 op = d[0];

    if (op == 0x00) {  // NOP (merged run, like the oracle)
        u32 c = 1;
        while (c < avail && d[c] == 0x00) c++;
        return c;
    }
    if (op == 0x08) {  // LOAD_CP_REG : u8 reg, u32 value
        if (avail < 6) return 0;
        const u8 reg = d[1];
        const u32 val = be32(d + 2);
        load_cp(cp, reg, val);
        if (reg >= 0xA0 && reg <= 0xAF) {
            const u32 array = reg - 0xA0;
            if (array == 12 || array == 13)  // XF_A pos / XF_B nrm matrix arrays
                out.mtx_arrays.push_back({offset, array, val});
        }
        return 6;
    }
    if (op == 0x10) {  // LOAD_XF_REG : u32 cmd2, then transfer_size*4 data bytes
        if (avail < 5) return 0;
        const u32 cmd2 = be32(d + 1);
        const u32 ss = ((cmd2 >> 16) & 0xF) + 1;
        if (avail < 5 + ss * 4) return 0;
        return 5 + ss * 4;
    }
    if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {  // LOAD_INDX_A/B/C/D
        if (avail < 5) return 0;
        return 5;
    }
    if (op == 0x40) {  // CALL_DL : u32 addr, u32 size  (counted, NOT recursed — matches oracle)
        if (avail < 9) return 0;
        out.display_lists++;
        return 9;
    }
    if (op == 0x61) {  // LOAD_BP_REG : u8 reg, u24 value
        if (avail < 5) return 0;
        const u8 reg = d[1];
        if (reg == 0x47 || reg == 0x48) out.token_offsets.push_back(offset);  // PE token / token-int
        if (reg == 0x52) out.copies++;                                        // TRIGGER_EFB_COPY
        return 5;
    }
    if (op >= 0x80 && op <= 0xBF) {  // DRAW PRIMITIVE
        if (avail < 3) return 0;
        const u32 vat = op & 0x07;
        const u32 vsize = ngx_vertex_size(cp, vat);
        const u16 nv = be16(d + 1);
        if (avail < 3 + (u32)nv * vsize) return 0;
        out.prims++;
        if (cb) { NgxPrim pr{op, (int)nv, d + 3}; cb(pr, user); }
        return 3 + (u32)nv * vsize;
    }

    // 0x44 (unknown-metrics) and 0x48 (invalidate vertex cache) keep the stream
    // in sync (1 byte, non-fatal); anything else is a framing failure.
    if (op != 0x44 && op != 0x48) unknown = true;
    return 1;
}

}  // namespace

u32 ngx_attr_offset(const NgxCP& cp, unsigned vat, int attr) {
    // Matrix-index bytes (PosMatIdx + 8 TexMatIdx) precede the position.
    u32 off = (u32)__builtin_popcount(cp.vcd_lo & 0x1FF);
    if (attr <= NGX_POS) return off;

    // Position
    off += pos_size((cp.vcd_lo >> 9) & 3, (cp.vat[vat][0] >> 1) & 7, (cp.vat[vat][0] >> 0) & 1);
    if (attr == NGX_NRM) return off;
    // Normal
    off += normal_size((cp.vcd_lo >> 11) & 3, (cp.vat[vat][0] >> 31) & 1,
                       (cp.vat[vat][0] >> 9) & 1, (cp.vat[vat][0] >> 10) & 7);
    if (attr == NGX_CLR0) return off;
    // Color0
    off += color_size((cp.vcd_lo >> 13) & 3, (cp.vat[vat][0] >> 14) & 7);
    if (attr == NGX_CLR1) return off;
    // Color1
    off += color_size((cp.vcd_lo >> 15) & 3, (cp.vat[vat][0] >> 18) & 7);
    // Tex coords 0..7
    for (int i = 0; i < 8; i++) {
        if (attr == NGX_TEX0 + i) return off;
        off += tex_size((cp.vcd_hi >> (2 * i)) & 3, tex_fmt(cp, vat, i), tex_elem(cp, vat, i));
    }
    return off;  // NGX_ATTR_END == full vertex size
}

u32 ngx_vertex_size(const NgxCP& cp, unsigned vat) {
    return ngx_attr_offset(cp, vat, NGX_ATTR_END);
}

NgxCP& ngx_cp_state() { return g_ngx_cp; }

bool ngx_parse_frame(const u8* p, size_t n, GxFrameInfo& out) {
    out = GxFrameInfo{};
    bool unknown = false;
    u32 pos = 0;
    while (pos < n) {
        const u32 sz = run_command(p + pos, (u32)(n - pos), g_ngx_cp, out, pos, unknown, nullptr, nullptr);
        if (sz == 0 || unknown) break;  // truncated or unknown opcode
        pos += sz;
    }
    out.ok = (pos == n) && !unknown;
    out.total = (u32)n;
    if (!out.ok) { out.fail_offset = pos; out.fail_opcode = pos < n ? p[pos] : 0; }
    return out.ok;
}

bool ngx_walk_stream(const u8* p, size_t n, NgxCP& cp,
                     void (*on_prim)(const NgxPrim&, void*), void* user) {
    GxFrameInfo scratch{};   // forensics discarded; we only want the prim callback
    bool unknown = false;
    u32 pos = 0;
    while (pos < n) {
        const u32 sz = run_command(p + pos, (u32)(n - pos), cp, scratch, pos, unknown,
                                   (PrimCb)on_prim, user);
        if (sz == 0 || unknown) break;
        pos += sz;
    }
    return (pos == n) && !unknown;
}

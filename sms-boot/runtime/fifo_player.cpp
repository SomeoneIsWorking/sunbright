// fifo_player.cpp — Dolphin .dff FIFO replay loader + driver (parity harness).
//
// Diagnostic-only (SB_FIFO_REPLAY). See fifo_player.h. The loader is a faithful
// C++ port of the read side of tools/oracle/parse_fifo_dff.py.

#include "fifo_player.h"

#include <aurora/aurora.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace sb {

namespace {
constexpr std::uint32_t kDffFileId = 0x0D01F1F0;

// Opcode constants (dolphin/gx/GXCommandList.h + GXAurora.h). Defined locally
// to keep the translator self-contained (aurora.h does not pull in the GX
// headers). These are stable HW constants.
constexpr std::uint8_t GX_NOP          = 0x00;
constexpr std::uint8_t GX_LOAD_CP_REG  = 0x08;
constexpr std::uint8_t GX_LOAD_XF_REG  = 0x10;
constexpr std::uint8_t GX_CMD_CALL_DL  = 0x40;
constexpr std::uint8_t GX_LOAD_BP_REG  = 0x61;
constexpr std::uint8_t GX_AURORA       = 0x50;
constexpr std::uint8_t GX_OPCODE_MASK  = 0xF8;
constexpr std::uint8_t GX_VAT_MASK     = 0x07;
constexpr std::uint16_t GX_AURORA_LOAD_ARRAYBASE = 0x0010;
// CP array-base register addresses (aurora ignores the raw 32-bit value; the
// translator must synthesize GX_AURORA_LOAD_ARRAYBASE with a host pointer).
constexpr std::uint8_t CP_REG_ARRAYBASE_LO = 0xA0;
constexpr std::uint8_t CP_REG_ARRAYBASE_HI = 0xAF;

void fail(const char* why) {
    std::fprintf(stderr, "[fifo_player] FATAL: %s\n", why);
    std::abort();  // fail-fast per project policy
}
} // namespace

FifoCapture load_dff(const std::string& path) {
    FifoCapture cap;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) fail(("cannot open " + path).c_str());
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < (long)sizeof(DffFileHeader)) fail(".dff smaller than header");
    cap.fileData.resize(sz);
    if (std::fread(cap.fileData.data(), 1, sz, f) != (size_t)sz) fail("short read");
    std::fclose(f);

    const auto* h = reinterpret_cast<const DffFileHeader*>(cap.fileData.data());
    if (h->fileId != kDffFileId) fail("bad .dff magic");
    cap.header = h;

    // File-wide memory snapshots (borrowed slices; fileData outlives them).
    auto slice = [&](std::uint64_t off, std::uint32_t s) -> const std::uint8_t* {
        if (off + s > cap.fileData.size()) fail("memory snapshot out of bounds");
        return cap.fileData.data() + off;
    };
    cap.bpMem  = slice(h->bpMemOffset, h->bpMemSize);   cap.bpMemSize  = h->bpMemSize;
    cap.cpMem  = slice(h->cpMemOffset, h->cpMemSize);   cap.cpMemSize  = h->cpMemSize;
    cap.xfMem  = slice(h->xfMemOffset, h->xfMemSize);   cap.xfMemSize  = h->xfMemSize;
    cap.xfRegs = slice(h->xfRegsOffset, h->xfRegsSize); cap.xfRegsSize = h->xfRegsSize;
    cap.texMem = slice(h->texMemOffset, h->texMemSize); cap.texMemSize = h->texMemSize;

    // Frames + their memory updates.
    if (h->frameListOffset + (std::uint64_t)h->frameCount * sizeof(DffFrameInfo) > cap.fileData.size())
        fail("frame list out of bounds");
    cap.frames.resize(h->frameCount);
    for (std::uint32_t i = 0; i < h->frameCount; ++i) {
        const auto* fi = reinterpret_cast<const DffFrameInfo*>(
            cap.fileData.data() + h->frameListOffset + i * sizeof(DffFrameInfo));
        FifoFrame& frame = cap.frames[i];
        frame.fifoStart = fi->fifoStart;
        frame.fifoEnd   = fi->fifoEnd;
        if (fi->fifoDataOffset + fi->fifoDataSize > cap.fileData.size()) fail("fifoData out of bounds");
        frame.fifoData.assign(cap.fileData.data() + fi->fifoDataOffset,
                              cap.fileData.data() + fi->fifoDataOffset + fi->fifoDataSize);

        if (fi->numMemoryUpdates) {
            if (fi->memoryUpdatesOffset + (std::uint64_t)fi->numMemoryUpdates * sizeof(DffMemoryUpdate)
                > cap.fileData.size())
                fail("memory update list out of bounds");
            frame.memoryUpdates.reserve(fi->numMemoryUpdates);
            for (std::uint32_t j = 0; j < fi->numMemoryUpdates; ++j) {
                const auto* mu = reinterpret_cast<const DffMemoryUpdate*>(
                    cap.fileData.data() + fi->memoryUpdatesOffset + j * sizeof(DffMemoryUpdate));
                MemoryUpdate upd;
                upd.fifoPosition = mu->fifoPosition;
                upd.address      = mu->address;
                upd.type         = mu->type;
                if (mu->dataOffset + mu->dataSize > cap.fileData.size()) fail("memupdate data oob");
                upd.data.assign(cap.fileData.data() + mu->dataOffset,
                                cap.fileData.data() + mu->dataOffset + mu->dataSize);
                frame.memoryUpdates.push_back(std::move(upd));
            }
            // Memory updates must be sorted by fifoPosition for interleaved replay.
            std::sort(frame.memoryUpdates.begin(), frame.memoryUpdates.end(),
                      [](const MemoryUpdate& a, const MemoryUpdate& b) {
                          return a.fifoPosition < b.fifoPosition;
                      });
        }
    }
    return cap;
}

void print_summary(const FifoCapture& cap) {
    const auto* h = cap.header;
    std::printf("[fifo_player] frames=%u bpMem=%u cpMem=%u xfMem=%u xfRegs=%u texMem=%u\n",
                h->frameCount, h->bpMemSize, h->cpMemSize, h->xfMemSize, h->xfRegsSize, h->texMemSize);
    for (std::uint32_t i = 0; i < cap.frames.size(); ++i) {
        const auto& fr = cap.frames[i];
        int nv = 0, nt = 0, nx = 0;
        for (const auto& mu : fr.memoryUpdates) {
            if (mu.type == kMemUpdateVertexStream) ++nv;
            else if (mu.type == kMemUpdateTextureMap) ++nt;
            else if (mu.type == kMemUpdateXFData) ++nx;
        }
        std::printf("  frame %u: %u cmd bytes, %zu memupdates (vtx=%d tex=%d xf=%d)\n",
                    i, (unsigned)fr.fifoData.size(), fr.memoryUpdates.size(), nv, nt, nx);
    }
}

// ---- CP/VAT vertex-size state (port of parse_fifo_dff.py CPState) ----
//
// To walk the command stream in sync we must size each primitive's vertex
// payload. That requires tracking VCD_LO/VCD_HI + the 8 VAT triplets exactly
// as the known-good Python parser does (VideoCommon/CPMemory +
// VertexLoaderBase::GetVertexSize). Byte values 0x08/0x10/0x20/0x40 occur
// INSIDE vertex streams, so a naive forward-scan walker misreads them as
// opcodes; this state machine is the only correct way.

namespace {

// VertexLoader size tables (1:1 from parse_fifo_dff.py, originally from Dolphin
// VideoCommon/VertexLoader_{Position,Normal,Color,TextCoord}).
// POS_SIZE[type][format] = (size_XY, size_XYZ)
const int POS_SIZE[4][8][2] = {
    {},  // type 0 (NotPresent) unused
    {{2,3},{2,3},{4,6},{4,6},{8,12},{8,12},{8,12},{8,12}},
    {{1,1},{1,1},{1,1},{1,1},{1,1},{1,1},{1,1},{1,1}},
    {{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2}},
};
// NORMAL_SIZE_DIRECT[index3][elements][format]
const int NRM_DIRECT[2][2][8] = {
  { {3,3,6,6,12,12,12,12}, {9,9,18,18,36,36,36,36} },
  { {3,3,6,6,12,12,12,12}, {9,9,18,18,36,36,36,36} },
};
const int NRM_IDX8[2][2]  = {{1,1},{1,3}};
const int NRM_IDX16[2][2] = {{2,2},{2,6}};
// COLOR_SIZE[type][ColorFormat 0..5]
const int COLOR_SIZE[4][6] = {
    {}, {2,3,4,2,3,4}, {1,1,1,1,1,1}, {2,2,2,2,2,2},
};
// TEX_SIZE[type][format] = (size_S, size_ST)
const int TEX_SIZE[4][8][2] = {
    {},
    {{1,2},{1,2},{2,4},{2,4},{4,8},{4,8},{4,8},{4,8}},
    {{1,1},{1,1},{1,1},{1,1},{1,1},{1,1},{1,1},{1,1}},
    {{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2},{2,2}},
};

inline int popcount9(std::uint32_t v) {  // low 9 bits
    return __builtin_popcount(v & 0x1FF);
}
inline int bits(std::uint32_t v, int lo, int w) {
    return (v >> lo) & ((1u << w) - 1u);
}

struct VAT {
    std::uint32_t g0=0, g1=0, g2=0;
    int posElements() const { return bits(g0, 0, 1); }
    int posFormat()   const { return bits(g0, 1, 3); }
    int normalElements() const { return bits(g0, 9, 1); }   // 0=N, 1=NTB
    int normalFormat()   const { return bits(g0, 10, 3); }
    int normalIndex3()   const { return bits(g0, 31, 1); }
    int colorElements(int i) const { return bits(g0, i==0?13:17, 1); }
    int colorFormat(int i)   const { return bits(g0, i==0?14:18, 3); }
    int texElements(int i) const {
        switch (i) {
        case 0: return bits(g0, 21, 1);
        case 1: return bits(g1, 0, 1);
        case 2: return bits(g1, 9, 1);
        case 3: return bits(g1, 18, 1);
        case 4: return bits(g1, 27, 1);
        case 5: return bits(g2, 5, 1);
        case 6: return bits(g2, 14, 1);
        case 7: return bits(g2, 23, 1);
        }
        return 0;
    }
    int texFormat(int i) const {
        switch (i) {
        case 0: return bits(g0, 22, 3);
        case 1: return bits(g1, 1, 3);
        case 2: return bits(g1, 10, 3);
        case 3: return bits(g1, 19, 3);
        case 4: return bits(g1, 28, 3);
        case 5: return bits(g2, 6, 3);
        case 6: return bits(g2, 15, 3);
        case 7: return bits(g2, 24, 3);
        }
        return 0;
    }
};

struct CpState {
    std::uint32_t vcdLo=0, vcdHi=0;
    VAT vats[8];
    int vertexSize(int vatIdx) const {
        const VAT& v = vats[vatIdx];
        int size = popcount9(vcdLo);  // PosMatIdx + up to 8 TexMatIdx bytes
        int posType = bits(vcdLo, 9, 2);
        if (posType) {
            size += POS_SIZE[posType][v.posFormat()][v.posElements()];
        }
        int nrmType = bits(vcdLo, 11, 2);
        if (nrmType) {
            int fmt = v.normalFormat();
            int elem = v.normalElements();
            int idx3 = v.normalIndex3();
            if (nrmType == 1) size += NRM_DIRECT[idx3][elem][fmt];
            else if (nrmType == 2) size += NRM_IDX8[idx3][elem];
            else size += NRM_IDX16[idx3][elem];
        }
        for (int i = 0; i < 2; ++i) {
            int ct = bits(vcdLo, 13 + i*2, 2);
            if (ct) {
                int fmt = v.colorFormat(i);
                size += COLOR_SIZE[ct][fmt < 6 ? fmt : 5];
            }
        }
        for (int i = 0; i < 8; ++i) {
            int tt = bits(vcdHi, i*2, 2);
            if (tt) {
                size += TEX_SIZE[tt][v.texFormat(i)][v.texElements(i)];
            }
        }
        return size;
    }
};

// ---- GC-RAM shadow: a flat backing buffer covering GC MEM1 ----
//
// Aurora reads vertex/matrix data from 64-bit HOST pointers. The .dff delivers
// the same data as GC-RAM-addressed memory updates (VertexStream for vertex
// attrs, XFData for the pos/normal/tex/light matrix arrays). We build ONE flat
// shadow buffer covering the full GC MEM1 arena, copy each update's bytes into
// it, and synthesize GX_AURORA_LOAD_ARRAYBASE host pointers that point into the
// shadow at the GC address directly (shadow[gcAddr]). This is the bridge between
// Dolphin's RAM-addressed FIFO model and aurora's host-pointer model.
//
// The shadow covers all of MEM1 (24 MiB) so array bases that point to static
// data predating the capture (not in any update) resolve to valid zeroed memory
// rather than a stale/null pointer — and the buffer never reallocates, so host
// pointers embedded in the translated stream stay valid for the whole replay.

struct GcShadow {
    static constexpr std::uint32_t kMem1Base = 0x00000000u;
    static constexpr std::uint32_t kMem1Size = 0x01800000u;  // 24 MiB
    std::vector<std::uint8_t> buf;
    void init() { buf.assign(kMem1Size, 0); }
    void write(std::uint32_t gcAddr, const std::uint8_t* d, std::size_t n) {
        if (gcAddr + n > kMem1Size) return;  // outside MEM1
        std::memcpy(buf.data() + gcAddr, d, n);
    }
    std::uint8_t* host(std::uint32_t gcAddr) { return buf.data() + gcAddr; }
};

// Emit a GX_AURORA_LOAD_ARRAYBASE (0x50 00 1x) into the output stream.
// Format (command_processor.cpp:2859): u8 op=0x50, u16 sub=0x0010|cpIdx (BE),
// u64 hostPtr (BE), u32 size (BE), u8 le-flag.
void emitArrayBase(std::vector<std::uint8_t>& out, int cpIdx,
                   std::uint64_t hostPtr, std::uint32_t size, bool le) {
    auto pushBE16 = [&](std::uint16_t v) {
        out.push_back((v >> 8) & 0xFF); out.push_back(v & 0xFF);
    };
    auto pushBE64 = [&](std::uint64_t v) {
        for (int i = 7; i >= 0; --i) out.push_back((v >> (i*8)) & 0xFF);
    };
    auto pushBE32 = [&](std::uint32_t v) {
        for (int i = 3; i >= 0; --i) out.push_back((v >> (i*8)) & 0xFF);
    };
    out.push_back(GX_AURORA);
    pushBE16(std::uint16_t(GX_AURORA_LOAD_ARRAYBASE | (cpIdx & 0x0F)));
    pushBE64(hostPtr);
    pushBE32(size);
    out.push_back(le ? 1 : 0);
}
} // namespace

// ---- replay ----

namespace {

// Big-endian readers (the .dff command stream is GC-native big-endian).
std::uint32_t rd_be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8)  | std::uint32_t(p[3]);
}

// Build the translated command stream for one frame. Walks the .dff's command
// bytes with full VCD/VAT vertex-size tracking (so vertex data isn't misread
// as opcodes), and translates the two aurora-incompatible constructs:
//   (1) LOAD_CP_REG 0xA0-0xAF (array base, GC ptr) -> GX_AURORA_LOAD_ARRAYBASE
//       pointing into the GC-RAM shadow (vertex attrs 0-11 host-endian, matrix
//       arrays 12-15 little-endian per J3D convention).
//   (2) Memory updates interleaved at their fifoPosition -> writes into shadow.
// BP/LOAD_XF_REG/LOAD_CP_REG(non-arraybase)/primitives/LOAD_INDX pass through
// verbatim (HW-standard; aurora decodes them).
std::vector<std::uint8_t> translate_frame(const FifoFrame& frame, GcShadow& shadow) {
    std::vector<std::uint8_t> out;
    out.reserve(frame.fifoData.size() + frame.memoryUpdates.size() * 16);
    std::size_t muIdx = 0;
    const std::uint8_t* src = frame.fifoData.data();
    std::uint32_t pos = 0;
    const std::uint32_t end = (std::uint32_t)frame.fifoData.size();

    CpState cp;
    // Current GC array-base address per attr slot (0-15 = VA_POS..LIGHT_ARRAY).
    // Populated from LOAD_CP_REG 0xA0-0xAF; consumed when synthesizing
    // GX_AURORA_LOAD_ARRAYBASE. attr 12-15 = POS/NRM/TEX/LIGHT matrix arrays.
    std::uint32_t attrBase[16] = {};

    while (pos < end) {
        // Apply memory updates scheduled at-or-before this position.
        while (muIdx < frame.memoryUpdates.size() &&
               frame.memoryUpdates[muIdx].fifoPosition <= pos) {
            const MemoryUpdate& mu = frame.memoryUpdates[muIdx];
            // VertexStream + XFData both land in the shadow (vertex attrs + matrix
            // arrays). TextureMap/TMEM handled in a later layer.
            if (mu.type == kMemUpdateVertexStream || mu.type == kMemUpdateXFData) {
                shadow.write(mu.address, mu.data.data(), mu.data.size());
            }
            ++muIdx;
        }

        std::uint8_t op = src[pos];

        if (op == GX_NOP) {
            std::uint32_t r = 0;
            while (pos + r < end && src[pos + r] == GX_NOP) ++r;
            out.insert(out.end(), src + pos, src + pos + r);
            pos += r;
            continue;
        }
        if (op == GX_LOAD_CP_REG) {
            if (pos + 6 > end) break;
            std::uint8_t addr = src[pos + 1];
            std::uint32_t val = rd_be32(src + pos + 2);
            // Mirror CP state for vertex-size computation.
            int sub = addr & 0xF0;
            int vatIdx = addr & 0x07;
            if (sub == 0x50) cp.vcdLo = val;
            else if (sub == 0x60) cp.vcdHi = val;
            else if (sub == 0x70) cp.vats[vatIdx].g0 = val;
            else if (sub == 0x80) cp.vats[vatIdx].g1 = val;
            else if (sub == 0x90) cp.vats[vatIdx].g2 = val;
            // Array base (0xA0-0xAF): translate to GX_AURORA_LOAD_ARRAYBASE.
            if (addr >= CP_REG_ARRAYBASE_LO && addr <= CP_REG_ARRAYBASE_HI) {
                int attrIdx = addr - CP_REG_ARRAYBASE_LO;
                attrBase[attrIdx] = val;
                // The shadow covers all of MEM1, so any nonzero base resolves to
                // a valid host pointer (zeroed if the data isn't in an update).
                // Matrix arrays (attr 12-15) are host-computed (little-endian);
                // vertex attrs (0-11) are GC-native (big-endian -> le=false).
                bool le = (attrIdx >= 12);
                if (val == 0) {
                    emitArrayBase(out, attrIdx, 0, 0, le);  // null/empty slot
                } else {
                    std::uint64_t hostPtr = reinterpret_cast<std::uint64_t>(shadow.host(val));
                    // size=0 tells aurora to use array.sizeAuto (the max referenced
                    // index, maintained during draw_prim) — this avoids uploading the
                    // entire MEM1 tail as array data. The .dff doesn't carry per-array
                    // allocation sizes; sizeAuto is the correct extent.
                    emitArrayBase(out, attrIdx, hostPtr, 0, le);
                }
                pos += 6;
                continue;  // consumed: do NOT emit the raw CP_REG (aurora ignores it)
            }
            // All other CP regs pass through verbatim.
            out.insert(out.end(), src + pos, src + pos + 6);
            pos += 6;
            continue;
        }
        if (op == GX_LOAD_XF_REG) {
            if (pos + 5 > end) break;
            std::uint32_t cmd = rd_be32(src + pos + 1);
            std::uint32_t count = (cmd >> 16) + 1;
            std::uint32_t adv = 5 + count * 4;
            out.insert(out.end(), src + pos, src + pos + std::min(adv, end - pos));
            pos += adv;
            continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            // LOAD_INDX_A/B/C/D: pass through (aurora reads from the array base
            // we synthesized). Sizes the matrix fetch correctly.
            out.insert(out.end(), src + pos, src + pos + 5);
            pos += 5;
            continue;
        }
        if (op == GX_CMD_CALL_DL) {
            // Absent in SMS title (audited via known-good parser; naive walkers
            // misread vertex bytes as CALL_DL). If one truly appears, emit verbatim.
            out.insert(out.end(), src + pos, src + pos + std::min(9u, end - pos));
            pos += 9;
            continue;
        }
        if (op == 0x44 || op == 0x48) {  // metrics / invl_vc
            out.push_back(op);
            pos += 1;
            continue;
        }
        if (op == GX_LOAD_BP_REG) {
            out.insert(out.end(), src + pos, src + pos + std::min(5u, end - pos));
            pos += 5;
            continue;
        }
        if ((op & GX_OPCODE_MASK) >= 0x80) {
            // Primitive: 3-byte header + numVerts * vertexSize(vat).
            int vat = op & GX_VAT_MASK;
            std::uint16_t nverts = (std::uint16_t)((src[pos+1] << 8) | src[pos+2]);
            int vsz = cp.vertexSize(vat);
            std::uint32_t adv = 3 + (std::uint32_t)nverts * vsz;
            out.insert(out.end(), src + pos, src + pos + std::min(adv, end - pos));
            pos += adv;
            continue;
        }
        // Unknown: emit 1 byte and hope (shouldn't happen — 0 unknown opcodes audited).
        std::fprintf(stderr, "[fifo_player] WARN: unknown opcode 0x%02x @ %u\n", op, pos);
        out.push_back(op);
        pos += 1;
    }
    // Flush any trailing memory updates (their data already landed in the shadow;
    // the array-base opcs above already point at the shadow so this is harmless).
    return out;
}
} // namespace

int replay(const FifoCapture& cap) {
    print_summary(cap);
    std::fflush(stdout);
    int rendered = 0;
    // The GC-RAM shadow spans all of MEM1 (24 MiB) and is shared across frames:
    // frame-to-frame state (matrices, static vertex data) accumulates exactly as
    // it would on real GC hardware. Never reallocates, so host pointers embedded
    // in translated streams stay valid through the replay.
    GcShadow shadow;
    shadow.init();
    for (const auto& frame : cap.frames) {

        std::vector<std::uint8_t> cmds = translate_frame(frame, shadow);
        std::printf("[fifo_player] frame %d: translated %u -> %u bytes\n",
                    rendered, (unsigned)frame.fifoData.size(), (unsigned)cmds.size());
        std::fflush(stdout);
        if (!aurora_begin_frame()) {
            std::fprintf(stderr, "[fifo_player] aurora_begin_frame returned false -- aborting replay\n");
            break;
        }
        std::printf("[fifo_player] frame %d: aurora_fifo_replay %u bytes ...\n",
                    rendered, (unsigned)cmds.size());
        std::fflush(stdout);
        aurora_fifo_replay(cmds.data(), (std::uint32_t)cmds.size(), 1);
        std::printf("[fifo_player] frame %d: aurora_end_frame ...\n", rendered);
        std::fflush(stdout);
        aurora_end_frame();  // drains, renders, and captures via SB_DUMP_FRAME
        std::printf("[fifo_player] frame %d: done\n", rendered);
        std::fflush(stdout);
        ++rendered;
    }
    return rendered;
}

} // namespace sb

// C entry point for main.cpp's SB_FIFO_REPLAY gate.
extern "C" int sb_fifo_replay_run(const char* dffPath) {
    sb::FifoCapture cap = sb::load_dff(dffPath);
    return sb::replay(cap);
}


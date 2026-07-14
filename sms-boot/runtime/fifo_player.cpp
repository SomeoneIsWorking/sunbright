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
constexpr std::uint16_t GX_AURORA_LOAD_TEXOBJ    = 0x0030;
constexpr std::uint16_t GX_AURORA_LOAD_TLUT      = 0x0031;
constexpr std::uint16_t GX_AURORA_LOAD_COPY_SRC  = 0x0035;
constexpr std::uint16_t GX_AURORA_LOAD_COPY_DST  = 0x0036;
constexpr std::uint16_t GX_AURORA_LOAD_COPY_DEST = 0x0037;
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
    // bpMem/cpMem/xfMem/xfRegs are u32 arrays — their header size field is an
    // element count; the byte size is count*4. texMem is a u8 array (byte size).
    auto slice = [&](std::uint64_t off, std::uint32_t bytes) -> const std::uint8_t* {
        if (off + bytes > cap.fileData.size()) fail("memory snapshot out of bounds");
        return cap.fileData.data() + off;
    };
    cap.bpMem  = slice(h->bpMemOffset,  h->bpMemSize  * 4u); cap.bpMemCount  = h->bpMemSize;
    cap.cpMem  = slice(h->cpMemOffset,  h->cpMemSize  * 4u); cap.cpMemCount  = h->cpMemSize;
    cap.xfMem  = slice(h->xfMemOffset,  h->xfMemSize  * 4u); cap.xfMemCount  = h->xfMemSize;
    cap.xfRegs = slice(h->xfRegsOffset, h->xfRegsSize * 4u); cap.xfRegsCount = h->xfRegsSize;
    cap.texMem = slice(h->texMemOffset, h->texMemSize);      cap.texMemSize  = h->texMemSize;

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
        if (gcAddr + n > kMem1Size) {
            // FAIL FAST: a memupdate outside MEM1 means the address decoding
            // is wrong (or the capture targets memory this shadow doesn't
            // model) -- dropping it silently would surface later as
            // inexplicably-stale vertex/texture data.
            std::fprintf(stderr,
                "[fifo_player] FATAL: memory update outside MEM1 shadow: "
                "addr=0x%08X size=%zu (MEM1=%u bytes)\n", gcAddr, n, kMem1Size);
            std::abort();
        }
        std::memcpy(buf.data() + gcAddr, d, n);
    }
    std::uint8_t* host(std::uint32_t gcAddr) { return buf.data() + gcAddr; }
};

// Pre-load the file-wide register snapshots (bpMem/cpMem/xfRegs) as synthesized
// BP/CP/XF load commands. This establishes the GPU state that was live when the
// capture started — clear color, TEV config, VCD/VAT, projection matrices, etc.
// Without this, the first frame renders with default (zero) state and the EFB
// clears/draws come out wrong. Emitted as a prefix to frame 0's translated stream.
//
// bpMem: 256 u32s, bpMem[i] = BP register i's value (24-bit). Skip action regs
//   (0x52 EFB copy trigger, 0x53) — loading those has side effects.
// xfRegs: 88 u32s, XF register memory (projection/viewport matrices etc).
//   Loaded via LOAD_XF_REG to the matching XF address (xfRegs[i] -> addr i).
// cpMem: 256 u32s — VCD/VAT/MatrixIndex. These are also reloaded by the frame
//   stream itself (the stream loads VCD/VAT before each draw batch), so skipping
//   the cpMem pre-load is safe and avoids double-emitting array-base regs.
std::vector<std::uint8_t> preload_state(const FifoCapture& cap) {
    std::vector<std::uint8_t> out;
    auto pushBE24 = [&](std::uint32_t v) {
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back(v & 0xFF);
    };
    auto pushBE32 = [&](std::uint32_t v) {
        for (int i = 3; i >= 0; --i) out.push_back((v >> (i*8)) & 0xFF);
    };
    // The snapshots are Dolphin's in-memory state arrays written raw to the
    // file — HOST-endian (little-endian) u32s, NOT GC big-endian. Verified
    // empirically 2026-07-14: xfRegs[0x20] (XF 0x1020, projection A) reads
    // 0.003125 as LE (plausible) vs -4.3e8 as BE (garbage); bpMem[0x40]
    // (zmode) reads 0x1F as LE (enable+lequal+update) vs 0x1F000000 as BE.
    // (The command STREAM stays big-endian; only these snapshots are LE.)
    auto rdLE32 = [](const std::uint8_t* p) -> std::uint32_t {
        return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
               (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    };

    // BP registers (0x00-0xFF). Skip 0x52/0x53 (action: EFB copy trigger).
    if (cap.bpMem && cap.bpMemCount >= 256) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t val = rdLE32(cap.bpMem + i * 4) & 0x00FFFFFFu;
            if (i == 0x52 || i == 0x53) continue;  // action regs
            if (val == 0) continue;  // skip zero (default) to keep the prefix lean
            out.push_back(GX_LOAD_BP_REG);
            out.push_back(static_cast<std::uint8_t>(i));
            pushBE24(val);
        }
    }
    // XF REGISTERS (Dolphin xfRegs[0..87]) live at XF addresses 0x1000-0x1057
    // — NOT 0x0000 (that's position-matrix memory; loading them there
    // corrupted matrix mem at frame start, caught by aurora's now-always-on
    // PosMtx sub-copy CHECK). One batched LOAD_XF_REG at 0x1000.
    if (cap.xfRegs && cap.xfRegsCount > 0) {
        std::uint32_t cnt = cap.xfRegsCount;
        out.push_back(GX_LOAD_XF_REG);
        pushBE32(((cnt - 1) << 16) | 0x1000);  // ((count-1)<<16) | addr
        for (std::uint32_t i = 0; i < cnt; ++i) {
            pushBE32(rdLE32(cap.xfRegs + i * 4));
        }
    }
    // XF MEMORY (Dolphin xfMem[0..0xFFF]) — matrix/light memory at XF
    // 0x0000-0x0FFF. aurora's copy_xf_data only accepts whole-object writes,
    // so emit per-object chunks matching its region granularity.
    if (cap.xfMem && cap.xfMemCount >= 0x680) {
        auto emitChunk = [&](std::uint32_t xfAddr, std::uint32_t len) {
            out.push_back(GX_LOAD_XF_REG);
            pushBE32(((len - 1) << 16) | xfAddr);
            for (std::uint32_t i = 0; i < len; ++i) {
                pushBE32(rdLE32(cap.xfMem + (xfAddr + i) * 4));
            }
        };
        for (std::uint32_t a = 0x000; a + 12 <= 0x078; a += 12) emitChunk(a, 12);  // pos mtx
        for (std::uint32_t a = 0x078; a + 12 <= 0x0F0; a += 12) emitChunk(a, 12);  // tex mtx
        for (std::uint32_t a = 0x400; a + 9  <= 0x45A; a += 9)  emitChunk(a, 9);   // nrm mtx
        for (std::uint32_t a = 0x500; a + 12 <= 0x5F0; a += 12) emitChunk(a, 12);  // post-tex mtx
        for (std::uint32_t a = 0x600; a + 16 <= 0x680; a += 16) emitChunk(a, 16);  // lights
    }
    return out;
}

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
std::vector<std::uint8_t> translate_frame(const FifoCapture& cap, std::uint32_t frameIdx,
                                          GcShadow& shadow) {
    const FifoFrame& frame = cap.frames[frameIdx];
    std::vector<std::uint8_t> out;
    // Frame 0 gets the register-snapshot pre-load as a prefix.
    if (frameIdx == 0) {
        std::vector<std::uint8_t> pre = preload_state(cap);
        out.insert(out.end(), pre.begin(), pre.end());
    }
    out.reserve(out.size() + frame.fifoData.size() + frame.memoryUpdates.size() * 16);
    std::size_t muIdx = 0;
    const std::uint8_t* src = frame.fifoData.data();
    std::uint32_t pos = 0;
    const std::uint32_t end = (std::uint32_t)frame.fifoData.size();

    CpState cp;
    // Current GC array-base address per attr slot (0-15 = VA_POS..LIGHT_ARRAY).
    // Populated from LOAD_CP_REG 0xA0-0xAF; consumed when synthesizing
    // GX_AURORA_LOAD_ARRAYBASE. attr 12-15 = POS/NRM/TEX/LIGHT matrix arrays.
    std::uint32_t attrBase[16] = {};

    // Texture binding: aurora sources texture data EXCLUSIVELY from
    // GX_AURORA_LOAD_TEXOBJ (the native GDSetTexObj seam) — it ignores the BP
    // image0/image3 registers for data. The .dff stream carries the real GC
    // bind dynamics as BP writes: image0 = width/height/format, image3 =
    // physical base addr >> 5. So the translator mirrors every bind: track
    // both regs per texmap and emit LOAD_TEXOBJ (host pointer into the
    // GC-RAM shadow) whenever a texmap has both halves. TextureMap memory
    // updates only fill the shadow (plus bump the data version so aurora's
    // texture cache re-uploads under an unchanged pointer).
    struct TexSlot {
        std::uint32_t image0 = 0;      // raw image0 value (w/h/fmt)
        std::uint32_t baseAddr = 0;    // byte address ((image3 & 0xFFFFFF) << 5)
        bool haveI0 = false, haveI3 = false;
    };
    TexSlot texSlots[8];               // 8 texture maps
    std::uint32_t texDataVersion = 1;  // bumped on every TextureMap memupdate

    // EFB copy state: aurora's copy_tex() sources its rect/dims/format/dest
    // exclusively from GX_AURORA_LOAD_COPY_{SRC,DST,DEST} (the native GXCopyTex
    // seam) — it does NOT decode the raw BP copy regs. The .dff stream carries
    // the copies as BP writes, so mirror them: track 0x49 (src top/left),
    // 0x4A (src size-1), 0x4B (dest addr >> 5), and at each 0x52 trigger with
    // copy_to_xfb==0 synthesize the three aurora opcodes first. Field layout
    // per Dolphin BPMemory.h (X10Y10 + UPE_Copy).
    std::uint32_t bpCopySrcXY = 0;     // 0x49: x=bits 0-9, y=bits 10-19
    std::uint32_t bpCopySrcWH = 0;     // 0x4A: (w-1)=bits 0-9, (h-1)=bits 10-19
    std::uint32_t bpCopyDest  = 0;     // 0x4B: dest phys addr >> 5

    // Emit a GX_AURORA_LOAD_TEXOBJ for texmap `id` from its tracked state.
    auto emitTexObj = [&](int id) {
        const TexSlot& ts = texSlots[id];
        std::uint32_t i0 = ts.image0;
        std::uint32_t w = (i0 & 0x3FF) + 1;
        std::uint32_t h = ((i0 >> 10) & 0x3FF) + 1;
        std::uint32_t fmt = (i0 >> 20) & 0xF;
        if (fmt == 8 || fmt == 9 || fmt == 10) {
            // FAIL FAST: LOAD_TLUT synthesis is not implemented; a CI-format
            // bind would sample garbage palettes and masquerade as a render
            // defect. Crash at the missing feature instead. (Was a one-time
            // warn -- silent-ish fallbacks banned 2026-07-14.)
            std::fprintf(stderr,
                "[fifo_player] FATAL: CI-format texture bound (fmt=%u texmap=%d gcAddr=0x%06X) "
                "but LOAD_TLUT synthesis is not implemented -- implement TLUT tracking "
                "(BP 0x64/0x65) before replaying this capture\n",
                fmt, id, ts.baseAddr);
            std::abort();
        }
        std::uint64_t hostPtr = ts.baseAddr
            ? reinterpret_cast<std::uint64_t>(shadow.host(ts.baseAddr)) : 0;
        // SB_FIFO_TEXDBG=1: log the first synthesized binds (which texmap, GC
        // addr, dims/format) to sanity-check bind synthesis against the oracle.
        static int s_texDbg = -1;
        if (s_texDbg < 0) s_texDbg = std::getenv("SB_FIFO_TEXDBG") != nullptr ? 1 : 0;
        if (s_texDbg == 1) {
            static int n = 0;
            if (n < 40) {
                std::fprintf(stderr,
                    "[fifo-texbind] n=%d texmap=%d gcAddr=0x%06X %ux%u fmt=%u ver=%u\n",
                    ++n, id, ts.baseAddr, w, h, fmt, texDataVersion);
            }
        }
        auto pushBE16 = [&](std::uint16_t v) { out.push_back(v>>8); out.push_back(v&0xFF); };
        auto pushBE64 = [&](std::uint64_t v) { for(int i=7;i>=0;--i) out.push_back((v>>(i*8))&0xFF); };
        auto pushBE32 = [&](std::uint32_t v) { for(int i=3;i>=0;--i) out.push_back((v>>(i*8))&0xFF); };
        out.push_back(GX_AURORA);
        pushBE16(GX_AURORA_LOAD_TEXOBJ);
        out.push_back(static_cast<std::uint8_t>(id));
        pushBE64(hostPtr);
        pushBE32(w);
        pushBE32(h);
        pushBE32(fmt);
        pushBE32(0);          // tlut (none for now; LOAD_TLUT is a separate layer)
        out.push_back(0);     // mipCount (base only; mode1 max_lod arrives via BP)
        pushBE32(ts.baseAddr);      // texObjId: GC base addr uniquely names the object
        pushBE32(texDataVersion);   // re-binds after new data get a fresh version
    };

    while (pos < end) {
        // Apply memory updates scheduled at-or-before this position.
        while (muIdx < frame.memoryUpdates.size() &&
               frame.memoryUpdates[muIdx].fifoPosition <= pos) {
            const MemoryUpdate& mu = frame.memoryUpdates[muIdx];
            if (mu.type == kMemUpdateVertexStream || mu.type == kMemUpdateXFData) {
                shadow.write(mu.address, mu.data.data(), mu.data.size());
            } else if (mu.type == kMemUpdateTextureMap) {
                // New texture bytes land in the shadow; bump the version so the
                // NEXT bind of any texobj over this data re-uploads. If a texmap
                // is currently bound to this address, re-emit it immediately
                // (data changed under a live binding).
                shadow.write(mu.address, mu.data.data(), mu.data.size());
                ++texDataVersion;
                for (int t = 0; t < 8; ++t) {
                    if (texSlots[t].haveI0 && texSlots[t].haveI3 &&
                        texSlots[t].baseAddr == mu.address) {
                        emitTexObj(t);
                    }
                }
            } else {
                // FAIL FAST: an unhandled memupdate type (e.g. kMemUpdateTMEM
                // 0x08 — TMEM preloads) means the capture depends on state this
                // translator doesn't model. Dropping it silently would surface
                // as inexplicable texture garbage later.
                std::fprintf(stderr,
                    "[fifo_player] FATAL: unhandled memory-update type 0x%02X "
                    "(addr=0x%08X size=%zu) -- implement it before replaying this capture\n",
                    mu.type, mu.address, mu.data.size());
                std::abort();
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
                // ALL arrays are big-endian here: every base points into the
                // GC-RAM shadow, which holds raw Dolphin-emulated GC memory
                // (memoryUpdates), including the matrix arrays (attr 12-15).
                // (le=true only applies to the NATIVE runtime, where the game
                // computes matrix arrays host-side; a .dff replay never does.)
                bool le = false;
                static int s_abDbg = -1;
                if (s_abDbg < 0) s_abDbg = std::getenv("SB_FIFO_TEXDBG") != nullptr ? 1 : 0;
                if (s_abDbg == 1) {
                    static int n = 0;
                    if (n < 40) {
                        std::fprintf(stderr, "[fifo-arraybase] n=%d cpReg=0x%02X attr=%d gcAddr=0x%08X\n",
                                     ++n, addr, attrIdx, val);
                    }
                }
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
            // Mirror texture binds: image0 (w/h/fmt) + image3 (base addr >> 5)
            // per texmap; a texmap with both halves gets a synthesized
            // LOAD_TEXOBJ at every image write (GXLoadTexObj emits image3 last,
            // but J3D re-binds in varying order -- emitting on either write
            // once both are known is bind-order agnostic; a redundant re-emit
            // with unchanged fields is a no-op in aurora's cache).
            if (pos + 5 <= end) {
                std::uint8_t reg = src[pos + 1];
                // image0 regs: 0x88-0x8B (texmap 0-3), 0xA8-0xAB (texmap 4-7)
                // image3 regs: 0x94-0x97 (texmap 0-3), 0xB4-0xB7 (texmap 4-7)
                std::uint32_t val = (std::uint32_t(src[pos+2]) << 16) |
                                    (std::uint32_t(src[pos+3]) << 8) | src[pos+4];
                int tm = -1; bool isI0 = false;
                if (reg >= 0x88 && reg <= 0x8B) { tm = reg - 0x88; isI0 = true; }
                else if (reg >= 0xA8 && reg <= 0xAB) { tm = reg - 0xA8 + 4; isI0 = true; }
                else if (reg >= 0x94 && reg <= 0x97) { tm = reg - 0x94; }
                else if (reg >= 0xB4 && reg <= 0xB7) { tm = reg - 0xB4 + 4; }
                if (tm >= 0) {
                    if (isI0) { texSlots[tm].image0 = val; texSlots[tm].haveI0 = true; }
                    else      { texSlots[tm].baseAddr = (val & 0x00FFFFFFu) << 5;
                                texSlots[tm].haveI3 = true; }
                    // Emit ONLY at image3 — GX writes a bind's registers in
                    // ascending order (mode, image0..image3), so image3 marks
                    // the set complete. Emitting at image0 would pair the NEW
                    // dims with the PREVIOUS bind's base address and poison
                    // aurora's (texObjId, version)-keyed cache with wrong dims.
                    if (!isI0 && texSlots[tm].haveI0) {
                        ++texDataVersion;  // fresh version per bind: never
                                           // trust a possibly-poisoned entry
                        emitTexObj(tm);
                    }
                }
                // EFB copy regs + trigger.
                if (reg == 0x49) bpCopySrcXY = val;
                else if (reg == 0x4A) bpCopySrcWH = val;
                else if (reg == 0x4B) bpCopyDest = val;
                else if (reg == 0x52 && ((val >> 14) & 1u) == 0 &&
                         std::getenv("SB_FIFO_NO_COPYSYN") == nullptr) {
                    // SB_FIFO_NO_COPYSYN=1 (diagnostic): skip EFB-copy synthesis
                    // so copy-fed texture binds fall back to decoding the
                    // recorded RAM snapshot bytes (what Dolphin's memupdates
                    // captured) instead of aurora's live EFB resolve. A/B
                    // isolates "copy plumbing wrong" from "copied content wrong":
                    // frame-feedback textures bootstrap from record-time pixels.
                    // Texture copy (copy_to_xfb==0): synthesize the aurora copy
                    // opcodes BEFORE the 0x52 passthrough triggers copy_tex().
                    // UPE_Copy: target_pixel_format=bits 3-6 (tp_realFormat =
                    // tpf/2 + (tpf&1)*8), half_scale=bit 9, clear=bit 11.
                    std::uint32_t srcX = bpCopySrcXY & 0x3FF;
                    std::uint32_t srcY = (bpCopySrcXY >> 10) & 0x3FF;
                    std::uint32_t srcW = (bpCopySrcWH & 0x3FF) + 1;
                    std::uint32_t srcH = ((bpCopySrcWH >> 10) & 0x3FF) + 1;
                    std::uint32_t tpf = (val >> 3) & 0xF;
                    std::uint32_t fmt = tpf / 2 + (tpf & 1) * 8;
                    bool halfScale = ((val >> 9) & 1u) != 0;
                    std::uint32_t dstW = halfScale ? srcW / 2 : srcW;
                    std::uint32_t dstH = halfScale ? srcH / 2 : srcH;
                    if (fmt >= 8) {
                        // FAIL FAST: Z/exotic copy formats are not handled by
                        // the synthesis; passing them through would misresolve
                        // and pollute downstream sampling. (Was a warn.)
                        std::fprintf(stderr,
                            "[fifo_player] FATAL: EFB copy with Z/exotic real-format %u "
                            "(tpf=%u, trigger val=0x%06X) -- implement this format's "
                            "synthesis before replaying this capture\n",
                            fmt, tpf, val);
                        std::abort();
                    }
                    auto pushBE16 = [&](std::uint16_t v) { out.push_back(v>>8); out.push_back(v&0xFF); };
                    auto pushBE32 = [&](std::uint32_t v) { for(int i=3;i>=0;--i) out.push_back((v>>(i*8))&0xFF); };
                    auto pushBE64 = [&](std::uint64_t v) { for(int i=7;i>=0;--i) out.push_back((v>>(i*8))&0xFF); };
                    out.push_back(GX_AURORA);
                    pushBE16(GX_AURORA_LOAD_COPY_SRC);
                    pushBE32(srcX); pushBE32(srcY); pushBE32(srcW); pushBE32(srcH);
                    out.push_back(GX_AURORA);
                    pushBE16(GX_AURORA_LOAD_COPY_DST);
                    pushBE32(dstW); pushBE32(dstH); pushBE32(fmt);
                    std::uint32_t destAddr = (bpCopyDest & 0x00FFFFFFu) << 5;
                    out.push_back(GX_AURORA);
                    pushBE16(GX_AURORA_LOAD_COPY_DEST);
                    pushBE64(reinterpret_cast<std::uint64_t>(shadow.host(destAddr)));
                }
            }
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
        // Unknown opcode: the walker has DESYNCED from the stream (0 unknown
        // opcodes in audited captures) -- every byte after this point would be
        // misinterpreted. Crash at the desync, never "emit 1 byte and hope".
        std::fprintf(stderr,
            "[fifo_player] FATAL: unknown opcode 0x%02x @ %u (frame stream desync -- "
            "vertex-size walker or opcode table is wrong for this capture)\n", op, pos);
        std::abort();
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

        std::vector<std::uint8_t> cmds = translate_frame(cap, rendered, shadow);
        std::printf("[fifo_player] frame %d: translated %u -> %u bytes\n",
                    rendered, (unsigned)frame.fifoData.size(), (unsigned)cmds.size());
        std::fflush(stdout);
        if (!aurora_begin_frame()) {
            // FAIL FAST: a replay frame that can't begin means device loss or
            // an external quit -- a truncated replay must not look like a
            // successful shorter one.
            std::fprintf(stderr,
                "[fifo_player] FATAL: aurora_begin_frame failed at frame %d of %zu\n",
                rendered, cap.frames.size());
            std::abort();
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
    // Pump two empty frames so async readbacks queued on the last replay frame
    // (SB_DUMP_FRAME's buffer map) resolve before the device is destroyed --
    // without this the dump aborts with "Buffer was destroyed before mapping
    // was resolved" and writes nothing.
    for (int i = 0; i < 2; ++i) {
        if (!aurora_begin_frame()) break;
        aurora_end_frame();
    }
    return rendered;
}

} // namespace sb

// C entry point for main.cpp's SB_FIFO_REPLAY gate.
extern "C" int sb_fifo_replay_run(const char* dffPath) {
    // Pipelines compile synchronously by default (aurora, 2026-07-14) --
    // no env needed here. If SB_ASYNC_PIPELINES is exported, a draw hitting a
    // missing pipeline crashes in aurora's bind_pipeline rather than skipping.
    sb::FifoCapture cap = sb::load_dff(dffPath);
    return sb::replay(cap);
}


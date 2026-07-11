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

// ---- replay ----

namespace {
// Opcode constants (dolphin/gx/GXCommandList.h).
constexpr std::uint8_t GX_NOP          = 0x00;
constexpr std::uint8_t GX_LOAD_BP_REG  = 0x61;
constexpr std::uint8_t GX_LOAD_CP_REG  = 0x08;
constexpr std::uint8_t GX_LOAD_XF_REG  = 0x10;
constexpr std::uint8_t GX_CMD_CALL_DL  = 0x40;
constexpr std::uint8_t GX_AURORA       = 0x50;
constexpr std::uint8_t GX_OPCODE_MASK  = 0xF8;
constexpr std::uint8_t GX_VAT_MASK     = 0x07;
// CP array-base register addresses (aurora ignores these; the translator must
// synthesize GX_AURORA_LOAD_ARRAYBASE from the .dff's VertexStream data instead).
constexpr std::uint8_t CP_REG_ARRAYBASE_LO = 0xA0;
constexpr std::uint8_t CP_REG_ARRAYBASE_HI = 0xAF;

// Big-endian readers (the .dff command stream is GC-native big-endian).
std::uint32_t rd_be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8)  | std::uint32_t(p[3]);
}

// Build the translated command stream for one frame: pass through the .dff's
// command bytes verbatim (BP/CP/XF/prim are HW-standard and aurora decodes
// them), interleaving memory updates at their fifoPosition. This FIRST PASS
// does no address translation (vertex/texture host-pointer synthesis) -- it
// validates the decoder runs clean and the frame renders its clear/2D tail.
// Translation layers are added incrementally on top.
std::vector<std::uint8_t> translate_frame(const FifoFrame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(frame.fifoData.size() + frame.memoryUpdates.size() * 16);
    std::size_t muIdx = 0;
    const std::uint8_t* src = frame.fifoData.data();
    std::uint32_t pos = 0;
    const std::uint32_t end = (std::uint32_t)frame.fifoData.size();
    while (pos < end) {
        // Apply any memory updates scheduled at this position.
        while (muIdx < frame.memoryUpdates.size() &&
               frame.memoryUpdates[muIdx].fifoPosition <= pos) {
            // TODO(translation): VertexStream -> host buffer + GX_AURORA_LOAD_ARRAYBASE;
            //   TextureMap -> host buffer + GX_AURORA_LOAD_TEXOBJ; XFData -> XF snapshot.
            // For this first pass: record that we saw them (no-op emit).
            ++muIdx;
        }
        std::uint8_t op = src[pos];
        // Pass every opcode through verbatim; we just need to advance pos correctly
        // so the memory-update interleaving stays in sync. Reuse the same opcode-size
        // logic the decoder will use (a partial copy of command_processor's sizing).
        std::uint32_t adv = 1;  // default: 1-byte opcode
        if (op == GX_NOP) {
            // Collapse the NOP run into one append.
            std::uint32_t r = 0;
            while (pos + r < end && src[pos + r] == GX_NOP) ++r;
            adv = r;
        } else if (op == GX_LOAD_BP_REG) {
            adv = 5;
        } else if (op == GX_LOAD_CP_REG) {
            adv = 6;
        } else if (op == GX_LOAD_XF_REG) {
            // 1 (op) + 4 (cmd: addr[0:16] | (count-1)<<16) + count*4 data words
            if (pos + 5 > end) { adv = end - pos; }
            else {
                std::uint32_t cmd = rd_be32(src + pos + 1);
                std::uint32_t count = (cmd >> 16) + 1;
                adv = 5 + count * 4;
            }
        } else if (op >= 0x20 && op <= 0x38 && (op & 0x18) == 0x20) {
            // LOAD_INDX_A/B/C/D (0x20,0x28,0x30,0x38)
            adv = 5;
        } else if (op == GX_CMD_CALL_DL) {
            adv = 9;  // absent in SMS title (audited); sized for safety
        } else if (op == 0x44 || op == 0x48) {
            adv = 1;
        } else if (op == GX_AURORA) {
            // 0x50 + u16 subcmd; payload size varies -- not present in a .dff.
            adv = 3;  // minimal; not expected in replay input
        } else if ((op & GX_OPCODE_MASK) >= 0x80) {
            // Primitive: 3-byte header (prim|vat, numVerts u16 BE), then vertex bytes.
            // Vertex size depends on the VAT config -- we can't size it here without
            // the CP state, so copy the 3-byte header and let the decoder consume.
            // For memory-update interleaving we only need pos to reach the next
            // opcode boundary, which requires vertex sizing. Fall through to
            // appending the whole remaining stream up to the next recognized opcode
            // is unsafe; instead we advance by 3 + (skip the primitive in the decoder).
            // SIMPLEST CORRECT APPROACH: stop translating per-opcode here. Emit the
            // rest of the frame verbatim and flush remaining memory updates; the
            // decoder already handles primitives end-to-end.
            out.insert(out.end(), src + pos, src + end);
            pos = end;
            continue;
        }
        out.insert(out.end(), src + pos, src + pos + std::min(adv, end - pos));
        pos += adv;
    }
    return out;
}
} // namespace

int replay(const FifoCapture& cap) {
    print_summary(cap);
    int rendered = 0;
    for (const auto& frame : cap.frames) {
        std::vector<std::uint8_t> cmds = translate_frame(frame);
        if (!aurora_begin_frame()) {
            std::fprintf(stderr, "[fifo_player] aurora_begin_frame returned false -- aborting replay\n");
            break;
        }
        aurora_fifo_replay(cmds.data(), (std::uint32_t)cmds.size(), 1);
        aurora_end_frame();  // drains, renders, and captures via SB_DUMP_FRAME
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


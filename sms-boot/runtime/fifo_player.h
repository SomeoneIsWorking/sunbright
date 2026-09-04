// fifo_player.h — Dolphin .dff FIFO replay for the parity harness.
//
// Replays a captured Dolphin GX FIFO (.dff) through aurora's renderer so the
// output can be compared pixel-for-pixel against Dolphin's render of the same
// FIFO — a controlled same-input/two-renderers experiment. Diagnostic ONLY
// (SB_FIFO_REPLAY env gate); consumes a static .dff, no Dolphin code.
//
// See debug_journal/2026-07-11_fifo_replay_no_calldl.md for the
// resolution of the CALL_DL risk (SMS title GX stream has zero display-list
// calls, so the command stream is flat and self-contained).
//
// Loader = faithful C++ port of the read side of tools/oracle/parse_fifo_dff.py
// (itself a reimpl of Dolphin's FifoDataFile binary format). On-disk structs are
// little-endian, packed.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sb {

#pragma pack(push, 1)
// Dolphin FifoDataFile on-disk structs (see scratch/oracle_fifo/FifoDataFile.cpp.ref).
// All little-endian on disk.
struct DffFileHeader {
    std::uint32_t fileId;            // 0x0D01F1F0
    std::uint32_t fileVersion;       // 6
    std::uint32_t minLoaderVersion;  // 1
    std::uint64_t bpMemOffset;  std::uint32_t bpMemSize;
    std::uint64_t cpMemOffset;  std::uint32_t cpMemSize;
    std::uint64_t xfMemOffset;  std::uint32_t xfMemSize;
    std::uint64_t xfRegsOffset; std::uint32_t xfRegsSize;
    std::uint64_t frameListOffset; std::uint32_t frameCount; std::uint32_t flags;
    std::uint64_t texMemOffset; std::uint32_t texMemSize;
    std::uint32_t mem1Size; std::uint32_t mem2Size;
    char gameId[8];
    std::uint8_t reserved[24];
};
static_assert(sizeof(DffFileHeader) == 128, "DffFileHeader must be 128 bytes");

struct DffFrameInfo {
    std::uint64_t fifoDataOffset;
    std::uint32_t fifoDataSize;
    std::uint32_t fifoStart;
    std::uint32_t fifoEnd;
    std::uint64_t memoryUpdatesOffset;
    std::uint32_t numMemoryUpdates;
    std::uint8_t reserved[32];
};
static_assert(sizeof(DffFrameInfo) == 64, "DffFrameInfo must be 64 bytes");

struct DffMemoryUpdate {
    std::uint32_t fifoPosition;  // offset into the frame's command stream
    std::uint32_t address;       // GC RAM address this update patches
    std::uint64_t dataOffset;    // offset into the .dff file for the data
    std::uint32_t dataSize;
    std::uint8_t type;           // MemoryUpdate::Type: TextureMap=1, XFData=2, VertexStream=4, TMEM=8
    std::uint8_t reserved[3];
};
static_assert(sizeof(DffMemoryUpdate) == 24, "DffMemoryUpdate must be 24 bytes");
#pragma pack(pop)

// Memory-update type constants (Dolphin MemoryUpdate::Type).
constexpr std::uint8_t kMemUpdateTextureMap  = 0x01;
constexpr std::uint8_t kMemUpdateXFData      = 0x02;
constexpr std::uint8_t kMemUpdateVertexStream = 0x04;
constexpr std::uint8_t kMemUpdateTMEM        = 0x08;

// One memory update, with its data materialized out of the file.
struct MemoryUpdate {
    std::uint32_t fifoPosition;
    std::uint32_t address;
    std::vector<std::uint8_t> data;
    std::uint8_t type;
};

// One captured frame.
struct FifoFrame {
    std::vector<std::uint8_t> fifoData;        // raw GX command bytes
    std::uint32_t fifoStart, fifoEnd;          // GC RAM ring bounds (informational)
    std::vector<MemoryUpdate> memoryUpdates;   // sorted by fifoPosition
};

// A loaded .dff. The file-wide memory snapshots (bpMem/cpMem/xfMem/xfRegs/texMem)
// are kept as borrowed slices into `fileData` (zero-copy).
//
// IMPORTANT: the header's *Size fields are u32 ELEMENT COUNTS, not byte sizes
// (Dolphin's FifoDataFile writes header.bpMemSize = BP_MEM_SIZE = 256, then
// WriteArray(m_BPMem) writes 256 u32s = 1024 bytes). The byte size is count*4.
// texMem is the exception: it's a u8 array, so texMemSize IS the byte size.
// ENDIANNESS: these snapshots are Dolphin's host-endian (LITTLE-endian) state
// arrays written raw — unlike the command stream and memoryUpdates, which are
// GC big-endian. Verified empirically (see preload_state).
struct FifoCapture {
    std::vector<std::uint8_t> fileData;        // the whole file, kept alive
    const DffFileHeader* header = nullptr;
    const std::uint8_t* bpMem = nullptr;  std::uint32_t bpMemCount = 0;   // u32 elements
    const std::uint8_t* cpMem = nullptr;  std::uint32_t cpMemCount = 0;   // u32 elements
    const std::uint8_t* xfMem = nullptr;  std::uint32_t xfMemCount = 0;   // u32 elements
    const std::uint8_t* xfRegs = nullptr; std::uint32_t xfRegsCount = 0;  // u32 elements
    const std::uint8_t* texMem = nullptr; std::uint32_t texMemSize = 0;   // bytes (u8 array)
    std::vector<FifoFrame> frames;
};

// Load a .dff. OSPanics on bad magic / truncation (fail-fast per project policy).
FifoCapture load_dff(const std::string& path);

// Print a one-line-per-frame summary (mirrors parse_fifo_dff.py --summary).
void print_summary(const FifoCapture& cap);

// Replays every frame of `cap` through aurora's renderer and dumps each via the
// existing SB_DUMP_FRAME path (set SB_DUMP_FRAME=<path.rgba>; output is
// <path>.<frame>.rgba). Returns the number of frames rendered. Driver of the
// SB_FIFO_REPLAY mode in main.cpp.
int replay(const FifoCapture& cap);

} // namespace sb

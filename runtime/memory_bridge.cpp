#include "memory_bridge.h"
#include "cpu_state.h"
#include <cstring>
#include <stdexcept>

bool g_recomp_touched_mmio = false;

// Memory bridge: routes effective addresses to Dolphin's MemMap or our flat buffer.
//
// GC memory map:
//   0x80000000–0x81800000  Main RAM (24 MB), cached
//   0xC0000000–0xC1800000  Main RAM, uncached (same physical)
//   0xCC000000             Hardware registers
//
// We strip the top bit to get physical address: ea & 0x1FFFFFFF → phys

#ifdef HAVE_DOLPHIN_MEMMAP
#  include "Core/HW/Memmap.h"
#  include "Core/HW/GPFifo.h"
#  include "Core/PowerPC/MMU.h"
#  include "Core/System.h"

static inline Memory::MemoryManager& MEM() {
    return Core::System::GetInstance().GetMemory();
}

// MMIO (hardware registers) must go through the MMU's Read<T>/Write<T>, which
// dispatch to the VI/PE/DSP/SI/EXI handlers. MemoryManager::Read_U*/Write_U* only
// touch RAM (CopyToEmu/FromEmu) and silently fail on MMIO ("Invalid range"/"Unknown
// Pointer") — that broke e.g. the DSP mailbox handshake.
static inline PowerPC::MMU& MMU_() {
    return Core::System::GetInstance().GetMMU();
}

// The write-gather pipe (physical 0x0C008000) is how the CPU streams GX display-
// list commands to the GP FIFO. It isn't normal MMIO — writes must accumulate in
// GPFifoManager and burst to the FIFO. Generic Write_U32 would just warn "Unknown
// Pointer" and the commands would never reach the GPU.
static inline bool is_gather_pipe(u32 ea) { return (ea & 0x0FFFFFFF) == 0x0C008000; }
static inline GPFifo::GPFifoManager& GPF() { return Core::System::GetInstance().GetGPFifo(); }

// Fast path: main RAM only (cached 0x8xxxxxxx / uncached 0xCxxxxxxx mirrors of the
// low 24 MB). Returns nullptr for everything else — MMIO, locked cache, etc. —
// which must go through Dolphin's Read_U*/Write_U* so hardware register handlers
// (VI/PE/DSP/SI/EXI…) actually run. Reading those via a raw pointer returns
// garbage and breaks any code that polls a hardware status bit.
static inline u8* ram_ptr(u32 ea) {
    const u32 top = ea >> 28;
    if ((top == 0x8 || top == 0xC) && (ea & 0x0FFFFFFF) < 0x01800000)
        return MEM().GetPointerForRange(ea, 1);
    return nullptr;
}

#  define MMIO_R(bits, ea)     (g_recomp_touched_mmio = true, MMU_().Read<u##bits>(ea))
#  define MMIO_W(bits, ea, v)  (g_recomp_touched_mmio = true, MMU_().Write<u##bits>((v), (ea)))
#else
// Standalone mode: flat 24 MB RAM buffer, no MMIO.
static u8 g_ram[24 * 1024 * 1024];
static bool g_ram_init = false;

void memory_bridge_init(const u8* initial, u32 size) {
    if (size > sizeof(g_ram)) size = sizeof(g_ram);
    if (initial) std::memcpy(g_ram, initial, size);
    g_ram_init = true;
}

static inline u8* ram_ptr(u32 ea) {
    u32 phys = ea & 0x1FFFFFFF;
    if (phys >= sizeof(g_ram)) return nullptr;
    return g_ram + phys;
}

#  define MMIO_R(bits, ea)     0
#  define MMIO_W(bits, ea, v)  ((void)0)
#endif

// ── Byte-swapped reads/writes (GC = big-endian, host = little-endian) ───────

u8 mem_r8(u32 ea) {
    if (u8* p = ram_ptr(ea)) return *p;
    return MMIO_R(8, ea);
}

u16 mem_r16(u32 ea) {
    if (u8* p = ram_ptr(ea)) return ((u16)p[0] << 8) | p[1];
    return MMIO_R(16, ea);
}

u32 mem_r32(u32 ea) {
    if (u8* p = ram_ptr(ea)) return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
    return MMIO_R(32, ea);
}

u64 mem_r64(u32 ea) {
    if (u8* p = ram_ptr(ea))
        return ((u64)p[0]<<56)|((u64)p[1]<<48)|((u64)p[2]<<40)|((u64)p[3]<<32)
             | ((u64)p[4]<<24)|((u64)p[5]<<16)|((u64)p[6]<<8)|p[7];
    return MMIO_R(64, ea);
}

f32 mem_rf32(u32 ea) {
    u32 bits = mem_r32(ea);
    f32 v; std::memcpy(&v, &bits, 4); return v;
}

f64 mem_rf64(u32 ea) {
    u64 bits = mem_r64(ea);
    f64 v; std::memcpy(&v, &bits, 8); return v;
}

void mem_w8(u32 ea, u8 v) {
    if (u8* p = ram_ptr(ea)) { *p = v; return; }
#ifdef HAVE_DOLPHIN_MEMMAP
    if (is_gather_pipe(ea)) { g_recomp_touched_mmio = true; GPF().Write8(v); return; }
#endif
    MMIO_W(8, ea, v);
}

void mem_w16(u32 ea, u16 v) {
    if (u8* p = ram_ptr(ea)) { p[0] = v >> 8; p[1] = v & 0xFF; return; }
#ifdef HAVE_DOLPHIN_MEMMAP
    if (is_gather_pipe(ea)) { g_recomp_touched_mmio = true; GPF().Write16(v); return; }
#endif
    MMIO_W(16, ea, v);
}

void mem_w32(u32 ea, u32 v) {
    if (u8* p = ram_ptr(ea)) {
        p[0]=v>>24; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF; return;
    }
#ifdef HAVE_DOLPHIN_MEMMAP
    if (is_gather_pipe(ea)) { g_recomp_touched_mmio = true; GPF().Write32(v); return; }
#endif
    MMIO_W(32, ea, v);
}

void mem_w64(u32 ea, u64 v) {
    if (u8* p = ram_ptr(ea)) {
        p[0]=v>>56; p[1]=(v>>48)&0xFF; p[2]=(v>>40)&0xFF; p[3]=(v>>32)&0xFF;
        p[4]=(v>>24)&0xFF; p[5]=(v>>16)&0xFF; p[6]=(v>>8)&0xFF; p[7]=v&0xFF; return;
    }
#ifdef HAVE_DOLPHIN_MEMMAP
    if (is_gather_pipe(ea)) { g_recomp_touched_mmio = true; GPF().Write64(v); return; }
    // No Write_U64 in Dolphin's MMIO API — split into two 32-bit writes.
    MMIO_W(32, ea, (u32)(v >> 32));
    MMIO_W(32, ea + 4, (u32)v);
#else
    MMIO_W(32, ea, (u32)(v >> 32));
    MMIO_W(32, ea + 4, (u32)v);
#endif
}

void mem_wf32(u32 ea, f32 v) {
    u32 bits; std::memcpy(&bits, &v, 4); mem_w32(ea, bits);
}

void mem_wf64(u32 ea, f64 v) {
    u64 bits; std::memcpy(&bits, &v, 8); mem_w64(ea, bits);
}

// ── psq dequantize/quantize ──────────────────────────────────────────────────
// GQR types: 0=f32, 4=u8, 5=u16, 6=s8, 7=s16
// scale: 0–63, divisor = 1 << scale

f64 psq_dequantize(u32 raw, u32 type, u32 scale) {
    float s = 1.0f / (float)(1u << scale);
    switch (type) {
    case 0:  { f32 v; std::memcpy(&v, &raw, 4); return v; }
    case 4:  return (u8)raw  * s;
    case 5:  return (u16)raw * s;
    case 6:  return (s8)raw  * s;
    case 7:  return (s16)raw * s;
    default: return 0.0;
    }
}

u32 psq_quantize(f64 val, u32 type, u32 scale) {
    float s = (float)(1u << scale);
    switch (type) {
    case 0:  { f32 v = (f32)val; u32 r; std::memcpy(&r, &v, 4); return r; }
    case 4:  return (u8) std::max(0.0, std::min(255.0, val * s));
    case 5:  return (u16)std::max(0.0, std::min(65535.0, val * s));
    case 6:  return (u8) (s8) std::max(-128.0, std::min(127.0, val * s));
    case 7:  return (u16)(s16)std::max(-32768.0, std::min(32767.0, val * s));
    default: return 0;
    }
}

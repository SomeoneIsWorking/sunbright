#include "memory_bridge.h"
#include "cpu_state.h"
#include <cstring>
#include <stdexcept>

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
   static inline u8* phys_ptr(u32 ea) {
       return Memory::GetPointer(ea);
   }
#else
// Standalone mode: flat 24 MB RAM buffer
static u8 g_ram[24 * 1024 * 1024];
static bool g_ram_init = false;

void memory_bridge_init(const u8* initial, u32 size) {
    if (size > sizeof(g_ram)) size = sizeof(g_ram);
    if (initial) std::memcpy(g_ram, initial, size);
    g_ram_init = true;
}

static u8* phys_ptr(u32 ea) {
    u32 phys = ea & 0x1FFFFFFF;
    if (phys >= sizeof(g_ram)) return nullptr;
    return g_ram + phys;
}
#endif

// ── Byte-swapped reads/writes (GC = big-endian, host = little-endian) ───────

u8 mem_r8(u32 ea) {
    u8* p = phys_ptr(ea);
    if (!p) return 0;
    return *p;
}

u16 mem_r16(u32 ea) {
    u8* p = phys_ptr(ea);
    if (!p) return 0;
    return ((u16)p[0] << 8) | p[1];
}

u32 mem_r32(u32 ea) {
    u8* p = phys_ptr(ea);
    if (!p) return 0;
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
}

u64 mem_r64(u32 ea) {
    return ((u64)mem_r32(ea) << 32) | mem_r32(ea + 4);
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
    u8* p = phys_ptr(ea);
    if (p) *p = v;
}

void mem_w16(u32 ea, u16 v) {
    u8* p = phys_ptr(ea);
    if (!p) return;
    p[0] = v >> 8; p[1] = v & 0xFF;
}

void mem_w32(u32 ea, u32 v) {
    u8* p = phys_ptr(ea);
    if (!p) return;
    p[0]=v>>24; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF;
}

void mem_w64(u32 ea, u64 v) {
    mem_w32(ea, (u32)(v >> 32));
    mem_w32(ea + 4, (u32)v);
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

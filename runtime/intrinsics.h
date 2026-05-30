#pragma once
#include "cpu_state.h"
#include <bit>
#include <cmath>

// Memory access — implemented in memory_bridge.cpp, backed by Dolphin's MemMap.
// All addresses are effective (as the game sees them).
// The bridge strips the top bit (0x8xxxxxxx → 0x0xxxxxxx) for physical access.
extern u8  mem_r8 (u32 ea);
extern u16 mem_r16(u32 ea);
extern u32 mem_r32(u32 ea);
extern u64 mem_r64(u32 ea);
extern f32 mem_rf32(u32 ea);
extern f64 mem_rf64(u32 ea);
extern void mem_w8 (u32 ea, u8  v);
extern void mem_w16(u32 ea, u16 v);
extern void mem_w32(u32 ea, u32 v);
extern void mem_w64(u32 ea, u64 v);
extern void mem_wf32(u32 ea, f32 v);
extern void mem_wf64(u32 ea, f64 v);

// Convenience macros used by emitted code
#define MEM_R8(ea)      mem_r8(ea)
#define MEM_R16(ea)     mem_r16(ea)
#define MEM_R32(ea)     mem_r32(ea)
#define MEM_R64(ea)     mem_r64(ea)
#define MEM_RF32(ea)    mem_rf32(ea)
#define MEM_RF64(ea)    mem_rf64(ea)
#define MEM_W8(ea,v)    mem_w8(ea,v)
#define MEM_W16(ea,v)   mem_w16(ea,v)
#define MEM_W32(ea,v)   mem_w32(ea,v)
#define MEM_W64(ea,v)   mem_w64(ea,v)
#define MEM_WF32(ea,v)  mem_wf32(ea,v)
#define MEM_WF64(ea,v)  mem_wf64(ea,v)

// Byte-swap helpers (GC is big-endian, host is little-endian)
inline u16 bswap16(u16 v) { return __builtin_bswap16(v); }
inline u32 bswap32(u32 v) { return __builtin_bswap32(v); }
inline u64 bswap64(u64 v) { return __builtin_bswap64(v); }

// Rotate helpers
inline u32 rotl32(u32 v, u32 n) { return (v << n) | (v >> (32 - n)); }

// Sign extend
template<int bits>
inline s32 sext(u32 v) {
    constexpr u32 mask = 1u << (bits - 1);
    return (v ^ mask) - mask;
}

// Carry/overflow for add/sub
inline bool add_carry(u32 a, u32 b) {
    return (u64)a + (u64)b > 0xFFFFFFFFull;
}
inline bool add_carry3(u32 a, u32 b, u32 c) {
    return (u64)a + (u64)b + (u64)c > 0xFFFFFFFFull;
}
inline bool add_overflow(u32 a, u32 b, u32 result) {
    return ((~(a ^ b)) & (a ^ result)) >> 31;
}
inline bool sub_carry(u32 a, u32 b) {
    return a >= b;  // PPC carry is inverted borrow
}

// Paired singles — GQR decode for psq_l/psq_st
// GQR format: [ST_TYPE:3][pad:5][ST_SCALE:6][pad:2][LD_TYPE:3][pad:5][LD_SCALE:6]
inline u32 gqr_ld_type (u32 gqr) { return  gqr & 0x7;        }
inline u32 gqr_ld_scale(u32 gqr) { return (gqr >> 8) & 0x3F;  }
inline u32 gqr_st_type (u32 gqr) { return (gqr >> 16) & 0x7;  }
inline u32 gqr_st_scale(u32 gqr) { return (gqr >> 24) & 0x3F; }

// Quantized load/store implementations (psq_l / psq_st)
// Types: 0=f32, 4=u8, 5=u16, 6=s8, 7=s16
f64 psq_dequantize(u32 raw, u32 type, u32 scale);
u32 psq_quantize(f64 val, u32 type, u32 scale);

// FP single rounding (frsp)
inline f64 frsp(f64 v) { return (f32)v; }

// FP reciprocal estimate (fres) — approximate
inline f64 fres(f64 v) { return 1.0 / v; }

// FP reciprocal sqrt estimate (frsqrte)
inline f64 frsqrte(f64 v) { return 1.0 / std::sqrt(v); }

// OS HLE call — implemented in runtime/os_hle.cpp
extern void os_hle_call(CPUState& cpu, u32 address);

// Indirect branch resolution — look up recompiled function by address
// Returns nullptr if not recompiled (Dolphin JIT will handle it)
using RecompFunc = void (*)(CPUState&);
extern RecompFunc recomp_lookup(u32 address);

// Call a PPC address — dispatches to recompiled or JIT
extern void call_ppc(CPUState& cpu, u32 address);

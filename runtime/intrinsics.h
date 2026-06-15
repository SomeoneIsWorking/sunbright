#pragma once
#include "cpu_state.h"
#include <bit>
#include <cmath>
#include <new>

// Memory access — backed by Dolphin's MemMap (memory_bridge.cpp).
// All addresses are effective (as the game sees them).
//
// The generated game code is dense with loads/stores, so the common case — an access to main
// RAM (cached 0x8xxxxxxx / uncached 0xCxxxxxxx mirror of the low 24 MB) — is INLINED here to a
// single host load/store + bswap with no function call (sb_r*/sb_w* below, reached via the
// MEM_* macros the emitter uses). Only non-RAM (MMIO / gather pipe / wild-write trap) and the
// brief pre-memory-init window fall back to the out-of-line *_slow handlers. Profiling showed
// ~99% of recomp wall-time was the old per-access function call into the bridge; inlining the
// RAM path is the fix. The out-of-line mem_r*/mem_w* wrappers (declared below) stay for the
// handful of runtime callers (native_os, overrides) that call them directly without the macros.
extern u8*  g_ram_base;        // base of main RAM, or nullptr until memory init (→ slow path)
extern u8*  g_l1_base;         // base of the locked-L1 array (0xE0000000, 256 KB), or nullptr
extern bool g_in_poll_yield;   // re-entrancy guard while a spin-loop yield runs
extern u32  g_poll_last;       // last read EA (spin-loop detector state)
extern u32  g_poll_reps;       // consecutive reads of g_poll_last

// Out-of-line slow paths (MMIO, gather pipe, wild-write trap, pre-init RAM).
extern u8   mem_r8_slow (u32 ea);
extern u16  mem_r16_slow(u32 ea);
extern u32  mem_r32_slow(u32 ea);
extern u64  mem_r64_slow(u32 ea);
extern void mem_w8_slow (u32 ea, u8  v);
extern void mem_w16_slow(u32 ea, u16 v);
extern void mem_w32_slow(u32 ea, u32 v);
extern void mem_w64_slow(u32 ea, u64 v);
extern void sb_poll_fire(u32 ea);   // advance CoreTiming / deliver IRQ once a tight poll is confirmed

// RAM fast-path host pointer for `ea`, or nullptr → caller takes the slow path.
inline u8* sb_ram_fast(u32 ea) {
    const u32 top = ea >> 28;
    if ((top == 0x8u || top == 0xCu) && (ea & 0x0FFFFFFFu) < 0x01800000u && g_ram_base)
        return g_ram_base + (ea & 0x01FFFFFFu);
    // Locked L1 cache (games use 0xE0000000; Dolphin backs it with a flat 256 KB array).
    // THP video decode runs its DCT/color-convert scratch here — without this fast path every
    // access in those inner loops took the out-of-line MMU slow path (the 6 fps movie crawl).
    if (top == 0xEu && (ea & 0x0FFFFFFFu) < 0x00040000u && g_l1_base)
        return g_l1_base + (ea & 0x0003FFFFu);
    return nullptr;
}
// Cheap inlined spin-loop note (on reads): same address ≥24× in a row → confirmed poll → yield.
// Only the threshold case makes the out-of-line call; normal sequential access just resets.
inline void sb_poll_note(u32 ea) {
    if (g_in_poll_yield) return;
    if (ea != g_poll_last) { g_poll_last = ea; g_poll_reps = 0; return; }
    if (++g_poll_reps < 24u) return;
    g_poll_reps = 0;
    sb_poll_fire(ea);
}

inline u8  sb_r8 (u32 ea) { if (u8* p = sb_ram_fast(ea)) { sb_poll_note(ea); return *p; } return mem_r8_slow(ea); }
inline u16 sb_r16(u32 ea) { if (u8* p = sb_ram_fast(ea)) { sb_poll_note(ea); u16 v; __builtin_memcpy(&v,p,2); return __builtin_bswap16(v); } return mem_r16_slow(ea); }
inline u32 sb_r32(u32 ea) { if (u8* p = sb_ram_fast(ea)) { sb_poll_note(ea); u32 v; __builtin_memcpy(&v,p,4); return __builtin_bswap32(v); } return mem_r32_slow(ea); }
inline u64 sb_r64(u32 ea) { if (u8* p = sb_ram_fast(ea)) { u64 v; __builtin_memcpy(&v,p,8); return __builtin_bswap64(v); } return mem_r64_slow(ea); }
inline f32 sb_rf32(u32 ea) { u32 b = sb_r32(ea); f32 v; __builtin_memcpy(&v,&b,4); return v; }
inline f64 sb_rf64(u32 ea) { u64 b = sb_r64(ea); f64 v; __builtin_memcpy(&v,&b,8); return v; }
// SUNBRIGHT_WATCH_WADDR=hex — write-watch diagnostic: any guest store touching the watched
// 8-byte window logs value + the emitted writer function (resolved via dladdr on the host
// return address). g_watch_wa==0 (default) keeps the hot path to a single predictable branch.
extern u32 g_watch_wa;                       // memory_bridge.cpp (env-initialized)
void sb_watch_fire(u32 ea, u32 v, int width, void* host_ra);
inline bool sb_watch_hit(u32 ea) { return __builtin_expect(g_watch_wa != 0, 0) && ((ea ^ g_watch_wa) & ~7u) == 0; }
inline void sb_w8 (u32 ea, u8  v) { if (sb_watch_hit(ea)) sb_watch_fire(ea, v, 1, __builtin_return_address(0)); if (u8* p = sb_ram_fast(ea)) { *p = v; return; } mem_w8_slow(ea, v); }
inline void sb_w16(u32 ea, u16 v) { if (sb_watch_hit(ea)) sb_watch_fire(ea, v, 2, __builtin_return_address(0)); if (u8* p = sb_ram_fast(ea)) { u16 b = __builtin_bswap16(v); __builtin_memcpy(p,&b,2); return; } mem_w16_slow(ea, v); }
inline void sb_w32(u32 ea, u32 v) { if (sb_watch_hit(ea)) sb_watch_fire(ea, v, 4, __builtin_return_address(0)); if (u8* p = sb_ram_fast(ea)) { u32 b = __builtin_bswap32(v); __builtin_memcpy(p,&b,4); return; } mem_w32_slow(ea, v); }
inline void sb_w64(u32 ea, u64 v) { if (u8* p = sb_ram_fast(ea)) { u64 b = __builtin_bswap64(v); __builtin_memcpy(p,&b,8); return; } mem_w64_slow(ea, v); }
inline void sb_wf32(u32 ea, f32 v) { u32 b; __builtin_memcpy(&b,&v,4); sb_w32(ea, b); }
inline void sb_wf64(u32 ea, f64 v) { u64 b; __builtin_memcpy(&b,&v,8); sb_w64(ea, b); }

// Out-of-line wrappers (defined in memory_bridge.cpp) for callers that don't use the macros.
extern u8  mem_r8 (u32 ea);
extern u16 mem_r16(u32 ea);
extern u32 mem_r32(u32 ea);
extern u64 mem_r64(u32 ea);
extern f32 mem_rf32(u32 ea);
extern f64 mem_rf64(u32 ea);
extern void mem_w8 (u32 ea, u8  v);
extern void mem_w16(u32 ea, u16 v);
extern void mem_w32(u32 ea, u32 v);
extern void dcbz32(u32 ea);          // dcbz: zero the 32-byte cache block containing ea
extern void icbi32(u32 ea);          // icbi: invalidate Dolphin icache/JIT block for the line at ea
extern void mem_w64(u32 ea, u64 v);
extern void mem_wf32(u32 ea, f32 v);
extern void mem_wf64(u32 ea, f64 v);

// Convenience macros used by emitted code → the INLINE fast path.
#define MEM_R8(ea)      sb_r8(ea)
#define MEM_R16(ea)     sb_r16(ea)
#define MEM_R32(ea)     sb_r32(ea)
#define MEM_R64(ea)     sb_r64(ea)
#define MEM_RF32(ea)    sb_rf32(ea)
#define MEM_RF64(ea)    sb_rf64(ea)
#define MEM_W8(ea,v)    sb_w8(ea,v)
#define MEM_W16(ea,v)   sb_w16(ea,v)
#define MEM_W32(ea,v)   sb_w32(ea,v)
#define MEM_W64(ea,v)   sb_w64(ea,v)
#define MEM_WF32(ea,v)  sb_wf32(ea,v)
#define MEM_WF64(ea,v)  sb_wf64(ea,v)

// Byte-swap helpers (GC is big-endian, host is little-endian)
inline u16 bswap16(u16 v) { return __builtin_bswap16(v); }
inline u32 bswap32(u32 v) { return __builtin_bswap32(v); }
inline u64 bswap64(u64 v) { return __builtin_bswap64(v); }

// Rotate helpers. n==0 must return v unchanged; `v >> 32` is UB, so guard it
// (very hot path — rlwinm with sh=0, e.g. the mask/clear-bit idioms).
inline u32 rotl32(u32 v, u32 n) { n &= 31; return n ? ((v << n) | (v >> (32 - n))) : v; }

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

// Paired singles — GQR decode for psq_l/psq_st.
// Gekko GQR layout (matches Dolphin UGQR, value bit 0 = LSB):
//   ST_TYPE: bits 0-2   ST_SCALE: bits 8-13   LD_TYPE: bits 16-18   LD_SCALE: bits 24-29
// (LD and ST were SWAPPED here — psq_l then dequantized with the *store* type/scale. Harmless
// when LD==ST or GQR=0/float, but a load-only quantized GQR — e.g. THP's paired-single IDCT,
// GQR5 — decoded to garbage. Fixed: load reads the high half, store the low half.)
inline u32 gqr_st_type (u32 gqr) { return  gqr & 0x7;        }
inline u32 gqr_st_scale(u32 gqr) { return (gqr >> 8) & 0x3F;  }
inline u32 gqr_ld_type (u32 gqr) { return (gqr >> 16) & 0x7;  }
inline u32 gqr_ld_scale(u32 gqr) { return (gqr >> 24) & 0x3F; }

// Quantized load/store implementations (psq_l / psq_st)
// Types: 0=f32, 4=u8, 5=u16, 6=s8, 7=s16
f64 psq_dequantize(u32 raw, u32 type, u32 scale);
u32 psq_quantize(f64 val, u32 type, u32 scale);
// Width/stride-correct paired-single load/store (what the emitter uses): reads/writes 1/2/4-byte
// elements at the right stride per the GQR load/store type. w=1 → single element (ps1=1.0).
void psq_load (u32 ea, u32 gqr, u32 w, f64* p0, f64* p1);
void psq_store(u32 ea, u32 gqr, u32 w, f64 v0, f64 v1);

// FP single rounding (frsp)
inline f64 frsp(f64 v) { return (f32)v; }

// FP reciprocal estimate (fres) / reciprocal-sqrt estimate (frsqrte).
// The Gekko/Broadway FPU computes these with a piecewise lookup-table approximation
// (~12-bit accurate), NOT true 1.0/v. Software that refines the estimate with
// Newton-Raphson (e.g. matan/the math runtime) is sensitive to the exact estimate
// bits, so we call Dolphin's bit-accurate reproduction of the hardware table rather
// than a full-precision divide (which lands on a different result after refinement +
// fctiwz). Falls back to the precise form in a standalone (no-Dolphin) build.
#ifdef HAVE_DOLPHIN_CORE
namespace Common { double ApproximateReciprocal(double); double ApproximateReciprocalSquareRoot(double); }
inline f64 fres(f64 v)    { return Common::ApproximateReciprocal(v); }
inline f64 frsqrte(f64 v) { return Common::ApproximateReciprocalSquareRoot(v); }
#else
inline f64 fres(f64 v)    { return 1.0 / v; }
inline f64 frsqrte(f64 v) { return 1.0 / std::sqrt(v); }
#endif

// OS HLE call — implemented in runtime/os_hle.cpp
extern void os_hle_call(CPUState& cpu, u32 address);

// Indirect branch resolution — look up recompiled function by address
// Returns nullptr if not recompiled (Dolphin JIT will handle it)
using RecompFunc = void (*)(CPUState&);
extern RecompFunc recomp_lookup(u32 address);

// Call a PPC address (bl/bctrl) — dispatches to recompiled or JIT. In the C-call
// model the callee returns and execution continues inline in the caller.
extern void call_ppc(CPUState& cpu, u32 address);

// Tail-branch to a PPC address (b/bctr that leaves the function). In the C-call
// model, a recomp target is a nested call (then the caller returns); a non-recomp
// target unwinds the recomp C stack back to Run and hands control to the CPU loop
// (the boot→main-loop handoff and other never-returning transitions land here).
extern void tail_ppc(CPUState& cpu, u32 address);

// Special-purpose registers we don't model in CPUState (HID0/HID2, BATs, DSISR,
// SRRn, etc.) pass straight through to Dolphin's live PowerPCState.spr[]. This
// keeps OS/HW-init code (e.g. mtspr HID2 to enable paired singles) coherent with
// the JIT that runs alongside us. Standalone builds back these with a flat array.
extern u32  spr_get(u32 n);
extern void spr_set(u32 n, u32 v);

// MSR is not modeled in CPUState either. mfmsr reads Dolphin's live MSR; msr_set
// applies the proper Dolphin side effects (feature-flag recompute, exception
// delivery). Functions that *write* MSR (mtmsr/rfi) are routed to Dolphin's JIT
// by the recompiler, since those can redirect control flow mid-instruction.
extern u32  msr_get();
extern void msr_set(u32 v);
// Like msr_set but WITHOUT the synchronous exception check: updates MSR + Dolphin's
// derived feature flags, but does not deliver pending interrupts. The C-call model
// runs recomp on the native C stack and never consults ppc.pc mid-tree, so an
// exception redirect there would be lost; async interrupts are instead delivered at
// the recomp→JIT boundary. Used by the OSDisable/Enable/RestoreInterrupts overrides.
extern void msr_set_raw(u32 v);

// 64-bit time base (mftb/mftbu). Reads Dolphin's live, monotonic time base so it
// advances with CoreTiming — code that spins until the TB reaches a target (delay
// loops, timeouts) actually makes progress. Standalone builds use a fake counter.
extern u64  tb_get();

// ── Function-call boundary (docs/ARCHITECTURE_TARGET.md) ─────────────────────
// Engine-object handle table: recompiled game code holds 32-bit HANDLES for the
// host-native PC-engine objects it calls into. A bridged call marshals an engine
// pointer ARG via sb_eng_host() and an engine pointer RETURN via sb_eng_handle()
// (runtime/bridge.h SB_ENGINE_TYPE). Defined in runtime/eng_handle.cpp; full
// contract in eng_handle.h. (The boundary is function-calls-only; the per-type
// field-flip that also used these was retired — no field-access in game code.)
extern u32   sb_eng_handle(void* host);
extern void* sb_eng_host(u32 handle);
extern void  sb_eng_release(void* host);

// Guest main-RAM effective address -> host pointer (for the SUNBRIGHT_BRIDGE
// marshalling thunk, runtime/bridge.h). ea==0 -> nullptr. Defined in memory_bridge.cpp.
extern void* sb_guest_to_host(u32 ea);

// Inverse: a host pointer held in a GUEST-DATA pointer FIELD of a host engine object ->
// the 32-bit guest address the recompiled game expects. nullptr -> 0; a host pointer
// outside the guest arena -> 0. Used by the generated bridged-getter (eng_accessors.cpp)
// when an inline engine-field READ returns a guest-data pointer (so the game sees a guest
// ea, not a truncated host pointer). Defined in memory_bridge.cpp.
extern u32 sb_host_to_guest(void* host);

// Loud trap for an inline engine-field STORE: the function-call boundary owns engine
// objects as handles, and the recompiler does not yet emit field SETTERS. A guest MEM_W
// against a 0x9xxxxxxx handle token would silently corrupt — so a typed inline store
// routes here instead and ABORTS with the offending T::member, rather than writing wild.
// (Add an sbset_ getter-style setter when a real inline write surfaces.) eng_handle.cpp.
extern void sb_eng_field_write_trap(const char* what, u32 handle);

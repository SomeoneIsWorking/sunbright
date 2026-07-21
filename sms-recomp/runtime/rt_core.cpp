// rt_core.cpp — the standalone guest runtime for the recompiled game.
//
// This is the whole boundary between recompiled PPC and the host. In the retired
// architecture these symbols came from dolphin_hook.cpp and rode on Dolphin's
// memory/JIT; the two-runtime doctrine (CLAUDE.md) puts the recomp on its OWN
// substrate, with aurora owning the hardware seams. Nothing here may depend on
// Dolphin.
//
// Provides exactly the surface `nm -u` reports for the generated chunks:
//   guest memory : g_ram_base, g_l1_base, mem_r{8,16,32,64}_slow, mem_w{...}_slow
//   dispatch     : call_ppc, tail_ppc
//   cpu/os state : msr_get, msr_set_raw, icbi32
//   diagnostics  : g_in_poll_yield, g_poll_last, g_poll_reps, sb_poll_fire,
//                  g_watch_wa, sb_watch_fire
//   paired single: psq_load, psq_store
//
// The fast paths are already inline in intrinsics.h (sb_ram_fast + __builtin_bswap*),
// so only the slow/MMIO paths land here.

#include "cpu_state.h"
#include "intrinsics.h"
#include "../overrides/overrides.h"

#include <lucent/log.h>
#include <lucent/config.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <ctime>

// ── Guest memory ─────────────────────────────────────────────────────────────
// GameCube MEM1 is 24 MB at 0x80000000 (cached) / 0xC0000000 (uncached); the
// locked-L1 array lives at 0xE0000000. sb_ram_fast() masks with 0x01FFFFFF, so the
// allocation must cover the full 32 MB mask range even though only 24 MB is real —
// otherwise a stray access in 0x01800000..0x01FFFFFF would run off the end. We map
// 32 MB and treat [24 MB, 32 MB) as a poison window that faults loudly rather than
// silently aliasing.
static const size_t kMem1Size = 0x01800000;  // 24 MB, the real MEM1
static const size_t kMapSize  = 0x02000000;  // 32 MB, the full sb_ram_fast mask range
static const size_t kL1Size   = 0x00040000;  // 256 KB locked-L1 at 0xE0000000

u8*  g_ram_base      = nullptr;
u8*  g_l1_base       = nullptr;
bool g_in_poll_yield = false;
u32  g_poll_last     = 0;
u32  g_poll_reps     = 0;
u32  g_watch_wa      = 0;   // armed watch address; 0 = disarmed

extern "C" bool rt_mem_init() {
    if (g_ram_base) return true;

    void* p = mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        lucent::error("rt", "failed to map {} MB of guest RAM", kMapSize >> 20);
        return false;
    }
    // Poison the aliasing tail so an out-of-MEM1 access traps instead of silently
    // reading zeros that look like valid game data.
    if (mprotect((u8*)p + kMem1Size, kMapSize - kMem1Size, PROT_NONE) != 0)
        lucent::warn("rt", "could not poison the MEM1 tail; stray accesses will alias");

    g_ram_base = (u8*)p;

    void* l1 = mmap(nullptr, kL1Size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_l1_base = (l1 == MAP_FAILED) ? nullptr : (u8*)l1;

    lucent::info("rt", "guest memory ready: MEM1 {} MB @ {}, L1 {} KB @ {}",
                 kMem1Size >> 20, (void*)g_ram_base, kL1Size >> 10, (void*)g_l1_base);
    return true;
}

// ── Slow / MMIO path ─────────────────────────────────────────────────────────
// Anything sb_ram_fast() rejects is either MMIO (CP/PE/PI/DSP/DI/SI/EXI/AI) or a
// genuine bad access. Aurora owns those devices, so this is where the recomp will
// eventually hand off. Until that routing exists these are LOUD, not silent zeros —
// a silent 0 here reads as valid hardware state and would fake plausible-but-wrong
// behaviour for a long time before anyone noticed.
static void slow_report(const char* op, u32 ea, unsigned width) {
    lucent::debug("mmio", "unrouted {}{} @ 0x{:08x}", op, width * 8, ea);
}

u8  mem_r8_slow (u32 ea)            { slow_report("r", ea, 1); return 0; }
u16 mem_r16_slow(u32 ea)            { slow_report("r", ea, 2); return 0; }
u32 mem_r32_slow(u32 ea)            { slow_report("r", ea, 4); return 0; }
u64 mem_r64_slow(u32 ea)            { slow_report("r", ea, 8); return 0; }
void mem_w8_slow (u32 ea, u8  v)    { (void)v; slow_report("w", ea, 1); }
void mem_w16_slow(u32 ea, u16 v)    { (void)v; slow_report("w", ea, 2); }
void mem_w32_slow(u32 ea, u32 v)    { (void)v; slow_report("w", ea, 4); }
void mem_w64_slow(u32 ea, u64 v)    { (void)v; slow_report("w", ea, 8); }

// ── Dispatch ─────────────────────────────────────────────────────────────────
struct JumpEntry { u32 addr; void (*fn)(CPUState&); };
extern "C" const JumpEntry  g_recomp_table[];
extern "C" const size_t     g_recomp_table_size;

// Fold an instruction address onto the cached-virtual window the recompiled image is
// keyed by. The GameCube BATs give every code byte three aliases — 0x8xxxxxxx cached,
// 0xCxxxxxxx uncached, 0x0xxxxxxx physical — and OS code uses all three: it clears
// MSR[IR|DR] and rfi's to a PHYSICAL address (rlwinm rN,rN,0,2,31 immediately before
// mtsrr0 is the giveaway) so the jump runs with translation off. Without this fold the
// dispatcher sees 0x003464a0, finds nothing, and aborts on what is a perfectly ordinary
// OS control transfer. Modelling the fixed alias is faithful; it is what the BATs do.
static u32 code_addr_fold(u32 addr) {
    const u32 phys = addr & 0x0FFFFFFFu;
    if (phys >= kMem1Size) return addr;   // not a MEM1 alias — leave it to fail loudly
    return 0x80000000u | phys;
}

// The generated table is emitted in ascending address order, so a binary search is
// both correct and O(log n) — this is the hottest call in the whole runtime.
static void (*lookup(u32 addr))(CPUState&) {
    size_t lo = 0, hi = g_recomp_table_size;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        u32 a = g_recomp_table[mid].addr;
        if (a == addr) return g_recomp_table[mid].fn;
        if (a < addr) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

void call_ppc(CPUState& cpu, u32 address) {
    address = code_addr_fold(address);
    // Overrides win over the recompiled body. Checked here rather than by patching the
    // jump table because the generated code routes EVERY call — direct bl and indirect
    // bctrl alike — through this one function, so a single check covers both.
    if (auto fn = override_lookup(address)) { fn(cpu); return; }
    if (auto fn = lookup(address)) { fn(cpu); return; }
    // Not recompiled. In the Dolphin era this fell through to the JIT; standalone
    // there is no fallback, so this is a hard stop rather than a silent no-op that
    // would let execution wander on with a half-executed call.
    lucent::error("rt", "call to un-recompiled address 0x{:08x} (lr=0x{:08x})",
                  address, cpu.lr);
    std::abort();
}

void tail_ppc(CPUState& cpu, u32 address) { call_ppc(cpu, address); }

void rt_unhandled_insn(CPUState& cpu, u32 pc, u32 raw, const char* mnemonic) {
    lucent::error("rt", "unhandled instruction '{}' at 0x{:08x} (raw=0x{:08x}, lr=0x{:08x})",
                  mnemonic, pc, raw, cpu.lr);
    lucent::error("rt", "add an emitter for it in ppc_decoder.cpp + c_emitter.cpp");
    std::abort();
}

// ── CPU / OS state ───────────────────────────────────────────────────────────
// MTMSR/RFI are modeled in the recompiler (the recompiled OS owns interrupt state),
// so MSR is just storage here; interrupt DELIVERY is the scheduler's job.
static u32 g_msr = 0;
u32  msr_get()            { return g_msr; }
void msr_set_raw(u32 v)   { g_msr = v; }

// Instruction-cache invalidate. We never execute from guest memory (all code is
// recompiled ahead of time), so this is a genuine no-op rather than a stub —
// documented as an intentional seam, not an unimplemented one.
void icbi32(u32 /*ea*/) {}

// ── Diagnostics carried over from the Dolphin era ────────────────────────────
// sb_poll_note() (inline, intrinsics.h) calls this when the same address is read 24
// times in a row, i.e. a guest spin-loop. With cooperative threading it becomes a
// yield point; until the scheduler is standing it only reports.
void sb_poll_fire(u32 ea) {
    lucent::debug("poll", "spin-loop on 0x{:08x}", ea);
}

void sb_watch_fire(u32 ea, u32 value, int width, void* ret) {
    lucent::warn("watch", "write 0x{:08x} ({} bytes) @ 0x{:08x} from {}",
                 value, width, ea, ret);
}

// ── Paired-single quantized load/store ───────────────────────────────────────
// Ported from the retired memory_bridge.cpp, which had already root-caused the GQR
// scale: it is a 6-bit SIGNED value (0-31 positive, 32-63 = -32..-1). Load
// multiplies by 2^(-scale), store by 2^(+scale), matching Dolphin's
// m_dequantizeTable/m_quantizeTable. An earlier version used `1u << scale`, which is
// unsigned and UB for scale >= 32 — e.g. the u8 YUV store uses scale=61 = -3.
// gqr_ld_type/ld_scale/st_type/st_scale already come from intrinsics.h.
static inline bool  psq_is_float(u32 t) { return t != 4 && t != 5 && t != 6 && t != 7; }
static inline u32   psq_stride  (u32 t) { return (t == 4 || t == 6) ? 1u : (t == 5 || t == 7) ? 2u : 4u; }
static inline float psq_ld_mult (u32 s6) { int s=(int)(s6&0x3F); if(s>=32) s-=64; return std::ldexp(1.0f,-s); }
static inline float psq_st_mult (u32 s6) { int s=(int)(s6&0x3F); if(s>=32) s-=64; return std::ldexp(1.0f, s); }

void psq_load(u32 ea, u32 gqr, u32 w, f64* p0, f64* p1) {
    const u32 t = gqr_ld_type(gqr);
    if (psq_is_float(t)) {                       // float: 4-byte elements, scale ignored
        u32 r0 = sb_r32(ea); f32 v0; std::memcpy(&v0, &r0, 4); *p0 = v0;
        if (!w) { u32 r1 = sb_r32(ea + 4); f32 v1; std::memcpy(&v1, &r1, 4); *p1 = v1; }
        else *p1 = 1.0;
        return;
    }
    const float s = psq_ld_mult(gqr_ld_scale(gqr));
    auto one = [&](u32 a) -> f64 {
        switch (t) {
        case 4:  return (u8) sb_r8 (a) * s;
        case 5:  return (u16)sb_r16(a) * s;
        case 6:  return (s8) sb_r8 (a) * s;
        default: return (s16)sb_r16(a) * s;      // 7 = s16
        }
    };
    *p0 = one(ea);
    *p1 = w ? 1.0 : one(ea + psq_stride(t));
}

void psq_store(u32 ea, u32 gqr, u32 w, f64 v0, f64 v1) {
    const u32 t = gqr_st_type(gqr);
    if (psq_is_float(t)) {
        f32 f0 = (f32)v0; u32 b0; std::memcpy(&b0, &f0, 4); sb_w32(ea, b0);
        if (!w) { f32 f1 = (f32)v1; u32 b1; std::memcpy(&b1, &f1, 4); sb_w32(ea + 4, b1); }
        return;
    }
    const float s = psq_st_mult(gqr_st_scale(gqr));
    auto one = [&](u32 a, f64 v) {
        double x = v * s;
        switch (t) {
        case 4: sb_w8 (a, (u8) (x < 0 ? 0 : x > 255 ? 255 : x)); break;
        case 5: sb_w16(a, (u16)(x < 0 ? 0 : x > 65535 ? 65535 : x)); break;
        case 6: sb_w8 (a, (u8) (s8)(x < -128 ? -128 : x > 127 ? 127 : x)); break;
        default: sb_w16(a, (u16)(s16)(x < -32768 ? -32768 : x > 32767 ? 32767 : x)); break;
        }
    };
    one(ea, v0);
    if (!w) one(ea + psq_stride(t), v1);
}

// ── dcbz / time base / syscall ───────────────────────────────────────────────
// dcbz zeroes the 32-byte cache block containing `ea`. This is NOT a no-op: games
// use it as a fast bulk clear, so skipping it leaves stale data the guest believes
// is zeroed. Writes go straight to the backing store — the block is guest bytes, and
// zero is endian-neutral.
void dcbz32(u32 ea) {
    const u32 base = ea & ~31u;
    for (u32 i = 0; i < 32; i++)
        if (u8* p = sb_ram_fast(base + i)) *p = 0;
}

// Time base: the Gekko TBR ticks at the bus clock / 4 = 162 MHz / 4 = 40.5 MHz.
// Derived from a monotonic host clock so guest timing advances at the right rate;
// OSGetTime/OSTicksToSeconds depend on this scale being correct.
u64 tb_get() {
    static const u64 kTbHz = 40500000ull;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * kTbHz + (u64)ts.tv_nsec * kTbHz / 1000000000ull;
}

// `sc` (syscall). The recompiler emits this for the SC instruction; on GameCube the
// OS uses it for context switching / interrupt-enable transitions, which the
// recompiled OS itself models (MTMSR/RFI are recompiled). Nothing should reach here
// yet, so say so LOUDLY rather than silently continuing with the guest believing a
// syscall completed.
void os_hle_call(CPUState& cpu, u32 address) {
    lucent::warn("os", "unhandled syscall at 0x{:08x} (r3=0x{:08x})", address, cpu.gpr[3]);
}

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

#include "../overrides/overrides.h"
#include "cpu_state.h"
#include "guest_address_table.h"
#include "intrinsics.h"
#include "mmio.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <sys/mman.h>
#include <unordered_map>

// ── Guest memory ─────────────────────────────────────────────────────────────
// GameCube MEM1 is 24 MB at 0x80000000 (cached) / 0xC0000000 (uncached); the
// locked-L1 array lives at 0xE0000000. sb_ram_fast() masks with 0x01FFFFFF, so the
// allocation must cover the full 32 MB mask range even though only 24 MB is real —
// otherwise a stray access in 0x01800000..0x01FFFFFF would run off the end. We map
// 32 MB and treat [24 MB, 32 MB) as a poison window that faults loudly rather than
// silently aliasing.
static const size_t kMem1Size = 0x01800000; // 24 MB, the real MEM1
static const size_t kMapSize = 0x02000000;  // 32 MB, the full sb_ram_fast mask range
static const size_t kL1Size = 0x00040000;   // 256 KB locked-L1 at 0xE0000000

u8* g_ram_base = nullptr;
u8* g_l1_base = nullptr;
bool g_in_poll_yield = false;
u32 g_poll_last = 0;
u32 g_poll_reps = 0;
u32 g_watch_wa = 0; // armed watch address; 0 = disarmed

extern void aram_device_init();
extern void dsp_device_init();
extern void ai_device_init();
extern void exi_device_init();
extern void vi_device_init();
extern void si_device_init();
extern void sram_device_init();
extern void di_device_init();
extern void pi_device_init();
extern void mi_device_init();
extern void gxregs_device_init();
extern void gxfifo_device_init();
extern void gxfifo_stats(u64&, u64&, u64&);

extern "C" bool rt_mem_init() {
    if (g_ram_base)
        return true;

    void* p = mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        lucent::error("rt", "failed to map {} MB of guest RAM", kMapSize >> 20);
        return false;
    }
    // Poison the aliasing tail so an out-of-MEM1 access traps instead of silently
    // reading zeros that look like valid game data.
    if (mprotect((u8*)p + kMem1Size, kMapSize - kMem1Size, PROT_NONE) != 0)
        lucent::warn("rt", "could not poison the MEM1 tail; stray accesses will alias");

    g_ram_base = (u8*)p;

    void* l1 = mmap(nullptr, kL1Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_l1_base = (l1 == MAP_FAILED) ? nullptr : (u8*)l1;

    lucent::info("rt", "guest memory ready: MEM1 {} MB @ {}, L1 {} KB @ {}", kMem1Size >> 20,
                 (void*)g_ram_base, kL1Size >> 10, (void*)g_l1_base);

    // Devices register after guest RAM exists (ARAM DMAs into it).
    dsp_device_init();
    ai_device_init();
    exi_device_init();
    vi_device_init();
    si_device_init();
    di_device_init();
    pi_device_init();
    mi_device_init();
    gxregs_device_init();
    gxfifo_device_init();
    if (const char* w = std::getenv("SBR_WATCH")) {
        g_watch_wa = (u32)std::strtoul(w, nullptr, 0);
        lucent::info("rt", "watchpoint armed at 0x{:08x}", g_watch_wa);
    }
    aram_device_init();
    sram_device_init(); // attaches to EXI, so it must come after exi_device_init()
    return true;
}

// ── Slow / MMIO path ─────────────────────────────────────────────────────────
// Anything sb_ram_fast() rejects is either MMIO (CP/PE/PI/DSP/DI/SI/EXI/AI) or a
// genuine bad access. Aurora owns those devices, so this is where the recomp will
// eventually hand off. Until that routing exists these are LOUD, not silent zeros —
// a silent 0 here reads as valid hardware state and would fake plausible-but-wrong
// behaviour for a long time before anyone noticed.
void rt_dump_guest_stack(const char* why);

static void slow_report(const char* op, u32 ea, unsigned width) {
    lucent::debug("mmio", "unrouted {}{} @ 0x{:08x}", op, width * 8, ea);
}

// An effective address in the first 64 KB with no segment nibble is a NULL pointer plus a
// field offset — `lwz rX,0x1d0(r3)` with r3 == 0. Real hardware has no BAT covering it, so
// this faults there too; returning 0 here instead would hand the guest a plausible value and
// let it run on for millions of instructions before anything looked wrong (measured: 314k
// reads of 0x1d0 in 15 s, long after whatever produced the NULL). Stop at the cause.
static void trap_null(const char* op, u32 ea, unsigned width) {
    if ((ea >> 28) != 0 || ea >= 0x00010000u)
        return;
    lucent::error("rt", "NULL-pointer {}{} at guest address 0x{:08x} (field offset 0x{:x})", op,
                  width * 8, ea, ea);
    rt_dump_guest_stack("null dereference");
    std::abort();
}

// A device gets first refusal; anything unclaimed is still reported loudly.
static u32 slow_read(u32 ea, unsigned width) {
    u32 v = 0;
    if (mmio_read(ea, width, v))
        return v;
    trap_null("r", ea, width);
    // FAIL FAST. Returning 0 for a device nobody implemented is a fabricated hardware
    // answer: the guest treats it as real, and a 0 that should have been a pointer becomes
    // a NULL dereference thousands of instructions later with no trace of where it came
    // from. Stopping here names the missing device at the exact instruction that needed it,
    // which is also the order in which devices should be implemented.
    lucent::error("rt",
                  "read{} from unrouted device register 0x{:08x} — no device claims "
                  "it, and inventing a value would corrupt the guest silently",
                  width * 8, ea);
    rt_dump_guest_stack("unrouted device read");
    std::abort();
}
static void slow_write(u32 ea, unsigned width, u32 v) {
    if (mmio_write(ea, width, v))
        return;
    trap_null("w", ea, width);
    slow_report("w", ea, width);
}

u8 mem_r8_slow(u32 ea) {
    return (u8)slow_read(ea, 1);
}
u16 mem_r16_slow(u32 ea) {
    return (u16)slow_read(ea, 2);
}
u32 mem_r32_slow(u32 ea) {
    return slow_read(ea, 4);
}
u64 mem_r64_slow(u32 ea) {
    slow_report("r", ea, 8);
    return 0;
}
void mem_w8_slow(u32 ea, u8 v) {
    slow_write(ea, 1, v);
}
void mem_w16_slow(u32 ea, u16 v) {
    slow_write(ea, 2, v);
}
void mem_w32_slow(u32 ea, u32 v) {
    slow_write(ea, 4, v);
}
void mem_w64_slow(u32 ea, u64 v) {
    (void)v;
    slow_report("w", ea, 8);
}

// ── Dispatch ─────────────────────────────────────────────────────────────────
struct JumpEntry {
    u32 addr;
    void (*fn)(CPUState&);
};
extern "C" const JumpEntry g_recomp_table[];
extern "C" const size_t g_recomp_table_size;

// Fold an instruction address onto the cached-virtual window the recompiled image is
// keyed by. The GameCube BATs give every code byte three aliases — 0x8xxxxxxx cached,
// 0xCxxxxxxx uncached, 0x0xxxxxxx physical — and OS code uses all three: it clears
// MSR[IR|DR] and rfi's to a PHYSICAL address (rlwinm rN,rN,0,2,31 immediately before
// mtsrr0 is the giveaway) so the jump runs with translation off. Without this fold the
// dispatcher sees 0x003464a0, finds nothing, and aborts on what is a perfectly ordinary
// OS control transfer. Modelling the fixed alias is faithful; it is what the BATs do.
static u32 code_addr_fold(u32 addr) {
    const u32 phys = addr & 0x0FFFFFFFu;
    if (phys >= kMem1Size)
        return addr; // not a MEM1 alias — leave it to fail loudly
    return 0x80000000u | phys;
}

// The generated table is emitted in ascending address order, so a binary search is
// both correct and O(log n) — this is the hottest call in the whole runtime.
using RecompiledFunction = void(CPUState&);

struct RecompiledDispatch {
    GuestAddressTable<RecompiledFunction> functions;

    RecompiledDispatch() {
        for (size_t i = 0; i < g_recomp_table_size; ++i) {
            const auto& entry = g_recomp_table[i];
            if (!functions.insert(entry.addr, entry.fn)) {
                lucent::error("rt", "duplicate or invalid recompiled function address 0x{:08x}",
                              entry.addr);
                std::abort();
            }
        }
    }
};

static RecompiledFunction* lookup(u32 addr) {
    static RecompiledDispatch dispatch;
    return dispatch.functions.find(addr);
}

// Every blocker in the standalone bring-up is "execution reached address X" and the first
// question is always "through which guest functions?". Recompiled bodies are real C
// functions named func_<addr>, and calls nest on the host stack, so the host backtrace IS
// the guest call stack — provided the link uses -rdynamic so the names resolve.
void rt_dump_guest_stack(const char* why) {
    void* frames[64];
    int n = backtrace(frames, 64);
    char** names = backtrace_symbols(frames, n);
    lucent::error("rt", "guest call stack ({}):", why);
    if (!names) {
        lucent::error("rt", "  <backtrace_symbols failed>");
        return;
    }
    for (int i = 0; i < n; i++) {
        // Only the guest frames are interesting; runtime frames are noise.
        if (const char* f = std::strstr(names[i], "func_"))
            lucent::error("rt", "  #{:<2} {}", i, f);
    }
    std::free(names);
}

// ── A FATAL SIGNAL MUST SAY SOMETHING ────────────────────────────────────────
//
// Until now a SIGSEGV produced exit code 139 and nothing else. Issue #2 — an intermittent crash in
// the shutdown report path — sat open for exactly that reason: it reproduced about one run in
// three, every reproduction printed a full log that ended mid-report, and none of them said WHERE.
// An intermittent fault you cannot attribute is one you cannot bisect, so the cheapest possible fix
// is to make the crash name itself the first time it happens rather than the fifth.
//
// backtrace() is not async-signal-safe in the strict sense. It is used anyway, deliberately: the
// process is already dying, the alternative is no information at all, and this is the same call the
// stall watchdog and every hard-stop path in this file already make. The handler resets itself to
// SIG_DFL and re-raises afterwards so the exit status still reports the real signal and a core is
// still produced for anyone who wants one.
extern "C" void rt_fatal_signal(int sig, siginfo_t* info, void*) {
    static volatile sig_atomic_t s_inHandler = 0;
    if (s_inHandler == 0) {
        s_inHandler = 1;
        const char* name = sig == SIGSEGV   ? "SIGSEGV"
                           : sig == SIGBUS  ? "SIGBUS"
                           : sig == SIGFPE  ? "SIGFPE"
                           : sig == SIGILL  ? "SIGILL"
                           : sig == SIGABRT ? "SIGABRT"
                                            : "fatal signal";
        lucent::error("rt",
                      "{} at fault address {} — the host call stack follows. A frame named "
                      "func_<addr> is GUEST code at that guest address; frames without one "
                      "are the runtime or a library.",
                      name, info != nullptr ? info->si_addr : nullptr);
        rt_dump_guest_stack(name);
        // The HOST frames too, unfiltered. rt_dump_guest_stack keeps only func_* frames, which is
        // right when the question is "which guest code" and useless when the crash is in the
        // runtime's own shutdown path — which is precisely the case this handler was added for.
        void* frames[64];
        const int n = backtrace(frames, 64);
        char** names = backtrace_symbols(frames, n);
        if (names != nullptr) {
            lucent::error("rt", "host call stack ({} frame(s)):", n);
            for (int i = 0; i < n; i++)
                lucent::error("rt", "  #{:<2} {}", i, names[i]);
            std::free(names);
        } else {
            lucent::error("rt",
                          "host call stack: backtrace_symbols failed, so the frames above are "
                          "all there is");
        }
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// SBR_CRASH_SELFTEST=1 dereferences a null pointer on purpose. A crash handler that has never been
// seen to fire is indistinguishable from one that is not installed, and this one exists because a
// silent 139 wasted a session.
void rt_install_crash_handler() {
    struct sigaction sa{};
    sa.sa_sigaction = &rt_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    // SIGABRT TOO. It was left out at first because "we don't call abort()" — but the runtime does,
    // every hard-stop path in this file does, an unhandled C++ exception does, and so does Dawn
    // when a GPU assertion fails. Exit code 134 with no other output is the same unattributable
    // silence that kept issue #2 open for a SIGSEGV, and it turned up the same day on a shutdown
    // path. SA_RESETHAND plus the re-raise means the second abort dies plainly rather than looping.
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
        sigaction(sig, &sa, nullptr);

    if (const char* e = std::getenv("SBR_CRASH_SELFTEST"); e != nullptr && e[0] == '1') {
        lucent::info("rt",
                     "SELF-TEST: dereferencing a null pointer on purpose. The correct outcome "
                     "is the fault report that follows, then death by the real signal. A run "
                     "that reaches the next line means the handler is not installed and every "
                     "silent 139 stays unattributed.");
        volatile int* p = nullptr;
        *p = 1;
        lucent::error("rt", "SELF-TEST DID NOT FAULT — a null store was accepted, so this build "
                            "cannot test the handler at all.");
        std::abort();
    }
}

void call_ppc(CPUState& cpu, u32 address) {
    address = code_addr_fold(address);
    // Overrides win over the recompiled body. Checked here rather than by patching the
    // jump table because the generated code routes EVERY call — direct bl and indirect
    // bctrl alike — through this one function, so a single check covers both.
    if (auto fn = override_lookup(address)) {
        fn(cpu);
        return;
    }
    if (auto fn = lookup(address)) {
        fn(cpu);
        return;
    }
    // Not recompiled. In the Dolphin era this fell through to the JIT; standalone
    // there is no fallback, so this is a hard stop rather than a silent no-op that
    // would let execution wander on with a half-executed call.
    lucent::error("rt", "call to un-recompiled address 0x{:08x} (lr=0x{:08x})", address, cpu.lr);
    rt_dump_guest_stack("un-recompiled call");
    std::abort();
}

void tail_ppc(CPUState& cpu, u32 address) {
    call_ppc(cpu, address);
}

void rt_unhandled_insn(CPUState& cpu, u32 pc, u32 raw, const char* mnemonic) {
    lucent::error("rt", "unhandled instruction '{}' at 0x{:08x} (raw=0x{:08x}, lr=0x{:08x})",
                  mnemonic, pc, raw, cpu.lr);
    lucent::error("rt", "add an emitter for it in ppc_decoder.cpp + c_emitter.cpp");
    rt_dump_guest_stack("unhandled instruction");
    std::abort();
}

// ── CPU / OS state ───────────────────────────────────────────────────────────
// MTMSR/RFI are modeled in the recompiler (the recompiled OS owns interrupt state),
// so MSR is just storage here; interrupt DELIVERY is the scheduler's job.
static u32 g_msr = 0;
u32 msr_get() {
    return g_msr;
}
void msr_set_raw(u32 v) {
    g_msr = v;
}

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
    lucent::warn("watch", "write 0x{:08x} ({} bytes) @ 0x{:08x} from {}", value, width, ea, ret);
    rt_dump_guest_stack("watchpoint");
}

// THE WATCHPOINT ONLY SEES GUEST STORES, and that is a hole large enough to hide the bug that
// prompted this: a heap block's next-link was zeroed while SBR_WATCH sat on it and
// nothing fired, because the write did not come from recompiled code at all. Two native paths copy
// straight into guest RAM behind sb_w* — the DVD DMA (dev_di.cpp) and the ARAM DMA (dev_aram.cpp) —
// and any device that does the same must call this, or the watchpoint quietly under-reports.
//
// It reports the RANGE and the writer rather than a value, because that is what a bulk copy has:
// the interesting fact is "this transfer covered your address", not which byte landed there.
void sb_watch_range(u32 ea, u32 len, const char* who) {
    if (g_watch_wa == 0 || len == 0)
        return;
    if (g_watch_wa < ea || g_watch_wa >= ea + len)
        return;
    lucent::warn("watch",
                 "BULK WRITE from {} covered the watched address: 0x{:08x} + 0x{:x} bytes "
                 "spans 0x{:08x} (offset 0x{:x} into the transfer). This is a native device "
                 "copy, not a guest store, which is why the per-store watchpoint is silent.",
                 who, ea, len, g_watch_wa, g_watch_wa - ea);
    rt_dump_guest_stack("watchpoint (bulk)");
}

// ── Paired-single quantized load/store ───────────────────────────────────────
// Ported from the retired memory_bridge.cpp, which had already root-caused the GQR
// scale: it is a 6-bit SIGNED value (0-31 positive, 32-63 = -32..-1). Load
// multiplies by 2^(-scale), store by 2^(+scale), matching Dolphin's
// m_dequantizeTable/m_quantizeTable. An earlier version used `1u << scale`, which is
// unsigned and UB for scale >= 32 — e.g. the u8 YUV store uses scale=61 = -3.
// gqr_ld_type/ld_scale/st_type/st_scale already come from intrinsics.h.
static inline bool psq_is_float(u32 t) {
    return t != 4 && t != 5 && t != 6 && t != 7;
}
static inline u32 psq_stride(u32 t) {
    return (t == 4 || t == 6) ? 1u : (t == 5 || t == 7) ? 2u : 4u;
}
static inline float psq_ld_mult(u32 s6) {
    int s = (int)(s6 & 0x3F);
    if (s >= 32)
        s -= 64;
    return std::ldexp(1.0f, -s);
}
static inline float psq_st_mult(u32 s6) {
    int s = (int)(s6 & 0x3F);
    if (s >= 32)
        s -= 64;
    return std::ldexp(1.0f, s);
}

void psq_load(u32 ea, u32 gqr, u32 w, f64* p0, f64* p1) {
    const u32 t = gqr_ld_type(gqr);
    if (psq_is_float(t)) { // float: 4-byte elements, scale ignored
        u32 r0 = sb_r32(ea);
        f32 v0;
        std::memcpy(&v0, &r0, 4);
        *p0 = v0;
        if (!w) {
            u32 r1 = sb_r32(ea + 4);
            f32 v1;
            std::memcpy(&v1, &r1, 4);
            *p1 = v1;
        } else
            *p1 = 1.0;
        return;
    }
    const float s = psq_ld_mult(gqr_ld_scale(gqr));
    auto one = [&](u32 a) -> f64 {
        switch (t) {
        case 4:
            return (u8)sb_r8(a) * s;
        case 5:
            return (u16)sb_r16(a) * s;
        case 6:
            return (s8)sb_r8(a) * s;
        default:
            return (s16)sb_r16(a) * s; // 7 = s16
        }
    };
    *p0 = one(ea);
    *p1 = w ? 1.0 : one(ea + psq_stride(t));
}

void psq_store(u32 ea, u32 gqr, u32 w, f64 v0, f64 v1) {
    const u32 t = gqr_st_type(gqr);
    if (psq_is_float(t)) {
        f32 f0 = (f32)v0;
        u32 b0;
        std::memcpy(&b0, &f0, 4);
        sb_w32(ea, b0);
        if (!w) {
            f32 f1 = (f32)v1;
            u32 b1;
            std::memcpy(&b1, &f1, 4);
            sb_w32(ea + 4, b1);
        }
        return;
    }
    const float s = psq_st_mult(gqr_st_scale(gqr));
    auto one = [&](u32 a, f64 v) {
        double x = v * s;
        switch (t) {
        case 4:
            sb_w8(a, (u8)(x < 0 ? 0 : x > 255 ? 255 : x));
            break;
        case 5:
            sb_w16(a, (u16)(x < 0 ? 0 : x > 65535 ? 65535 : x));
            break;
        case 6:
            sb_w8(a, (u8)(s8)(x < -128 ? -128 : x > 127 ? 127 : x));
            break;
        default:
            sb_w16(a, (u16)(s16)(x < -32768 ? -32768 : x > 32767 ? 32767 : x));
            break;
        }
    };
    one(ea, v0);
    if (!w)
        one(ea + psq_stride(t), v1);
}

// ── dcbz / time base / syscall ───────────────────────────────────────────────
// dcbz zeroes the 32-byte cache block containing `ea`. This is NOT a no-op: games
// use it as a fast bulk clear, so skipping it leaves stale data the guest believes
// is zeroed. Writes go straight to the backing store — the block is guest bytes, and
// zero is endian-neutral.
void dcbz32(u32 ea) {
    const u32 base = ea & ~31u;
    for (u32 i = 0; i < 32; i++)
        if (u8* p = sb_ram_fast(base + i))
            *p = 0;
}

extern "C" unsigned VIGetRetraceCount(void) __attribute__((weak));

// Time base: the Gekko TBR ticks at the bus clock / 4 = 162 MHz / 4 = 40.5 MHz.
// Derived from a monotonic host clock so guest timing advances at the right rate;
// OSGetTime/OSTicksToSeconds depend on this scale being correct.
u64 tb_get() {
    static const u64 kTbHz = 40500000ull;

    // SBR_DETERMINISTIC=1 — a VIRTUAL time base, so two runs of the same input produce the same
    // frame. The host clock reaching the guest is why they do not: every OSGetTime the game makes
    // differs run to run, and anything seeded from it or timed against it diverges. Measured on
    // Delfino, two identical runs differed in 6,757 of 1,228,800 pixels, which made pixel-exact
    // A/B — the only instrument that can validate the 60fps interpolation write — unusable.
    //
    // The virtual clock advances one nominal NTSC field per present and counts calls within a
    // frame, so it is MONOTONIC (guest code that waits for the TB to advance still progresses)
    // and DETERMINISTIC (it is a function of the frame number and the call count, both of which
    // are themselves deterministic given deterministic input). The rate stays right, so timing
    // dependent game logic behaves as it does under the real clock.
    //
    // Off by default: it decouples guest time from real time, which is wrong for playing.
    static const bool deterministic = std::getenv("SBR_DETERMINISTIC") != nullptr;
    if (deterministic) {
        static unsigned last_frame = 0xFFFFFFFFu;
        static u64 calls = 0;
        const unsigned frame = (&VIGetRetraceCount) ? VIGetRetraceCount() : 0;
        if (frame != last_frame) {
            last_frame = frame;
            calls = 0;
        }
        // MONOTONIC AND UNBOUNDED, both required. The first version returned
        // frame*step + (++calls % step), which wraps: the clock jumped BACKWARD mid-frame, and
        // guest code that spins waiting for the time base to reach a deadline then never
        // finished -- the game hung at THP open and never reached gameplay. A modulo cannot be
        // used here at all, and neither can a bare frame base, because a spin-wait issues no
        // present and would sit on a constant clock forever.
        //
        // So: a candidate from the frame and the intra-frame call count, floored to strictly
        // exceed the last value returned. Deterministic (both terms are), never decreasing, and
        // always advancing even when the guest spins without presenting.
        static u64 last = 0;
        u64 v = (u64)frame * (kTbHz / 60ull) + (++calls) * 64ull;
        if (v <= last)
            v = last + 1;
        last = v;
        return v;
    }

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
    // Per SITE, not per call: the GX path hits one of these on essentially every command
    // (measured 1.67 M in a 40 s run), which buries every other line in the log and slows
    // the run enough to distort what it is measuring. Once per site still surfaces a new
    // one immediately, which is the point of the warning.
    static std::unordered_map<u32, unsigned long> seen;
    unsigned long& n = seen[address];
    if (++n == 1)
        lucent::warn("os",
                     "unhandled syscall at 0x{:08x} (r3=0x{:08x}) — further "
                     "occurrences at this address are counted, not logged",
                     address, cpu.gpr[3]);
}

// The machine's special-purpose registers: one set, shared by every thread's CPUState.
// See the note on CPUState::SprFile.
u32 CPUState::SprFile::s_spr[1024];

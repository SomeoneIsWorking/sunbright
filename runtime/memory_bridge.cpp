#include <cmath>
#include "memory_bridge.h"
#include "cpu_state.h"
#include "gx_stream.h"
#include "intrinsics.h"   // inline fast-path sb_r*/sb_w* + sb_poll_note (and the mem_* decls we define)
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <execinfo.h>
#include <sys/resource.h>

bool g_recomp_touched_mmio = false;
bool g_recomp_context_switched = false;
thread_local CPUState* g_cur_recomp_cpu = nullptr;

// ── GX matrix-load tap (SUNBRIGHT_GXCAP=1) ───────────────────────────────────
// Every model's transform reaches the GPU as an XF matrix-load through the write-
// gather pipe, which we own. GXLoadPosMtxImm/TexMtxImm emit the byte stream
//   [0x10] [u32 token = ((count-1)<<16)|xfAddr] [count × u32]
// A 12-word load (token>>16 == 0xB) to matrix memory (xfAddr < 0x100) is a 3x4
// position/texture matrix. We watch the gather-pipe writes and reconstruct them —
// no need for the exact GXLoadPosMtxImm address. This is the per-frame transform
// stream the motion interpolator will consume (see docs/model_interpolation.md).
namespace {
bool        g_gxcap = getenv("SUNBRIGHT_GXCAP") != nullptr;
int         g_gx_state = 0;     // 0 idle, 1 saw-0x10, 2 collecting words
int         g_gx_idx = 0;
u32         g_gx_addr = 0;
u32         g_gx_words[12];
unsigned long g_gx_total = 0;

// SUNBRIGHT_DBG_FIFO: count writes to the GP write-gather pipe (every GX draw command flows through
// here). Distinguishes "game isn't issuing draws" (count stays ~0) from "draws don't reach the GPU".
bool          g_dbg_fifo = getenv("SUNBRIGHT_DBG_FIFO") != nullptr;
unsigned long g_fifo_writes = 0;
inline void fifo_count() {
    if (g_dbg_fifo && (++g_fifo_writes % 200000) == 1)
        std::fprintf(stderr, "[fifo] %lu gather-pipe writes\n", g_fifo_writes);
}

inline void gx_tap_cmd(u8 v) {       // a u8 written to the gather pipe
    if (v == 0x10) g_gx_state = 1;   // XF load command
    else if (g_gx_state != 2) g_gx_state = 0;
}
inline void gx_tap_word(u32 v) {     // a u32 written to the gather pipe
    if (g_gx_state == 1) {           // this u32 is the token
        if ((v >> 16) == 0xB && (v & 0xFFFF) < 0x100) {
            g_gx_state = 2; g_gx_idx = 0; g_gx_addr = v & 0xFFFF;
        } else {
            g_gx_state = 0;
        }
    } else if (g_gx_state == 2) {
        g_gx_words[g_gx_idx++] = v;
        if (g_gx_idx == 12) {
            g_gx_state = 0;
            ++g_gx_total;
            // Diagnostic only. Note (verified): the immediate XF matrices that
            // reach the FIFO are all J2D's constant 2D/UI matrix — real 3D model
            // transforms use indexed loads from the J3DMtxBuffer in RAM and never
            // pass through here. Capture for interpolation must be CPU-side.
            if ((g_gx_total % 5000) == 1) {
                f32 tx, ty, tz;
                std::memcpy(&tx, &g_gx_words[3], 4);
                std::memcpy(&ty, &g_gx_words[7], 4);
                std::memcpy(&tz, &g_gx_words[11], 4);
                std::fprintf(stderr, "[gxcap] immediate XF mtx total=%lu xf=%u pos=(%.2f, %.2f, %.2f)\n",
                             g_gx_total, g_gx_addr, tx, ty, tz);
            }
        }
    } else {
        g_gx_state = 0;
    }
}
}  // namespace

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
#  include "Core/PowerPC/PowerPC.h"
#  include "Core/Core.h"
#  include "Core/CoreTiming.h"
#  include "Core/System.h"
#  include "Core/PowerPC/JitInterface.h"
#  include "Core/HW/ProcessorInterface.h"
#  include "VideoCommon/CommandProcessor.h"
#  include "VideoCommon/Fifo.h"
#  include "Core/Core.h"

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

// One-shot diagnostic: name the FIRST gather-pipe writer of the run (guest pc/lr + host backtrace).
// Bursting the pipe before the game programs the CP FIFO registers overflows instantly
// ("FIFO is overflowed by GatherPipe", CP base/end still 0) — this names who jumped the gun.
extern void sb_log_first_gather();


// ── Recomp poll-loop progress (CoreTiming nudge / interrupt yield) ──────────
// A guest busy-wait that polls a status flag runs in recomp as a tight native C loop: it never
// reaches a recomp→JIT boundary, so Dolphin's CoreTiming never advances, no interrupt is delivered,
// and the polled flag never changes → infinite spin (99.5% CPU). Pure Dolphin advances CoreTiming
// every instruction. Fix: detect a poll (≥N repeated reads of the SAME address) and let the polled
// thing make progress:
//   • MMIO device register (0xCCxxxxxx): advance CoreTiming so the device updates the register;
//     interrupt deferred (clear MSR[EE]) — recomp can't redirect to a handler mid-function.
//     (e.g. JKRAram::create polling DSP/ARAM 0xCC005016 bit0.)
//   • RAM flag (e.g. the GC OS idle loop's RunQueueBits): advance CoreTiming AND deliver the pending
//     interrupt + run its ISR (sunbright_poll_yield), so the ISR (OSWakeupThread) can set the flag.
// Only fires on a confirmed tight poll, so one-off reads (incl. the DSP mailbox handshake) and
// normal sequential RAM access (different addresses) are untouched.
extern void sunbright_poll_yield();   // dolphin_hook.cpp
extern "C" void sb_trace(const char* tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
// Spin-loop detector state. The fast-path note (sb_poll_note) lives inline in intrinsics.h so a
// RAM read is just load+bswap; only a confirmed poll calls sb_poll_fire here. These globals are
// touched only on the EmuThread (the recomp + its yield), so plain globals (no thread_local).
bool g_in_poll_yield = false;   // re-entrancy guard (the ISR reads memory too)
u32  g_poll_last = 0;
u32  g_poll_reps = 0;
// One-shot CP/PI-FIFO programming log: the first 24 hardware writes that configure the GP FIFO.
// Diagnostic for "gather burst with CP unprogrammed" — shows whether/what GXSetGPFifo wrote.
void sb_log_fifo_reg_write(u32 ea, u32 v, int bits) {
    static int left = 48;
    // CPCtrl is written every frame; logging it unbounded was ~200k lines per run.
    // SUNBRIGHT_DBG_FIFOREG=1 restores the always-log-CPCtrl firehose.
    static const bool fifohose = getenv("SUNBRIGHT_DBG_FIFOREG") != nullptr;
    const u32 lo = ea & 0x0FFFFFFFu;
    const bool is_ctrl = fifohose && (lo == 0x0C000002u);
    if (left <= 0 && !is_ctrl) return;
    if ((lo >= 0x0C000000u && lo < 0x0C000080u) || (lo >= 0x0C003000u && lo < 0x0C003030u) ||
        (lo >= 0x0C001000u && lo < 0x0C001010u)) {
        if (!is_ctrl) left--;
        fprintf(stderr, "[fiforeg] w%d %08x <- %08x (recomp pc=%08x)\n", bits, ea, v,
                g_cur_recomp_cpu ? g_cur_recomp_cpu->pc : 0);
    }
}

void sb_log_first_gather() {
    static unsigned long bursts = 0;
    bursts++;
    // Telemetry gated: the periodic CP-register reads below go through MMIO and each one
    // triggers Dolphin's SyncGPUForRegisterAccess (a full GPU sync) — measurable drag on the
    // hot gather path. Permanent diagnostic, env-gated per keep-diagnostics.
    static const bool dbg_fifo = getenv("SUNBRIGHT_DBG_FIFO") != nullptr;
    if (!dbg_fifo) return;
    if ((bursts & 0xFF) == 0) {      // every 256 bursts: FIFO backing up? (GPU stalled)
        const u32 dist = ((u32)sb_r16(0xCC000032u) << 16) | sb_r16(0xCC000030u);
        if (dist > 0x7A000u) {
            static int tele = 48;
            if (tele > 0) {
                tele--;
                const u32 rp     = ((u32)sb_r16(0xCC00003Au) << 16) | sb_r16(0xCC000038u);
                const u32 wp     = ((u32)sb_r16(0xCC000036u) << 16) | sb_r16(0xCC000034u);
                const u16 status = sb_r16(0xCC000000u);
                const u16 ctrl   = sb_r16(0xCC000002u);
                auto& sysm = Core::System::GetInstance();
                const u32 pi_cause = sysm.GetProcessorInterface().GetCause();
                const u32 pi_mask  = sysm.GetProcessorInterface().GetMask();
                const u32 exc      = sysm.GetPPCState().Exceptions;
                const u32 msr      = sysm.GetPPCState().msr.Hex;
                fprintf(stderr, "[gather] STALL burst#%lu dist=%08x rp=%08x wp=%08x status=%04x ctrl=%04x "
                        "pi=%08x/%08x exc=%08x msr=%08x intmsk=%08x intr_waiting=%d cpu_thread=%d\n",
                        bursts, dist, rp, wp, status, ctrl, pi_cause, pi_mask, exc, msr,
                        sb_r32(0x800000C4u),
                        (int)sysm.GetCommandProcessor().IsInterruptWaiting(),
                        (int)Core::IsCPUThread());
            }
        }
    }
    static bool logged = false;
    if (logged) return;
    logged = true;
    fprintf(stderr, "[gather] FIRST gather-pipe write: recomp pc/lr=%08x/%08x r1=%08x\n",
            g_cur_recomp_cpu ? g_cur_recomp_cpu->pc : 0,
            g_cur_recomp_cpu ? g_cur_recomp_cpu->lr : 0,
            g_cur_recomp_cpu ? g_cur_recomp_cpu->gpr[1] : 0);
    void* bt[48];
    int n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, fileno(stderr));
    // Bursting the pipe with the CP FIFO unprogrammed (base==end==0) instantly overflows — on
    // real HW this scribbles via a wild WPAR too. Invariant: the first burst must find a
    // configured FIFO; park for REPL stack inspection if not.
    // CP regs are 16-bit; a 32-bit MMIO read returns 0 (no 32-bit mapping) — read halves.
    const u32 cp_base = ((u32)sb_r16(0xCC000022u) << 16) | sb_r16(0xCC000020u);
    const u32 cp_end  = ((u32)sb_r16(0xCC000026u) << 16) | sb_r16(0xCC000024u);
    fprintf(stderr, "[gather] CP base=%08x end=%08x\n", cp_base, cp_end);
    fflush(stderr);
    if (cp_base == 0 && cp_end == 0) {
        extern void sunbright_park(const char*);
        sunbright_park("gather-pipe burst with CP FIFO unprogrammed");
    }
}

void sb_poll_fire(u32 ea) {
    // Advancing CoreTiming requires owning the CPU: a confirmed poll on a thread that is not the
    // declared CPU thread (e.g. runtime bookkeeping reads during an nthr context switch, after the
    // yielder Undeclared) must not fire — a VI/device callback taken here can CPUThreadGuard-
    // deadlock against the real CPU loop (Core::OnFrameEnd → PauseAndLock, found 2026-06-09).
    if (!Core::IsCPUThread()) { g_poll_reps = 0; return; }
    g_in_poll_yield = true;
    if (ea >= 0xCC000000u) {                    // MMIO device register: advance only (defer IRQ)
        auto& sys = Core::System::GetInstance();
        auto& ppc = sys.GetPPCState();
        const u32 saved_msr = ppc.msr.Hex;
        ppc.msr.Hex &= ~0x8000u;
        sys.GetCoreTiming().Idle();
        sys.GetCoreTiming().Advance();
        ppc.msr.Hex = saved_msr;
    } else {                                    // RAM flag (idle loop): advance + deliver IRQ + ISR
        sunbright_poll_yield();
    }
    g_in_poll_yield = false;
    static const bool log = getenv("SUNBRIGHT_DBG_POLL") != nullptr;
    if (log) { static long n = 0; if ((n++ & 0x3FF) == 0)
        fprintf(stderr, "[poll] yield at %08x (x%ld)\n", ea, n); }
}

// Base of main RAM (low 24 MB), published for the inlined fast path in intrinsics.h. Set lazily on
// the first RAM access through ram_ptr below (memory is built before boot, so it's stable after).
u8* g_ram_base = nullptr;
// Base of the locked-L1 array (0xE0000000): published with g_ram_base on first RAM access.
// THP decode uses the locked cache as scratch; the inlined fast path needs this pointer.
u8* g_l1_base = nullptr;

// Fast path: main RAM only (cached 0x8xxxxxxx / uncached 0xCxxxxxxx mirrors of the
// low 24 MB). Returns nullptr for everything else — MMIO, locked cache, etc. —
// which must go through Dolphin's Read_U*/Write_U* so hardware register handlers
// (VI/PE/DSP/SI/EXI…) actually run. Reading those via a raw pointer returns
// garbage and breaks any code that polls a hardware status bit.
static inline u8* ram_ptr(u32 ea) {
    const u32 top = ea >> 28;
    if ((top == 0x8 || top == 0xC) && (ea & 0x0FFFFFFF) < 0x01800000) {
        if (!g_ram_base) { g_ram_base = MEM().GetRAM(); g_l1_base = MEM().GetL1Cache(); }
        return g_ram_base ? g_ram_base + (ea & 0x01FFFFFFu) : MEM().GetPointerForRange(ea, 1);
    }
    return nullptr;
}

#  define MMIO_R(bits, ea)     (g_recomp_touched_mmio = true, MMU_().Read<u##bits>(ea))
#  define MMIO_W(bits, ea, v)  (g_recomp_touched_mmio = true, MMU_().Write<u##bits>((v), (ea)))
#else
// Standalone mode: flat 24 MB RAM buffer, no MMIO.
u8* g_l1_base = nullptr;   // no locked-cache backing in standalone mode (fast path stays off)
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

// Shared diagnostic: dump the live guest register file and name whichever GPR, plus a
// small displacement, equals the faulting ea — i.e. the corrupted base pointer. Used by
// BOTH the wild-read and wild-write traps so a bad store names its base register the same
// way a bad load does (the asymmetry hid that the TBeamManager-ctor crash was `this`≈small).
// SUNBRIGHT_FATAL_HOLD=1: instead of aborting, print a marker and PARK forever so the process (and
// the SUNBRIGHT_PROBE REPL) stay alive — lets you inspect guest state at the fault via the REPL
// (/r, /poll, /stack) without racing a fast crash. Default still aborts (fail-fast). Always skips
// the multi-GB core dump first (see trap_wild_write).
[[noreturn]] static void fatal_hold_or_abort() {
    struct rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    if (getenv("SUNBRIGHT_FATAL_HOLD")) {
        extern void sunbright_park(const char*);   // watchdog.h — marks the park so the watchdog spares it
        sunbright_park("FATAL_HOLD at fault");
    }
    abort();
}

// Core dump: works off ANY guest register source (recomp CPUState or Dolphin PPCState),
// so the recomp wild-access traps and the Dolphin-side invalid-access trap share one format.
static void dump_guest_regs_core(const u32* gpr, u32 lr, u32 ctr, u32 ea) {
    fprintf(stderr, "  Guest registers at fault (lr=%08x ctr=%08x):\n", lr, ctr);
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "    r%-2d=%08x  r%-2d=%08x  r%-2d=%08x  r%-2d=%08x\n",
                i, gpr[i], i+1, gpr[i+1], i+2, gpr[i+2], i+3, gpr[i+3]);
    // Name the base register: whichever GPR, plus a small displacement, equals ea.
    for (int i = 0; i < 32; i++) {
        u32 d = ea - gpr[i];
        if (d <= 0xffffu)
            fprintf(stderr, "  ▶ base looks like r%d=%08x + 0x%x (the bad pointer is r%d)\n",
                    i, gpr[i], d, i);
    }
    // Current guest OSThread + guest call chain. The guest stack links its frames regardless of
    // where the code runs (recomp host C stack or Dolphin JIT/interp): back-chain at [r1], saved
    // LR at [r1+4]. Walk it to recover the guest caller chain — names who called into the crashing
    // code (e.g. the archive-mount / boot-sequencer path).
    fprintf(stderr, "  Current OSThread (0x800000E4) = %08x\n", mem_r32(0x800000E4u));
    fprintf(stderr, "  Guest call chain (saved LRs up the stack):\n");
    u32 fp = gpr[1];
    for (int i = 0; i < 16 && fp >= 0x80000000u && fp < 0x81800000u; i++) {
        u32 saved_lr = mem_r32(fp + 4);
        fprintf(stderr, "    [%d] lr=%08x\n", i, saved_lr);
        u32 next = mem_r32(fp);
        if (next <= fp || next < 0x80000000u || next >= 0x81800000u) break;
        fp = next;
    }
}
static void dump_guest_regs_naming_base(u32 ea) {
    if (!g_cur_recomp_cpu) return;
    const CPUState& c = *g_cur_recomp_cpu;
    dump_guest_regs_core(c.gpr, c.lr, c.ctr, ea);
}

// ── Wild-write trap ─────────────────────────────────────────────────────────
// A guest store whose effective address is neither main RAM (0x8xxxxxxx /
// 0xCxxxxxxx low 24 MB, handled by ram_ptr) nor the write-gather pipe nor a real
// hardware region — MMIO 0xCC.., EFB 0xC8.., locked cache 0xE0.., all >=
// 0xC0000000 — is a *wild write*: the symptom of a corrupted base/stack pointer
// (e.g. r1 ≈ a float bit-pattern). A correct port never does this, so it is always
// fatal — we do NOT let Dolphin's MMU swallow it with a one-line "Invalid write"
// warning and let the game limp on over garbage. Because recomp→recomp calls stay
// on the host C stack (single C-call model), the native backtrace names the exact
// recomp function chain that produced the bad address.
[[noreturn]] static void trap_wild_write(u32 ea, unsigned long long val, int bits) {
    fflush(stdout);
    fprintf(stderr,
        "\n[sunbright] FATAL wild guest write: ea=0x%08x val=0x%0*llx (%d-bit)\n"
        "  ea is outside main RAM / gather-pipe / MMIO — corrupted base or stack pointer.\n",
        ea, bits / 4, val, bits);
    dump_guest_regs_naming_base(ea);
    fprintf(stderr, "  Native backtrace (recomp call chain, innermost first):\n");
    void* bt[96];
    int n = backtrace(bt, 96);
    backtrace_symbols_fd(bt, n, fileno(stderr));
    fflush(stderr);
    // Suppress the core dump: the default core_pattern pipes to systemd-coredump, which
    // dumps this multi-GB process and wedges for minutes (looks like "prints FATAL but
    // never exits"). The backtrace above is what we want; skip the core so abort() exits
    // promptly (exit 134).
    fatal_hold_or_abort();
}
static inline void check_wild_write(u32 ea, unsigned long long val, int bits) {
    if (ea < 0xC0000000u) trap_wild_write(ea, val, bits);
}

// ── Wild-read trap ──────────────────────────────────────────────────────────
// The read counterpart of the wild-write trap above. A guest READ whose effective address is
// neither main RAM (handled by ram_ptr) nor a real hardware region (all >= 0xC0000000) — e.g. the
// reported "Invalid read from 0x01fe023e" — is a corrupted base/pointer (a pointer that lost its
// high bit, or a read through NULL/garbage). Dolphin's MMU otherwise swallows it with a one-line
// "Invalid read" warning and hands back garbage, so the game limps on over poison until it dies
// somewhere downstream. That downstream death hides the real cause, so we make the ORIGINATING read
// fatal here — the native backtrace names the exact recomp function that produced the bad address.
static void report_wild_read(u32 ea, int bits) {
    fflush(stdout);
    fprintf(stderr,
        "\n[sunbright] FATAL wild guest read: ea=0x%08x (%d-bit)\n"
        "  ea is outside main RAM (0x80000000-0x817FFFFF) and all hardware regions (>=0xC0000000)\n"
        "  — a corrupted base/pointer (lost high bit, or read through NULL/garbage). This is the\n"
        "  ORIGINATOR; Dolphin's MMU would warn 'Invalid read' and return garbage, and the game\n"
        "  would crash later somewhere else, hiding this root cause.\n",
        ea, bits);
    if (g_cur_recomp_cpu)
        fprintf(stderr, "  current recomp function pc=%08x (coarse — the function being executed)\n",
                g_cur_recomp_cpu->pc);
    dump_guest_regs_naming_base(ea);
    fprintf(stderr, "  Native backtrace (recomp call chain, innermost first):\n");
    void* bt[96];
    int n = backtrace(bt, 96);
    backtrace_symbols_fd(bt, n, fileno(stderr));
    fflush(stderr);
}
static inline void check_wild_read(u32 ea, int bits) {
    if (ea >= 0xC0000000u) return;
    // Diagnostic-only escape hatch: when set, report the first few wild reads but let
    // execution continue (the read falls through to Dolphin's MMU = garbage, as upstream).
    // This lets the SUNBRIGHT_DIFF harness run THROUGH the corruption to the function whose
    // RAM output first diverges (the real corruptor). The DEFAULT still aborts (fail-fast).
    static const bool nonfatal = getenv("SUNBRIGHT_WILD_READ_NONFATAL") != nullptr;
    if (nonfatal) {
        static int once = 0;
        if (once++ < 8) report_wild_read(ea, bits);
        return;
    }
    report_wild_read(ea, bits);
    fatal_hold_or_abort();
}

// ── Dolphin-side invalid-access trap ─────────────────────────────────────────
// The recomp wild-read/write traps above only see accesses our memory bridge routes — i.e. code
// our recompiler emitted. Code still running under Dolphin's interpreter/JIT (e.g. JIT-only OS
// primitives, or the boot sequencer before it hands to recomp) goes through Dolphin's own MMU,
// which on an unmapped guest address PanicAlerts "Invalid read/write" and then RETURNS GARBAGE and
// lets the game limp on — hiding the corrupting originator exactly like the swallowed warnings the
// traps above were written to replace. main_sdl.cpp's MsgAlertHandler routes those two panics here
// so a Dolphin-side wild access is just as fatal, with the same guest-state + backtrace dump. ea/pc
// are parsed from the panic text by the handler (Dolphin doesn't set DAR/DSISR outside MMU mode).
[[noreturn]] void sunbright_trap_invalid_access(u32 ea, u32 pc, bool write) {
    fflush(stdout);
    fprintf(stderr,
        "\n[sunbright] FATAL invalid guest %s under Dolphin: ea=0x%08x PC=0x%08x\n"
        "  Dolphin's MMU flagged an unmapped guest access — a corrupted base/pointer (NULL or a\n"
        "  pointer that lost its high bit). Aborting at the originator instead of returning garbage\n"
        "  and crashing later somewhere else, which would hide this root cause.\n",
        write ? "write" : "read", ea, pc);
    // If the fault came through the recomp's slow path, g_cur_recomp_cpu (thread_local, same thread)
    // holds the LIVE recomp registers — the real r31 etc. The Dolphin ppc.gpr below is STALE for
    // recomp faults (recomp keeps state in CPUState, not ppc). Dump the live recomp regs first.
    if (g_cur_recomp_cpu) {
        fprintf(stderr, "  LIVE recomp registers (g_cur_recomp_cpu — authoritative for recomp faults):\n");
        dump_guest_regs_core(g_cur_recomp_cpu->gpr, g_cur_recomp_cpu->lr, g_cur_recomp_cpu->ctr, ea);
    }
#ifdef HAVE_DOLPHIN_MEMMAP
    auto& ppc = Core::System::GetInstance().GetPPCState();
    fprintf(stderr, "  (Dolphin ppc registers — may be stale for recomp faults:)\n");
    dump_guest_regs_core(ppc.gpr, LR(ppc), CTR(ppc), ea);
#endif
    fprintf(stderr, "  Native backtrace (host call chain, innermost first):\n");
    void* bt[96];
    int n = backtrace(bt, 96);
    backtrace_symbols_fd(bt, n, fileno(stderr));
    fflush(stderr);
    fatal_hold_or_abort();
}

// ── Byte-swapped reads/writes (GC = big-endian, host = little-endian) ───────
// These are the OUT-OF-LINE slow paths: MMIO, the gather pipe, the wild-write trap, and the
// pre-memory-init RAM window (ram_ptr lazily publishes g_ram_base on first use). The hot RAM
// case never reaches here — it is inlined at the call site (sb_r*/sb_w* in intrinsics.h). The
// reads still note spin-loops (sb_poll_note) so an MMIO status-bit poll is detected; r64 matches
// the original (no poll note). The thin public mem_r*/mem_w* wrappers at the bottom dispatch
// through the same inline fast path for callers that don't use the MEM_* macros.

u8 mem_r8_slow(u32 ea) {
    sb_poll_note(ea);
    if (u8* p = ram_ptr(ea)) return *p;
    check_wild_read(ea, 8);
    return MMIO_R(8, ea);
}

u16 mem_r16_slow(u32 ea) {
    sb_poll_note(ea);
    if (u8* p = ram_ptr(ea)) return ((u16)p[0] << 8) | p[1];
    check_wild_read(ea, 16);
    const u16 v = MMIO_R(16, ea);
    // DSP→CPU mailbox reads (the JAS syncDSP mail consume): a duplicated/mistimed consume is the
    // suspected dead-audio trigger (extra 0xFF00 frame-done → audioproc intcount==0 exit).
    {
        const u32 lo = ea & 0x0FFFFFFFu;
        if (lo == 0x0C005004u || lo == 0x0C005006u)
            sb_trace("dspmr", ea, v, 0, g_cur_recomp_cpu ? g_cur_recomp_cpu->pc : 0);
    }
    return v;
}

u32 mem_r32_slow(u32 ea) {
    sb_poll_note(ea);
    if (u8* p = ram_ptr(ea)) return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
    check_wild_read(ea, 32);
    const u32 v = MMIO_R(32, ea);
    // DSP→CPU mailbox consume (DSPReadMailFromDSP does one 32-bit read of both halves) — the
    // JAS syncDSP mail sequence; see mem_r16_slow's twin trace for halfword readers.
    if ((ea & 0x0FFFFFFFu) == 0x0C005004u)
        sb_trace("dspmr", ea, v, 0, g_cur_recomp_cpu ? g_cur_recomp_cpu->pc : 0);
    return v;
}

u64 mem_r64_slow(u32 ea) {
    if (u8* p = ram_ptr(ea))
        return ((u64)p[0]<<56)|((u64)p[1]<<48)|((u64)p[2]<<40)|((u64)p[3]<<32)
             | ((u64)p[4]<<24)|((u64)p[5]<<16)|((u64)p[6]<<8)|p[7];
    check_wild_read(ea, 64);
    return MMIO_R(64, ea);
}

void mem_w8_slow(u32 ea, u8 v) {
    if (u8* p = ram_ptr(ea)) { *p = v; return; }
#ifdef HAVE_DOLPHIN_MEMMAP
    if (is_gather_pipe(ea)) {
        if (g_gxcap) gx_tap_cmd(v);
        sb_log_first_gather(); fifo_count(); g_recomp_touched_mmio = true;
        if (gxs_active()) gxs_w8(v); else GPF().Write8(v);
        return;
    }
#endif
    check_wild_write(ea, v, 8);
    MMIO_W(8, ea, v);
}

// EXI/DSP-mailbox register write tracing (probe /tracelog). Permanent diagnostic
// (keep-diagnostics): the CARDMount lost-completion deadlock and the dead-audio bug were both
// pinned by reconstructing these sparse op/ack sequences. Defined here so both the 16-bit and
extern "C" void sb_trace(const char* tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
static inline void trace_exi_write(u32 ea, u32 v, int bits);

void mem_w16_slow(u32 ea, u16 v) {
    if (u8* p = ram_ptr(ea)) { p[0] = v >> 8; p[1] = v & 0xFF; return; }
#ifdef HAVE_DOLPHIN_MEMMAP
    if (is_gather_pipe(ea)) {
        fifo_count(); g_recomp_touched_mmio = true;
        if (gxs_active()) gxs_w16(v); else GPF().Write16(v);
        return;
    }
#endif
    check_wild_write(ea, v, 16);
    sb_log_fifo_reg_write(ea, v, 16);
    trace_exi_write(ea, v, 16);
    // DSP CSR (0xCC00500A) write-1-to-clear protection: a guest read-modify-write (the JASystem
    // CPU→DSP mailbox kick, OS mask updates) must not clear a PENDING status bit whose interrupt
    // we haven't dispatched yet — on hardware an EE-on context can't observe a pending DSPINT
    // (it vectors immediately). Only the currently-dispatching DSP handler may ack its own bit.
    // This was THE dead-audio bug: one swallowed DSP-done froze the whole frame cycle.
    if ((ea & 0x0FFFFFFEu) == 0x0C00500Au) {
        extern u32 sunbright_dsp_ack_allowed();
        const u16 old = MMIO_R(16, ea);
        const u16 pending = old & 0x00A8u;             // AIDINT|ARINT|DSPINT currently set
        const u16 protect = pending & (u16)~sunbright_dsp_ack_allowed();
        if (v & protect) {
            sb_trace("dspprot", ea, v, protect, old);  // would-have-swallowed (kept pending)
            v &= (u16)~protect;
        }
    }
    MMIO_W(16, ea, v);
#ifdef HAVE_DOLPHIN_MEMMAP
    // CPU→DSP mailbox-low write completed: if the HLE raised a DSP interrupt for its reply mails
    // (captured in aid_native.cpp, never CoreTiming-scheduled), deliver it NOW — the post-store
    // instruction boundary is where hardware takes it. Keeps the JAS mail round-trip synchronous.
    if ((ea & 0x0FFFFFFEu) == 0x0C005002u) {
        extern void sunbright_dsp_flush_sync();
        sunbright_dsp_flush_sync();
    }
    // CP breakpoint-register writes must wake the GPU loop: real CP hardware re-evaluates the
    // breakpoint continuously, but Dolphin's RunGpuLoop only wakes on gather bursts and CTRL
    // writes. When the GX pusher is SUSPENDED at the hi watermark (FIFO pacing) and the
    // draw-sync thread moves the breakpoint via FIFO_BP_LO/HI alone, nothing wakes the GPU —
    // it stays parked at the OLD breakpoint forever and the whole pipeline deadlocks
    // (2026-06-10). Faithful hardware semantics: a BP change lets the GPU proceed NOW.
    {
        const u32 lo = ea & 0x0FFFFFFFu;
        if (lo == 0x0C00003Cu || lo == 0x0C00003Eu)
            Core::System::GetInstance().GetFifo().RunGpu();
    }
#endif
}

static inline void trace_exi_write(u32 ea, u32 v, int bits) {
    const u32 lo = ea & 0x0FFFFFFFu;
    if (lo >= 0x0C006800u && lo < 0x0C006840u)
        sb_trace("exiw", ea, v, (u32)bits, g_cur_recomp_cpu ? g_cur_recomp_cpu->pc : 0);
    // DSP mailbox + CSR writes: the JASystem per-frame command cycle and the interrupt acks.
    // A CSR (0xCC00500A) read-modify-write ack that carries a sibling status bit swallows that
    // sibling's interrupt — suspected mechanism of the dead-audio DSP-done loss (2026-06-10).
    if (lo >= 0x0C005000u && lo < 0x0C005010u)
        sb_trace("dspmb", ea, v, (u32)bits, g_cur_recomp_cpu ? g_cur_recomp_cpu->pc : 0);
}

void mem_w32_slow(u32 ea, u32 v) {
    if (u8* p = ram_ptr(ea)) {
        p[0]=v>>24; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF; return;
    }
#ifdef HAVE_DOLPHIN_MEMMAP
    if (is_gather_pipe(ea)) {
        if (g_gxcap) gx_tap_word(v);
        sb_log_first_gather(); fifo_count(); g_recomp_touched_mmio = true;
        if (gxs_active()) gxs_w32(v); else GPF().Write32(v);
        return;
    }
#endif
    check_wild_write(ea, v, 32);
    sb_log_fifo_reg_write(ea, v, 32);
    trace_exi_write(ea, v, 32);
    MMIO_W(32, ea, v);
}

void mem_w64_slow(u32 ea, u64 v) {
    if (u8* p = ram_ptr(ea)) {
        p[0]=v>>56; p[1]=(v>>48)&0xFF; p[2]=(v>>40)&0xFF; p[3]=(v>>32)&0xFF;
        p[4]=(v>>24)&0xFF; p[5]=(v>>16)&0xFF; p[6]=(v>>8)&0xFF; p[7]=v&0xFF; return;
    }
#ifdef HAVE_DOLPHIN_MEMMAP
    if (is_gather_pipe(ea)) {
        g_recomp_touched_mmio = true;
        if (gxs_active()) gxs_w64(v); else GPF().Write64(v);
        return;
    }
    check_wild_write(ea, v, 64);
    // No Write_U64 in Dolphin's MMIO API — split into two 32-bit writes.
    MMIO_W(32, ea, (u32)(v >> 32));
    MMIO_W(32, ea + 4, (u32)v);
#else
    MMIO_W(32, ea, (u32)(v >> 32));
    MMIO_W(32, ea + 4, (u32)v);
#endif
}

// dcbz (Data Cache Block set to Zero): NOT a no-op — it zeroes the cache block (32 bytes on
// Gekko/Broadway) containing EA, without reading memory first. SMS's THP video decoder relies on
// this to clear the DCT coefficient buffer (and to establish locked-cache output lines) before the
// VLC decoder fills only the non-zero coefficients; NOP'ing it left stale coefficients from the
// previous block → a per-MCU "comb" in the FMV. Zero is endian-agnostic, and the memory bridge
// routes the address correctly (main RAM fast path, or the locked cache / MMIO slow path).
void dcbz32(u32 ea) {
    ea &= ~0x1Fu;                          // align to the 32-byte cache block
    for (int i = 0; i < 8; i++) sb_w32(ea + (u32)(i * 4), 0);
}

// icbi: invalidate the 32-byte instruction-cache block containing EA, in DOLPHIN's caches.
// Recomp execution never fetches through Dolphin's icache, but the interpreter (hybrid paths)
// does — so game code that loads/copies code under recomp and ICInvalidateRange's it must reach
// Dolphin's InstructionCache + JIT block cache, or the interpreter later executes the stale
// pre-copy line (the phantom-`bl` JUT-crash, 2026-06-10). Faithful port of the icbi side effect.
void icbi32(u32 ea) {
#ifdef HAVE_DOLPHIN_MEMMAP
    auto& sys = Core::System::GetInstance();
    sys.GetPPCState().iCache.Invalidate(sys.GetMemory(), sys.GetJitInterface(), ea & ~0x1Fu);
#else
    (void)ea;
#endif
}

// Public out-of-line wrappers (declared in intrinsics.h) — dispatch through the inline fast path.
u8   mem_r8 (u32 ea)         { return sb_r8(ea);  }
u16  mem_r16(u32 ea)         { return sb_r16(ea); }
u32  mem_r32(u32 ea)         { return sb_r32(ea); }
u64  mem_r64(u32 ea)         { return sb_r64(ea); }
f32  mem_rf32(u32 ea)        { return sb_rf32(ea); }
f64  mem_rf64(u32 ea)        { return sb_rf64(ea); }
void mem_w8 (u32 ea, u8  v)  { sb_w8(ea, v);  }
void mem_w16(u32 ea, u16 v)  { sb_w16(ea, v); }
void mem_w32(u32 ea, u32 v)  { sb_w32(ea, v); }
void mem_w64(u32 ea, u64 v)  { sb_w64(ea, v); }
void mem_wf32(u32 ea, f32 v) { sb_wf32(ea, v); }
void mem_wf64(u32 ea, f64 v) { sb_wf64(ea, v); }

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

// psq_l / psq_st with the CORRECT element width and stride per GQR type. The old emitter always
// did MEM_R32(ea)/MEM_R32(ea+4) and cast the (byteswapped) 32-bit word to (u8)/(u16) — only valid
// for float (4-byte) elements. A quantized type (u8/s8 = 1 byte at +0/+1; u16/s16 = 2 bytes at
// +0/+2) needs a narrow, correctly-strided load, and the narrow load must read the element AT `ea`
// (not the low byte of a 32-bit word, which big-endian byteswap puts at the highest address).
// This is what corrupted the THP paired-single IDCT (s16, type 7) → garbage FMV.
static inline bool  psq_is_float(u32 t) { return t != 4 && t != 5 && t != 6 && t != 7; }
static inline u32   psq_stride(u32 t)   { return (t == 4 || t == 6) ? 1u : (t == 5 || t == 7) ? 2u : 4u; }
// The GQR scale is a 6-bit SIGNED value (0–31 positive, 32–63 = −32..−1). Load multiplies by
// 2^(-scale), store by 2^(+scale) — matching Dolphin's m_dequantizeTable / m_quantizeTable.
// (The old code did `1u << scale` — unsigned, and undefined behavior for scale>=32, e.g. the u8
// YUV store here uses scale=61 = −3 → multiplier 2^3 on load / 2^-3 on store.)
static inline float psq_ld_mult(u32 scale6) { int s=(int)(scale6&0x3F); if(s>=32) s-=64; return std::ldexp(1.0f,-s); }
static inline float psq_st_mult(u32 scale6) { int s=(int)(scale6&0x3F); if(s>=32) s-=64; return std::ldexp(1.0f, s); }

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
    if (psq_is_float(t)) {                       // float: 4-byte elements, scale ignored
        f32 f0 = (f32)v0; u32 r0; std::memcpy(&r0, &f0, 4); sb_w32(ea, r0);
        if (!w) { f32 f1 = (f32)v1; u32 r1; std::memcpy(&r1, &f1, 4); sb_w32(ea + 4, r1); }
        return;
    }
    const float s = psq_st_mult(gqr_st_scale(gqr));
    auto one = [&](u32 a, f64 v) {
        switch (t) {
        case 4:  sb_w8 (a, (u8) std::max(0.0, std::min(255.0,   (f64)((f32)v * s)))); break;
        case 5:  sb_w16(a, (u16)std::max(0.0, std::min(65535.0, (f64)((f32)v * s)))); break;
        case 6:  sb_w8 (a, (u8)(s8) std::max(-128.0,   std::min(127.0,   (f64)((f32)v * s)))); break;
        default: sb_w16(a, (u16)(s16)std::max(-32768.0, std::min(32767.0, (f64)((f32)v * s)))); break;  // 7 = s16
        }
    };
    one(ea, v0);
    if (!w) one(ea + psq_stride(t), v1);
}

// ---- SUNBRIGHT_WATCH_WADDR write-watch (see intrinsics.h sb_w*) -------------------------
#include <dlfcn.h>
u32 g_watch_wa = [](){
    const char* e = getenv("SUNBRIGHT_WATCH_WADDR");
    return e ? (u32)strtoul(e, nullptr, 16) : 0u;
}();
// When armed by /interp60?watch=1, fire only during the 60 fps in-between redraw
// (so the real-field recompute doesn't drown the log) and cap the volume.
bool g_watch_redraw_only = false;
extern bool g_interp60_in_redraw;
void sb_watch_fire(u32 ea, u32 v, int width, void* host_ra) {
    if (g_watch_redraw_only && !g_interp60_in_redraw) return;
    static int fired = 0;
    if (fired >= 48) return;             // cap log volume (covers both modes)
    fired++;
    Dl_info di{};
    const char* fn = (dladdr(host_ra, &di) && di.dli_sname) ? di.dli_sname : "?";
    // host_ra raw: addr2line -e build/sunbright <ra> -> generated/functions_*.cpp:line
    // (the recomp funcs are internal symbols dladdr can't see; addr2line uses symtab).
    fprintf(stderr, "[watchw] %sea=%08x v=%0*x w%d from %s  ra=%p\n",
            g_interp60_in_redraw ? "REDRAW " : "", ea, width * 2, v, width, fn, host_ra);
}

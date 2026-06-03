#include "native_os.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

extern u32 mem_r32(u32 ea);   // from memory_bridge

// The reschedule-free recompiled OSCreateThread, super-called for faithful guest-struct init
// (see docs/native_threading.md: it never calls SelectThread/__OSReschedule, so it returns).
extern "C" void func_80348948(CPUState& cpu);

namespace {
std::unordered_map<u32, NativeOSFn> g_native_os;
u32 g_os_lo = 0xFFFFFFFFu;     // [lo, hi) fast-reject range — native_os_lookup is consulted
u32 g_os_hi = 0;              // on every call_ppc and every interpreter step, so reject early

// Native record of every guest OS thread created (step 4a). The matching nthr host thread is
// spawned from this in step 4b (together with OSResumeThread + the blocking primitives, where
// a thread can actually be scheduled and the path is end-to-end testable).
struct GuestThreadRec {
    u32 os_thread;   // guest OSThread* (also the OSCreateThread `thread` arg)
    u32 entry;       // thread function
    u32 param;       // its argument
    u32 stack;       // stack top
    u32 stack_size;
    int priority;
};
std::vector<GuestThreadRec> g_guest_threads;
}  // namespace

void native_os_register(u32 addr, NativeOSFn fn) {
    g_native_os[addr] = fn;
    g_os_lo = std::min(g_os_lo, addr);
    g_os_hi = std::max(g_os_hi, addr + 4);
}

NativeOSFn native_os_lookup(u32 addr) {
    if (addr < g_os_lo || addr >= g_os_hi) return nullptr;   // cheap reject (hot path)
    auto it = g_native_os.find(addr);
    return it == g_native_os.end() ? nullptr : it->second;
}

// ── Native OS primitives (GMSE01) ────────────────────────────────────────────
// OSGetCurrentThread (0x80348368): r3 = *(0x800000E4). Behaviour-identical to the
// recompiled/interpreted version (`lwz r3, 0xE4(0); blr`), so boot is unchanged. This is the
// first genuinely-native primitive — its job is to prove the native-OS set is consulted on
// BOTH the recomp call path and the run_jit_sync interpreter loop, before the load-bearing
// scheduling primitives (OSCreateThread/Resume/Sleep/Wakeup/message queues) are added.
static void os_get_current_thread(CPUState& cpu) {
    cpu.gpr[3] = mem_r32(0x800000E4u);
}

// OSCreateThread (0x80348948): BOOL OSCreateThread(OSThread* r3, func r4, param r5, stack r6,
// stackSize r7, priority r8, attr r9). Reschedule-free, so we super-call the recomp body to
// init the guest struct faithfully (state/priority/links/context/canary + active-thread list),
// then record the thread natively. The host thread itself is spawned in step 4b.
static void os_create_thread(CPUState& cpu) {
    const u32 os_thread = cpu.gpr[3];
    const u32 entry     = cpu.gpr[4];
    const u32 param     = cpu.gpr[5];
    const u32 stack     = cpu.gpr[6];
    const u32 stack_sz  = cpu.gpr[7];
    const int priority  = (int)(s32)cpu.gpr[8];

    func_80348948(cpu);                 // faithful guest-struct init (returns normally)
    if (cpu.gpr[3] == 0) return;        // creation failed (bad priority) — nothing to record

    g_guest_threads.push_back({os_thread, entry, param, stack, stack_sz, priority});
    fprintf(stderr,
        "[native_os] OSCreateThread #%zu  OSThread=%08x entry=%08x param=%08x stack=%08x "
        "size=%u prio=%d\n",
        g_guest_threads.size(), os_thread, entry, param, stack, stack_sz, priority);
}

void native_os_init() {
    static bool done = false;
    if (done) return;
    done = true;
    native_os_register(0x80348368u, os_get_current_thread);
    native_os_register(0x80348948u, os_create_thread);
    fprintf(stderr, "[native_os] registered %zu native OS primitive(s) over [%08x,%08x)\n",
            g_native_os.size(), g_os_lo, g_os_hi);
}

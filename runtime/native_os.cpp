#include "native_os.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

extern u32 mem_r32(u32 ea);   // from memory_bridge

namespace {
std::unordered_map<u32, NativeOSFn> g_native_os;
u32 g_os_lo = 0xFFFFFFFFu;     // [lo, hi) fast-reject range — native_os_lookup is consulted
u32 g_os_hi = 0;              // on every call_ppc and every interpreter step, so reject early
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

void native_os_init() {
    static bool done = false;
    if (done) return;
    done = true;
    native_os_register(0x80348368u, os_get_current_thread);
    fprintf(stderr, "[native_os] registered %zu native OS primitive(s) over [%08x,%08x)\n",
            g_native_os.size(), g_os_lo, g_os_hi);
}

// Native OS threading (SMS GMSE01) — see docs/native_threading.md.
//
// The GameCube OS multiplexes software threads onto one CPU via a PPC software
// scheduler (SelectThread / __OSReschedule + context save/load). Running that
// scheduler synchronously under run_jit_sync (single-step to LR) deadlocks on a
// blocking reschedule (it hits the OS idle loop, which never returns to LR). The
// proper fix — this is a PC port — is to replace the OS thread/sync primitives with
// NATIVE host-thread implementations and never execute the PPC scheduler.
//
// This module is built incrementally, one primitive at a time. It begins life as a
// TRANSPARENT TRACE: each override logs its arguments + caller, then super-calls the
// original recompiled body (func_<addr>), so behaviour is unchanged. The trace maps
// SMS's actual thread topology (what threads exist, when they start/stop, who waits
// on what) under Dolphin — the model we then rebuild natively. Set SUNBRIGHT_OSTRACE=1
// to enable the logging; the super-call always runs.
//
// As each primitive is reimplemented natively, its override stops super-calling and
// does the real work on host threads — but stays in THIS file, the single source of
// truth for OS threading (no env-gated fallback; see done-right-over-working).

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>

// Recompiled bodies we super-call while these remain transparent pass-throughs.
extern "C" void func_80348948(CPUState&);  // OSCreateThread
extern "C" void func_80348ee8(CPUState&);  // OSResumeThread
extern "C" void func_80349170(CPUState&);  // OSSuspendThread
extern "C" void func_80348a68(CPUState&);  // OSExitThread
extern "C" void func_80348d08(CPUState&);  // OSJoinThread
extern "C" void func_80348b4c(CPUState&);  // OSCancelThread

namespace {

bool ostrace() {
    static const bool on = getenv("SUNBRIGHT_OSTRACE") != nullptr;
    return on;
}

// 0x80348948 OSCreateThread(OSThread* t, void* func, void* param, void* stack,
//                           u32 stackSize, OSPriority prio, u16 attr)
SUNBRIGHT_OVERRIDE(ov_OSCreateThread, 0x80348948u) {
    if (ostrace())
        fprintf(stderr, "[osthread] OSCreateThread  thread=%08x entry=%08x param=%08x "
                        "stack=%08x size=%u prio=%d  (caller=%08x)\n",
                cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[7],
                (int)cpu.gpr[8], cpu.lr);
    func_80348948(cpu);
}

// 0x80348ee8 OSResumeThread(OSThread* t) -> suspend count
SUNBRIGHT_OVERRIDE(ov_OSResumeThread, 0x80348ee8u) {
    if (ostrace())
        fprintf(stderr, "[osthread] OSResumeThread  thread=%08x  (caller=%08x)\n",
                cpu.gpr[3], cpu.lr);
    func_80348ee8(cpu);
}

// 0x80349170 OSSuspendThread(OSThread* t) -> suspend count
SUNBRIGHT_OVERRIDE(ov_OSSuspendThread, 0x80349170u) {
    if (ostrace())
        fprintf(stderr, "[osthread] OSSuspendThread thread=%08x  (caller=%08x)\n",
                cpu.gpr[3], cpu.lr);
    func_80349170(cpu);
}

// 0x80348a68 OSExitThread(void* val)
SUNBRIGHT_OVERRIDE(ov_OSExitThread, 0x80348a68u) {
    if (ostrace())
        fprintf(stderr, "[osthread] OSExitThread    val=%08x  (caller=%08x)\n",
                cpu.gpr[3], cpu.lr);
    func_80348a68(cpu);
}

// 0x80348d08 OSJoinThread(OSThread* t, void** val) -> BOOL
SUNBRIGHT_OVERRIDE(ov_OSJoinThread, 0x80348d08u) {
    if (ostrace())
        fprintf(stderr, "[osthread] OSJoinThread    thread=%08x  (caller=%08x)\n",
                cpu.gpr[3], cpu.lr);
    func_80348d08(cpu);
}

// 0x80348b4c OSCancelThread(OSThread* t)
SUNBRIGHT_OVERRIDE(ov_OSCancelThread, 0x80348b4cu) {
    if (ostrace())
        fprintf(stderr, "[osthread] OSCancelThread  thread=%08x  (caller=%08x)\n",
                cpu.gpr[3], cpu.lr);
    func_80348b4c(cpu);
}

}  // namespace

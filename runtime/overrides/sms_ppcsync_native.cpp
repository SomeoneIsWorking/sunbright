// Native PPCSync — removes the syscall-exception round-trip from GXFlush (boot choke point).
//
// 0x80341AB8 is the SDK's PPCSync(): a bare `sc; blr`. The GC OS syscall vector (0xC00) runs
// `sync; rfi` — its only job is to order the CPU's write-gather stores before continuing. GXFlush
// calls it every frame right after padding the gather pipe with 8 zero words.
//
// Under the recomp, the `sc` is serviced by interpreting from the syscall vector until control
// returns to the caller's LR. With device interrupts now pending every frame (the deterministic
// frame heartbeat advances CoreTiming), that interp run gets diverted into ISR after ISR before
// it can reach the LR — the watchdog freeze at pc=lr=0x8035D93C (GXFlush+0x4C) with interp_steps
// spinning and no fields presented.
//
// On this runtime the barrier is meaningless: gather-pipe stores go through GPFifo::Write
// synchronously, so by the time PPCSync is called every byte has already reached the FIFO. The
// faithful PC-native port is a no-op return. Registered both as a recomp override and on the
// native_os seam (interpreter-run threads consult only the latter).

#include "../overrides.h"
#include "../intrinsics.h"

#ifdef HAVE_DOLPHIN_CORE
#include "../native_os.h"

namespace {

constexpr u32 PPCSYNC = 0x80341AB8u;

void native_ppcsync(CPUState&) {}   // stores already ordered — nothing to do

SUNBRIGHT_OVERRIDE(ov_PPCSync, PPCSYNC) { (void)cpu; }

}  // namespace

void native_ppcsync_register() { native_os_register(PPCSYNC, native_ppcsync); }
#else
void native_ppcsync_register() {}
#endif  // HAVE_DOLPHIN_CORE

// TEMP diagnostic (SUNBRIGHT_DBG_DVD): trace the DVD read pipeline to find why only
// the first transfer (nintendo.szs) is issued under native scheduling and the FSM stalls.
//   - DVDReadAbsAsyncPrio (8034da6c): a file/archive read REQUEST is queued.
//   - DVDLowRead          (8034acd8): a hardware transfer is actually KICKED (writes DICR).
//   - the DI interrupt handler dispatch advances the FSM; if DVDLowRead count << request
//     count, the queue is stalling after the first command.
// All OBSERVE (super-call the real body); behaviour is unchanged.
#include "../overrides.h"
#include <cstdio>
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE
extern "C" void func_8034acd8(CPUState& cpu);   // DVDLowRead
extern u32 mem_r32(u32 ea);

static bool dbg() { static const bool e = getenv("SUNBRIGHT_DBG_DVD") != nullptr; return e; }

// NOTE: DVDReadAbsAsyncPrio (8034da6c) is now owned by the native DVD read service
// (native_dvd.cpp, on the native_os seam). This file keeps only the DVDLowRead probe — a
// non-zero count here means something still kicks the GC DVD hardware FSM (it should not).
// void DVDLowRead(void* addr, u32 length, u32 offset, DVDLowCallback callback)
SUNBRIGHT_OVERRIDE(dbg_dvd_lowread, 0x8034acd8u) {
    if (dbg()) {
        static unsigned long n = 0;
        fprintf(stderr, "[dvd] #%lu DVDLowRead     addr=%08x len=%d off=0x%x cb=%08x cur=%08x\n",
                n++, cpu.gpr[3], (int)cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], mem_r32(0x800000E4));
    }
    func_8034acd8(cpu);
}
#endif

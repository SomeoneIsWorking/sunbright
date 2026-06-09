// TEMP diagnostic (SUNBRIGHT_DBG_AUDIO): the FX-line array base (global [r13-0x5c04] = 8040e5bc)
// is NULL when JAIData::initData calls DSPInterface::getFXHandle/setFXLine, so the FX array was
// never allocated. JASystem::Driver::init (803140cc) -> DSPInterface::initBuffer (80314f50)
// allocates it. Question: ordering / which thread. Log Driver::init, initBuffer (alloc result),
// initData (the FX-base value it sees), and getFXHandle null returns — each with the current OSThread.
// All OBSERVE (super-call the real body). Recomp-path only; if a function runs on a JIT-only thread
// (interpreter) it won't log here — that absence is itself evidence of a cross-thread run.
#include "../overrides.h"
#include <cstdio>
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE
extern "C" void func_803140cc(CPUState& cpu);   // JASystem::Driver::init
extern "C" void func_80314f50(CPUState& cpu);   // DSPInterface::initBuffer
extern "C" void func_80303fac(CPUState& cpu);   // JAIData::initData
extern "C" void func_8031505c(CPUState& cpu);   // DSPInterface::getFXHandle
extern "C" void func_80014d1c(CPUState& cpu);   // MSound::startSoundSet
extern "C" void func_80300ab8(CPUState& cpu);   // JAIBasic::initInterfaceMain
extern "C" void func_80301a28(CPUState& cpu);   // JAIBasic::initDriver
extern u32 mem_r32(u32 ea);

static bool on() { static const bool e = getenv("SUNBRIGHT_DBG_AUDIO") != nullptr; return e; }
static u32  cur() { return mem_r32(0x800000E4); }
static u32  fxbase(u32 r13) { return mem_r32(r13 - 0x5c04); }

SUNBRIGHT_OVERRIDE(dbg_driver_init, 0x803140ccu) {
    if (on()) fprintf(stderr, "[audio] Driver::init        cur=%08x fxbase(before)=%08x heap=%08x\n",
                      cur(), fxbase(cpu.gpr[13]), mem_r32(cpu.gpr[13] - 0x5b30));
    func_803140cc(cpu);
    if (on()) fprintf(stderr, "[audio] Driver::init DONE    cur=%08x fxbase(after)=%08x\n",
                      cur(), fxbase(cpu.gpr[13]));
}

SUNBRIGHT_OVERRIDE(dbg_init_buffer, 0x80314f50u) {
    if (on()) fprintf(stderr, "[audio] initBuffer           cur=%08x heap=%08x\n",
                      cur(), mem_r32(cpu.gpr[13] - 0x5b30));
    func_80314f50(cpu);
    if (on()) fprintf(stderr, "[audio] initBuffer DONE      cur=%08x fxbase=%08x dspbase=%08x\n",
                      cur(), fxbase(cpu.gpr[13]), mem_r32(cpu.gpr[13] - 0x5c08));
}

SUNBRIGHT_OVERRIDE(dbg_start_sound_set, 0x80014d1cu) {
    if (on()) fprintf(stderr, "[audio] MSound::startSoundSet cur=%08x\n", cur());
    func_80014d1c(cpu);
}
SUNBRIGHT_OVERRIDE(dbg_init_iface_main, 0x80300ab8u) {
    if (on()) fprintf(stderr, "[audio] initInterfaceMain    cur=%08x\n", cur());
    func_80300ab8(cpu);
}
SUNBRIGHT_OVERRIDE(dbg_init_driver, 0x80301a28u) {
    if (on()) fprintf(stderr, "[audio] JAIBasic::initDriver cur=%08x heap(r3)=%08x fxbase(before)=%08x\n",
                      cur(), cpu.gpr[3], fxbase(cpu.gpr[13]));
    func_80301a28(cpu);
    if (on()) fprintf(stderr, "[audio] JAIBasic::initDriver DONE cur=%08x fxbase(after)=%08x\n",
                      cur(), fxbase(cpu.gpr[13]));
}
SUNBRIGHT_OVERRIDE(dbg_init_data, 0x80303facu) {
    if (on()) fprintf(stderr, "[audio] JAIData::initData    cur=%08x fxbase=%08x  <-- NULL fxbase => crash\n",
                      cur(), fxbase(cpu.gpr[13]));
    func_80303fac(cpu);
}

SUNBRIGHT_OVERRIDE(dbg_get_fx_handle, 0x8031505cu) {
    const u32 base = fxbase(cpu.gpr[13]);
    if (on() && base == 0) {
        static unsigned long n = 0;
        if (n++ < 4)
            fprintf(stderr, "[audio] getFXHandle(idx=%u) cur=%08x fxbase=NULL -> returns %08x (BAD)\n",
                    cpu.gpr[3], cur(), cpu.gpr[3] * 0x20);
    }
    func_8031505c(cpu);
}
#endif

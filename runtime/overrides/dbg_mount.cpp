// TEMP diagnostic: why does TApplication::mountStageArchive (802a5bd4) never mount?
// It polls 0x8034e548, which returns nonzero while a DVD command-block is pending
// (globals A=r13-22832, B=r13-22840, C=r13-22856). Log the result + globals so we can
// see whether the recomp is stuck waiting on a DVD read that never completes.
#include "../overrides.h"
#include <cstdio>
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE
extern "C" void func_8034e548(CPUState& cpu);
extern "C" void func_802a5bd4(CPUState& cpu);
extern u32 mem_r32(u32 ea);

SUNBRIGHT_OVERRIDE(dbg_mountstage, 0x802a5bd4u) {       // TApplication::mountStageArchive entry
    if (getenv("SUNBRIGHT_DBG_MOUNT")) {
        static unsigned long n = 0;
        if ((n++ % 20) == 0)
            fprintf(stderr, "[mount] mountStageArchive entry #%lu r30=%08x\n", n, cpu.gpr[30]);
    }
    func_802a5bd4(cpu);   // super-call the real body
}

SUNBRIGHT_OVERRIDE(dbg_8034e548, 0x8034e548u) {
    const u32 r13 = cpu.gpr[13];
    func_8034e548(cpu);   // super-call the real body
    if (getenv("SUNBRIGHT_DBG_MOUNT")) {
        static unsigned long n = 0;
        if ((n++ % 50) == 0)
            fprintf(stderr, "[mount] 8034e548 -> r3=%08x  A=%08x B=%08x C=%08x (lr=%08x)\n",
                    cpu.gpr[3], mem_r32(r13 - 22832), mem_r32(r13 - 22840),
                    mem_r32(r13 - 22856), cpu.lr);
    }
}
#endif

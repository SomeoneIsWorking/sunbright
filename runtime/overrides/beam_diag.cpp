// TEMP diagnostic (SUNBRIGHT_BEAM_DIAG=1): the TBeamManager ctor (0x800deec0) crashes with a
// near-NULL `this` (wild write to ea=0x3ad). This probe answers the decisive fork: is `this`
// (r3) already garbage AT ENTRY (a bad caller / a bad `operator new` result), or does it get
// clobbered across the nested __construct_array call? It also watches the array helper
// __construct_array (0x80337f14) — the ctor passes it a code-computed element-ctor pointer
// (0x800df944) that the recompiler never emitted. Delete once root-caused.
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>
#include <cstdio>

static bool diag_on() { static const bool on = getenv("SUNBRIGHT_BEAM_DIAG"); return on; }

// TBeamManager::TBeamManager(const char*)
static void ov_tbeam_ctor(CPUState& cpu) {
    u32 self = cpu.gpr[3];
    if (diag_on()) {
        bool bogus = self != 0 && self < 0x80000000u;
        std::fprintf(stderr, "[beam] TBeamManager::ct this=%08x name=%08x lr=%08x%s\n",
                     self, cpu.gpr[4], cpu.lr, bogus ? "  <<< BOGUS this" : "");
    }
    if (RecompFunc o = recomp_raw(0x800deec0u)) o(cpu); else call_ppc(cpu, cpu.lr);
}

// __construct_array(array, ctor, dtor, elemsize, count)  (r3,r4,r5,r6,r7)
static void ov_construct_array(CPUState& cpu) {
    u32 array = cpu.gpr[3], ctor = cpu.gpr[4], elemsize = cpu.gpr[6], count = cpu.gpr[7];
    u32 saved_r30 = cpu.gpr[30], saved_r29 = cpu.gpr[29], saved_r31 = cpu.gpr[31];
    if (diag_on()) {
        bool bogus = array != 0 && array < 0x80000000u;
        std::fprintf(stderr, "[beam] __construct_array array=%08x ctor=%08x size=%u count=%u%s\n",
                     array, ctor, elemsize, count, bogus ? "  <<< BOGUS array" : "");
    }
    if (RecompFunc o = recomp_raw(0x80337f14u)) o(cpu); else call_ppc(cpu, cpu.lr);
    if (diag_on() && (cpu.gpr[30] != saved_r30 || cpu.gpr[29] != saved_r29 || cpu.gpr[31] != saved_r31))
        std::fprintf(stderr, "[beam] __construct_array CLOBBERED non-volatiles: "
                     "r29 %08x->%08x  r30 %08x->%08x  r31 %08x->%08x\n",
                     saved_r29, cpu.gpr[29], saved_r30, cpu.gpr[30], saved_r31, cpu.gpr[31]);
}

static const bool beam_diag_reg = [] {
    if (getenv("SUNBRIGHT_BEAM_DIAG")) {
        register_override(0x800deec0u, &ov_tbeam_ctor);
        register_override(0x80337f14u, &ov_construct_array);
    }
    return true;
}();

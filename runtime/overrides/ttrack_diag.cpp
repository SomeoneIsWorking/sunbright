// TEMP diagnostic (SUNBRIGHT_TTRACK_DIAG=1): func_8031d83c (JASystem::TTrack tick) recompiled
// hangs boot. Its wrap loop is `while (phase >= limit) phase -= limit` with phase=*(this+940),
// rate=*(this+944), limit=*(r2+1916). Log those per call to see if a value goes wrong (limit→0,
// phase→huge/NaN) under our recomp. Delete once root-caused.
#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include <cstdlib>
#include <cstdio>

static void ov_ttrack_tick(CPUState& cpu) {
    if (getenv("SUNBRIGHT_TTRACK_DIAG")) {
        u32 self = cpu.gpr[3], r2 = cpu.gpr[2];
        float phase = (self >= 0x80000000u && self < 0x81800000u) ? mem_rf32(self + 940) : 0.f;
        float rate  = (self >= 0x80000000u && self < 0x81800000u) ? mem_rf32(self + 944) : 0.f;
        u8    flag  = (self >= 0x80000000u && self < 0x81800000u) ? mem_r8(self + 964)  : 0;
        float limit = (r2 >= 0x80000000u && r2 < 0x81800000u) ? mem_rf32(r2 + 1916) : 0.f;
        static int n = 0;
        if (n++ < 60)
            std::fprintf(stderr, "[ttrack] this=%08x flag=%u phase=%.6f rate=%.6f limit=%.6f\n",
                         self, flag, phase, rate, limit);
    }
    if (RecompFunc o = recomp_raw(0x8031d83cu)) o(cpu); else call_ppc(cpu, cpu.lr);
}

static const bool ttrack_diag_reg = [] {
    if (getenv("SUNBRIGHT_TTRACK_DIAG")) register_override(0x8031d83cu, &ov_ttrack_tick);
    return true;
}();

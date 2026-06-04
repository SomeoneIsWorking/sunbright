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
    const bool diag = getenv("SUNBRIGHT_TTRACK_DIAG");
    u32 self = cpu.gpr[3], r2 = cpu.gpr[2];
    auto okp = [](u32 p){ return p >= 0x80000000u && p < 0x81800000u; };
    if (diag) {
        float phase = okp(self) ? mem_rf32(self + 940) : 0.f;
        float rate  = okp(self) ? mem_rf32(self + 944) : 0.f;
        u8    flag  = okp(self) ? mem_r8(self + 964)  : 0;
        float limit = okp(r2)   ? mem_rf32(r2 + 1916) : 0.f;
        // Log EVERY call, flush immediately: the LAST line printed before the run stalls is the
        // call whose recomp body (→updateSeq→TSeqParser) never returns = the freezing input.
        std::fprintf(stderr, "[ttrack] this=%08x flag=%u phase=%.6f rate=%.6f limit=%.6f\n",
                     self, flag, phase, rate, limit);
        std::fflush(stderr);
    }
    if (RecompFunc o = recomp_raw(0x8031d83cu)) o(cpu); else call_ppc(cpu, cpu.lr);
    if (diag) {
        float phase = okp(self) ? mem_rf32(self + 940) : 0.f;
        std::fprintf(stderr, "[ttrack]   -> returned r3=%d phase_after=%.6f\n", (int)cpu.gpr[3], phase);
        std::fflush(stderr);
    }
}

static const bool ttrack_diag_reg = [] {
    if (getenv("SUNBRIGHT_TTRACK_DIAG")) register_override(0x8031d83cu, &ov_ttrack_tick);
    return true;
}();

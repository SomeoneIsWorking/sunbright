// DIAGNOSTIC (SUNBRIGHT_DBG_THP): observe the THP video IDCT entry
// (__THPDecompressiMCURowNxN 0x8036fc94) — dumps the paired-single GQR quantization config + the
// output pointer on the first MCU row. Useful when chasing THP/paired-single decode bugs (it's how
// the GQR5=s16 / GQR6=u8,scale-3 layout was confirmed). Observes only — super-calls the real body,
// so behaviour is unchanged. Cold path (THP decode only); zero log cost when the env var is unset.
//
// The residual FMV "comb" itself was a `dcbz` mistranslation (emitted as a no-op; it must zero the
// 32-byte coefficient block) — fixed in the recompiler (c_emitter.cpp / dcbz32), found by force_jit
// bisecting this function's sub-calls vs the Dolphin oracle. See memory paired-single-recomp-bugs.
#include "../overrides.h"
#include <cstdio>
#include <cstdlib>

SUNBRIGHT_OVERRIDE(ov_dbg_thp_idct, 0x8036fc94) {
    static const bool on = getenv("SUNBRIGHT_DBG_THP") != nullptr;
    static bool logged = false;
    RecompFunc raw = recomp_raw(0x8036fc94u);   // the real generated body
    if (raw == nullptr) return;                  // shouldn't happen (it's recompiled)
    if (on && !logged) {
        logged = true;
        fprintf(stderr, "[thp] __THPDecompressiMCURowNxN first call: out r3=%08x\n", cpu.gpr[3]);
        for (int g = 0; g < 8; g++)
            if (cpu.gqr[g])
                fprintf(stderr, "[thp]   GQR%d=%08x st(type=%u scale=%u) ld(type=%u scale=%u)\n",
                        g, cpu.gqr[g], cpu.gqr[g] & 7, (cpu.gqr[g] >> 8) & 0x3F,
                        (cpu.gqr[g] >> 16) & 7, (cpu.gqr[g] >> 24) & 0x3F);
    }
    raw(cpu);
}

// PC-native port of func_8031d83c — the JASystem TTrack per-tick update (the "tick" that drives
// each audio track's sequence advance). The recompiled version deterministically freezes right
// after w1stLoad: force_jit bisection pins the bug uniquely to this function (force_jit 8031d83c
// clears it 0/6; force_jit of every callee path — parser, cmd handlers, updateSeq/updateTrack/
// mainProc — does NOT). The freeze is its phase-wrap loop never terminating, even though the
// generated C reads as a faithful translation. This faithful native port behaves CORRECTLY:
// phase accumulates ~rate/tick (tempo), reaches limit, the wrap loop runs ONE iteration
// (TTrack::mainProc drops phase below limit), repeat — normal playback (SUNBRIGHT_TICK_LOG).
//
// ROOT CAUSE STILL UNEXPLAINED — do NOT re-assert a guess. Ruled OUT: limit kept in guest f31
// being clobbered across the TTrack::mainProc call (probe: f31 survived, 0 clobbers over 10 wrap
// iterations). The recomp's own wrap loop spins where this faithful native equivalent does not;
// the exact divergence is unknown — exactly the "issue we can't even see" this native port
// bypasses. Flagged for a recompiler-level investigation (debugging-path branch 2): faithful
// generated C that still diverges points at the recomp execution model, not one bad instruction.
//
// Owning it natively: limit/phase live in C locals; we super-call the (correct) recomp callees by
// address. This override is the SOLE path (always registered); the recomp body stays reachable via
// recomp_raw(0x8031d83c) for A/B diffing only. SUNBRIGHT_TICK_LOG dumps the per-tick decision +
// wrap loop. Verified: clears the freeze (was 6/6 frozen) and boots through to the intro movie /
// game-data load, VI fields advancing, matching pure-Dolphin (SUNBRIGHT_DISABLE_RECOMP=1) progress.
#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include <cstdlib>
#include <cstdio>

namespace {

// Run a recomp function by address with args already in cpu.gpr; result lands in cpu.gpr[3].
inline void super(CPUState& cpu, u32 addr) {
    if (RecompFunc o = recomp_raw(addr)) o(cpu);
    else { cpu.lr = 0x8031d9c8u; call_ppc(cpu, addr); }
}

constexpr u32 UPDATE_SEQ   = 0x8031b940u;  // TTrack::updateSeq(self,0,1)
constexpr u32 TTRACK_MAIN  = 0x8031bb08u;  // TTrack::mainProc(self) -> s8 status
constexpr u32 CLOSE_TRACK  = 0x8031c914u;  // TTrack::closeTrack(self)
constexpr u32 DEALLOC_ROOT = 0x8031defcu;  // TrackMgr::deAllocRoot(self)
constexpr u32 OSC_GETOFF   = 0x80315674u;  // TOscillator::getOffset(osc)
constexpr u32 INC_SELF_OSC = 0x8031ba14u;  // TTrack::incSelfOsc(child)

void finish(CPUState& cpu, u32 self) {                 // updateSeq; closeTrack; deAllocRoot; ret -1
    cpu.gpr[3]=self; cpu.gpr[4]=0; cpu.gpr[5]=1; super(cpu, UPDATE_SEQ);
    cpu.gpr[3]=self;                              super(cpu, CLOSE_TRACK);
    cpu.gpr[3]=self;                              super(cpu, DEALLOC_ROOT);
    cpu.gpr[3]=(u32)-1;
}

void channel_update(CPUState& cpu, u32 self) {
    // loop1: 2 oscillator slots — if status (self+0x3a0+i*4)==14, getOffset(self+0x33c+i*32)
    for (int i = 0; i < 2; i++) {
        if ((s32)mem_r32(self + 0x3a0 + i*4) == 14) { cpu.gpr[3] = self + 0x33c + i*32; super(cpu, OSC_GETOFF); }
    }
    // loop2: 16 child slots — if child ptr (self+0x2c4+i*4)!=0, incSelfOsc(child)
    for (int i = 0; i < 16; i++) {
        u32 child = mem_r32(self + 0x2c4 + i*4);
        if (child != 0) { cpu.gpr[3] = child; super(cpu, INC_SELF_OSC); }
    }
    cpu.gpr[3] = 0;
}

void ov_ttrack_tick(CPUState& cpu) {
    const bool log = getenv("SUNBRIGHT_TICK_LOG");
    const u32 self = cpu.gpr[3];
    const u32 r2   = cpu.gpr[2];

    if (self == 0)            { cpu.gpr[3] = (u32)-1; return; }
    u8 flag = mem_r8(self + 0x3c4);
    if (flag == 0)            { cpu.gpr[3] = (u32)-1; return; }
    if (flag == 3)            { finish(cpu, self); return; }

    const float limit = mem_rf32(r2 + 1916);           // C local — cannot be clobbered by callees
    float phase = mem_rf32(self + 0x3ac) + mem_rf32(self + 0x3b0);   // phase += rate
    mem_wf32(self + 0x3ac, phase);
    phase = mem_rf32(self + 0x3ac);                    // reload (round-trip through f32, as the orig)

    if (log) {
        static int n = 0;
        if (n++ < 40)
            std::fprintf(stderr, "[tick] self=%08x flag=%u r2=%08x phase=%.6f limit=%.6f path=%s\n",
                         self, flag, r2, phase, limit, (phase < limit) ? "noWrap" : "WRAP");
    }
    if (phase < limit) {
        cpu.gpr[3]=self; cpu.gpr[4]=0; cpu.gpr[5]=1; super(cpu, UPDATE_SEQ);
    } else {
        int iters = 0;
        while (phase >= limit) {
            phase = phase - limit; mem_wf32(self + 0x3ac, phase);
            cpu.gpr[3] = self; super(cpu, TTRACK_MAIN);
            s32 r = (s32)(s8)cpu.gpr[3];               // extsb
            if (log && iters < 50)
                std::fprintf(stderr, "[tick] self=%08x iter=%d phase=%.6f limit=%.6f mainProc=%d\n",
                             self, iters, phase, limit, r);
            iters++;
            if (r == -1) { finish(cpu, self); return; }
            phase = mem_rf32(self + 0x3ac);            // reload for the re-check
        }
    }
    channel_update(cpu, self);
}

const bool ttrack_tick_native_reg = [] {
    // SUNBRIGHT_NO_TTRACK_NATIVE=1: A/B diagnostic — run the recompiled tick instead (the
    // original phase-wrap spin may be obsolete since the call-model/jump-table fixes).
    if (getenv("SUNBRIGHT_NO_TTRACK_NATIVE")) {
        fprintf(stderr, "[ttrack] native tick DISABLED (recompiled body runs)\n");
        return false;
    }
    register_override(0x8031d83cu, &ov_ttrack_tick);   // sole path — owns the JASystem TTrack tick
    return true;
}();

}  // namespace

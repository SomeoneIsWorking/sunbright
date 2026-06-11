// JASystem::TOscillator (GMSE01) — PC-native port of the envelope oscillator.
//
// Why: the silent-music bug is at the voice level. The sequencer is fully exonerated
// (song seq builds, ticks at oracle rate, cursors track the oracle, BankMgr::noteOn
// always succeeds), yet every played voice is stopped within an instant:
// TDSPChannel::play 131 / stop 131 / forceStopOsc 357 per run, and /vpb shows zero
// enabled voices during the title song. The kill decision comes from the channel
// update seeing the envelope oscillator report "done" — TOscillator, self-contained
// table-driven FP math, is the prime suspect under recomp and the right unit to own
// natively (port-don't-emulate).
//
// Faithful port of the decomp (reference/sms JASOscillator.cpp, verified field
// offsets against the USA emitted code). Globals (GMSE01, r13=0x804141C0):
//   Kernel::sDacRate        f32 @ 0x8040CDF0 (r13-29648, read by release())
//   Driver::sUpdateInterval u8  @ 0x8040CDA8 (r13-29720, getUpdateInterval)
//
// Overridden entries:
//   0x803155dc init / 0x80315614 initStart / 0x80315674 getOffset /
//   0x80315780 getOffsetNoCount / 0x803157dc forceStop / 0x80315818 release /
//   0x803159a0 calc(s16*)
// f32 results return in f1 (cpu.fpr[1].ps0), bool in r3 — per PPC EABI.

#include "../overrides.h"
#include "../intrinsics.h"

#include <cmath>

namespace {

// ---- guest layout ------------------------------------------------------------------
// TOscillator: +0 mOsc*, +4 mState u8, +5 unk5 u8, +6 unk6 u16, +8 mReleaseRate f32,
// +C mPhase f32, +10 mTargetPhase f32, +14 mPhaseChangeRate f32, +18 mDirectRelease u16,
// +1C mInitialReleaseRate f32.
// Osc_: +0 mode u8, +4 rate f32, +8 attackTbl s16*, +C releaseTbl s16*, +10 width f32,
// +14 base f32.

constexpr u32 kDacRate        = 0x8040CDF0u;
constexpr u32 kUpdateInterval = 0x8040CDA8u;

// Driver::FORCE_STOP_TABLE — accessed natively (the guest copy's address isn't in the
// map; the contents are constant and version-independent).
const s16 kForceStopTable[6] = { 0, 15, 0, 15, 0, 0 };

const f32 kRelTableSampleCell[17] = {
    1.0f,        0.970489f,   0.781274f,   0.54628098f, 0.39979199f,
    0.28931499f, 0.21210399f, 0.15747599f, 0.112613f,   0.081789598f,
    0.0579852f,  0.0436415f,  0.0308237f,  0.0237129f,  0.0152593f,
    0.00915555f, 0.0f,
};
const f32 kRelTableSqRoot[17] = {
    1.0f,      0.878906f,  0.765625f, 0.660156f,   0.5625f,   0.472656f,
    0.390625f, 0.316406f,  0.25f,     0.191406f,   0.140625f, 0.097656198f,
    0.0625f,   0.0351562f, 0.015625f, 0.00390625f, 0.0f,
};
const f32 kRelTableSquare[17] = {
    1.0f,        0.96824598f, 0.935414f, 0.90138799f, 0.86602497f,
    0.82915598f, 0.790569f,   0.75f,     0.707107f,   0.66143799f,
    0.61237198f, 0.559017f,   0.5f,      0.43301299f, 0.353553f,
    0.25f,       0.0f,
};

struct Osc {     // host view of guest TOscillator
    u32 ea;
    u32  osc()        const { return mem_r32(ea + 0x00); }
    u8   state()      const { return mem_r8 (ea + 0x04); }
    u8   u5()         const { return mem_r8 (ea + 0x05); }
    u16  u6()         const { return mem_r16(ea + 0x06); }
    f32  relRate()    const { return mem_rf32(ea + 0x08); }
    f32  phase()      const { return mem_rf32(ea + 0x0C); }
    f32  target()     const { return mem_rf32(ea + 0x10); }
    f32  changeRate() const { return mem_rf32(ea + 0x14); }
    u16  directRel()  const { return mem_r16(ea + 0x18); }
    f32  initRel()    const { return mem_rf32(ea + 0x1C); }

    void set_osc(u32 v)        { mem_w32(ea + 0x00, v); }
    void set_state(u8 v)       { mem_w8 (ea + 0x04, v); }
    void set_u5(u8 v)          { mem_w8 (ea + 0x05, v); }
    void set_u6(u16 v)         { mem_w16(ea + 0x06, v); }
    void set_relRate(f32 v)    { mem_wf32(ea + 0x08, v); }
    void set_phase(f32 v)      { mem_wf32(ea + 0x0C, v); }
    void set_target(f32 v)     { mem_wf32(ea + 0x10, v); }
    void set_changeRate(f32 v) { mem_wf32(ea + 0x14, v); }
    void set_directRel(u16 v)  { mem_w16(ea + 0x18, v); }
    void set_initRel(f32 v)    { mem_wf32(ea + 0x1C, v); }

    // Osc_ params
    f32 p_rate()   const { return mem_rf32(osc() + 0x04); }
    u32 p_attack() const { return mem_r32(osc() + 0x08); }
    u32 p_rel()    const { return mem_r32(osc() + 0x0C); }
    f32 p_width()  const { return mem_rf32(osc() + 0x10); }
    f32 p_base()   const { return mem_rf32(osc() + 0x14); }
};

inline f32 rate_per_tick() {
    return ((mem_rf32(kDacRate) / 80.0f) / 600.0f);
}
inline f32 update_interval() { return (f32)mem_r8(kUpdateInterval); }

// table read: guest s16* tables, or our native force-stop table (sentinel ea==1).
inline s16 tbl(u32 t, int idx) {
    if (t == 1u) return kForceStopTable[idx];
    return (s16)mem_r16(t + (u32)idx * 2);
}

inline void ret_f32(CPUState& cpu, f32 v) { cpu.fpr[1].ps0 = (f64)v; }

// ---- TOscillator::calc(table) — shared by getOffset and the direct entry ------------
f32 osc_calc(Osc o, u32 table) {
    while (o.relRate() <= 0.0f) {
        int idx = o.u6() * 3;
        o.set_phase(o.target());
        if (o.state() == 6) { o.set_state(0); break; }

        const s16 envMode  = tbl(table, idx);
        const s16 envTime  = tbl(table, idx + 1);
        const s16 envValue = tbl(table, idx + 2);

        if (envMode == 13) { o.set_u6((u16)envValue); continue; }
        if (envMode == 15) { o.set_state(0); break; }
        if (envMode == 14) {
            o.set_state(3);
            return o.p_base() + o.phase() * o.p_width();
        }

        o.set_u5((u8)envMode);

        if (envTime == 0) {
            o.set_target((f32)envValue / 32768.0f);
            o.set_u6(o.u6() + 1);
            continue;
        }

        const f32 rr = (f32)envTime * rate_per_tick() / update_interval();
        o.set_relRate(rr);
        o.set_initRel(rr);
        o.set_target((f32)envValue / 32768.0f);

        if (o.u5() == 0)
            o.set_changeRate((o.target() - o.phase()) / rr);
        else
            o.set_changeRate(o.target() - o.phase());

        o.set_u6(o.u6() + 1);
    }

    if (o.p_width() == 0.0f)
        return o.p_base();

    f32 newPhase;
    f32 cr = 0.0f;
    if ((f64)o.initRel() == 0.0) {
        newPhase = o.target();
        o.set_phase(newPhase);
    } else if (o.u5() == 0 || (cr = o.changeRate()) == 0.0f) {
        newPhase = o.target() - (o.changeRate() * o.relRate());
        o.set_phase(newPhase);
    } else if (o.u5() == 3 || o.u5() == 1 || o.u5() == 2) {
        const f32* t = (o.u5() == 3) ? kRelTableSampleCell
                     : (o.u5() == 1) ? kRelTableSquare
                                     : kRelTableSqRoot;
        f32 fIdx = (cr < 0.0f) ? 16.0f * (1.0f - (o.relRate() / o.initRel()))
                               : 16.0f * (o.relRate() / o.initRel());
        u32 idx  = (u32)fIdx;
        f32 prop = fIdx - (f32)idx;
        if (idx >= 16) { idx = 15; prop = 1.0f; }
        const f32 valAbs = std::fabs(cr * (prop * (t[idx + 1] - t[idx]) + t[idx]));
        newPhase = (o.changeRate() < 0.0f) ? o.target() + valAbs
                                           : o.target() - (o.changeRate() - valAbs);
        o.set_phase(newPhase);
    } else {
        newPhase = o.target() - (cr * o.relRate());
        o.set_phase(newPhase);
    }

    return newPhase * o.p_width() + o.p_base();
}

// ---- entry points --------------------------------------------------------------------

SUNBRIGHT_OVERRIDE(ov_osc_init, 0x803155dcu) {
    Osc o{ cpu.gpr[3] };
    o.set_osc(0); o.set_state(1); o.set_u5(0); o.set_u6(0);
    o.set_relRate(0.0f); o.set_phase(0.0f); o.set_target(0.0f);
    o.set_changeRate(0.0f); o.set_directRel(0); o.set_initRel(0.0f);
}

SUNBRIGHT_OVERRIDE(ov_osc_initStart, 0x80315614u) {
    Osc o{ cpu.gpr[3] };
    o.set_state(2); o.set_directRel(0);
    if (o.osc() == 0 || o.p_attack() == 0) { o.set_phase(0.0f); return; }
    o.set_u6(0); o.set_relRate(0.0f); o.set_target(0.0f); o.set_directRel(0);
    o.set_relRate(o.relRate() - o.p_rate());
}

SUNBRIGHT_OVERRIDE(ov_osc_getOffset, 0x80315674u) {
    Osc o{ cpu.gpr[3] };
    if (o.osc() == 0) { o.set_phase(1.0f); ret_f32(cpu, 1.0f); return; }
    switch (o.state()) {
    case 0: ret_f32(cpu, 1.0f); return;
    case 3: ret_f32(cpu, o.p_base() + o.phase() * o.p_width()); return;
    case 1: o.set_state(2); /* fallthrough */
    default: {
        u32 table;
        if      (o.state() == 4) table = o.p_rel();
        else if (o.state() == 5) table = 1u;          // native FORCE_STOP_TABLE
        else                     table = o.p_attack();
        if (table == 0 && o.state() != 6) { o.set_phase(1.0f); ret_f32(cpu, 1.0f); return; }
        if (o.state() == 5) o.set_relRate(o.relRate() - 1.0f);
        else                o.set_relRate(o.relRate() - o.p_rate());
        ret_f32(cpu, osc_calc(o, table));
        return;
    }
    }
}

SUNBRIGHT_OVERRIDE(ov_osc_getOffsetNoCount, 0x80315780u) {
    Osc o{ cpu.gpr[3] };
    if (o.osc() == 0) { o.set_phase(1.0f); ret_f32(cpu, 1.0f); return; }
    if (o.state() == 0) { ret_f32(cpu, 1.0f); return; }
    if (o.state() == 1) o.set_state(2);
    ret_f32(cpu, o.phase() * o.p_width() + o.p_base());
}

SUNBRIGHT_OVERRIDE(ov_osc_forceStop, 0x803157dcu) {
    Osc o{ cpu.gpr[3] };
    if (o.state() == 5) { cpu.gpr[3] = 0; return; }
    o.set_u6(0); o.set_relRate(0.0f); o.set_target(o.phase()); o.set_state(5);
    cpu.gpr[3] = 1;
}

SUNBRIGHT_OVERRIDE(ov_osc_release, 0x80315818u) {
    Osc o{ cpu.gpr[3] };
    if (o.state() == 5) { cpu.gpr[3] = 0; return; }

    if (o.p_attack() != o.p_rel()) {
        o.set_u6(0); o.set_relRate(0.0f); o.set_target(o.phase());
    }
    if (o.p_rel() == 0 && o.directRel() == 0)
        o.set_directRel(0x10);

    if (o.directRel() != 0) {
        o.set_state(6);
        o.set_u5((o.directRel() >> 14) & 3);
        f32 rr = (f32)(o.directRel() & 0x3FFF) * rate_per_tick() / update_interval();
        if (rr < 1.0f) rr = 1.0f;
        o.set_relRate(rr);
        o.set_initRel(rr);
        o.set_target(0.0f);
        if (o.u5() == 0)
            o.set_changeRate((o.target() - o.phase()) / rr);
        else
            o.set_changeRate(o.target() - o.phase());
    } else {
        o.set_state(4);
    }
    cpu.gpr[3] = 1;
}

SUNBRIGHT_OVERRIDE(ov_osc_calc, 0x803159a0u) {
    Osc o{ cpu.gpr[3] };
    ret_f32(cpu, osc_calc(o, cpu.gpr[4]));
}

} // namespace

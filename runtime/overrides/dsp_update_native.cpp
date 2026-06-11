// JASystem::TDSPChannel::updateAll (0x80314c60, GMSE01) — PC-native port.
//
// ROOT CAUSE of the silent-music bug ("single-frame samples"): updateAll opens with a
// DSP-overload limiter — it measures OSGetTick() deltas between the 8 DSP subframe
// syncs and, when history[0]/delta < DSP_LIMIT_RATIO, calls breakLowerActive(126),
// force-stopping every voice below priority 126 (all music). On hardware the 8 syncs
// arrive evenly paced (~700µs) and the ratio never trips in normal play. Under the
// hybrid, Dolphin's DSP HLE delivers sync mails INSTANTLY, so the deltas are tiny and
// erratic and the limiter fired every frame — measured: TDSPChannel::play 131 /
// stop 131 / forceStop 357 per run, every voice dead within a frame; SE "blips" were
// each note's first frame. Same disease class as the audioproc intcount==0 thread
// suicide (HLE instantaneity breaking a hardware pacing assumption).
//
// The port owns the overload policy: on PC there is no DSP to overload, so the
// limiter is dropped; the tick bookkeeping (old_time, history[]) is kept fresh for
// any other reader. Everything else is a faithful port of the decomp's 64-voice loop
// (callbacks/queue helpers invoked via their recompiled entries).
//
// Globals (GMSE01, r13 = 0x804141C0):
//   DSPCH (TDSPChannel[64] base ptr)  @ r13-23584 = 0x8040E5A0
//   old_time (OSTick)                 @ r13-23568 = 0x8040E5B0
//   history[8] (u32)                  @ 0x803E2FC0
//   DSP_LIMIT_RATIO (f32)             @ r13-29708 (unused — limiter dropped)
// Callees (recompiled): OSGetTick 0x803494f0, AudioThread::getDSPSyncCount 0x803110e8,
//   DSPBuffer::flushChannel 0x8031549c, DSPQueue::deQueue 0x80311600,
//   DSPQueue::checkQueue 0x80311768, PPCSync 0x80341ab8.
//
// TDSPChannel stride 0x14: +1 unk1 (1 = unused), +4 u16 unk4 (watchdog target),
// +6 u16 unk6 (callback countdown), +C DSPBuffer*, +10 callback ptr.
// DSPBuffer (guest VPB image): +0 u16 enabled, +2 u16 done, +0x10A u16 end_requested,
// +0x10C u16 watchdog counter.

#include "../overrides.h"
#include "../intrinsics.h"

namespace {

constexpr u32 kDSPCHPtr    = 0x8040E5A0u;
constexpr u32 kOldTime     = 0x8040E5B0u;
constexpr u32 kHistory     = 0x803E2FC0u;
constexpr u32 kOSGetTick   = 0x803494f0u;
constexpr u32 kGetSyncCnt  = 0x803110e8u;
constexpr u32 kFlush       = 0x8031549cu;
constexpr u32 kDeQueue     = 0x80311600u;
constexpr u32 kCheckQueue  = 0x80311768u;
constexpr u32 kPPCSync     = 0x80341ab8u;

inline u32 callg(CPUState& cpu, u32 fn, u32 a = 0, u32 b = 0) {
    cpu.gpr[3] = a; cpu.gpr[4] = b;
    call_ppc(cpu, fn);
    return cpu.gpr[3];
}

SUNBRIGHT_OVERRIDE(ov_dsp_updateAll, 0x80314c60u) {
    // Tick bookkeeping (kept faithful); the overload break is intentionally absent.
    const u32 now  = callg(cpu, kOSGetTick);
    const u32 prev = mem_r32(kOldTime);
    mem_w32(kOldTime, now);
    const u32 sync = callg(cpu, kGetSyncCnt);
    const u32 slot = (7u - sync) & 7u;
    mem_w32(kHistory + slot * 4, now - prev);

    const u32 base = mem_r32(kDSPCHPtr);
    for (u32 i = 0; i < 64; i++) {
        const u32 ch  = base + i * 0x14u;
        const u32 buf = mem_r32(ch + 0xC);
        if (mem_r8(ch + 1) == 1)
            continue;

        if (mem_r16(buf + 2) != 0) {                       // DSP reported voice done
            const u32 cb = mem_r32(ch + 0x10);
            if (cb)
                mem_w16(ch + 6, (u16)callg(cpu, cb, ch, 2));
            mem_w16(buf + 2, 0);
            mem_w16(buf + 0, 0);
            callg(cpu, kFlush, buf);
        }

        if (mem_r16(buf + 0x10A) == 0) {                   // no end requested: watchdog
            const u16 cnt = (u16)(mem_r16(buf + 0x10C) + 1);
            mem_w16(buf + 0x10C, cnt);
            const u32 cb = mem_r32(ch + 0x10);
            if (cnt == mem_r16(ch + 4) && cb)
                callg(cpu, cb, ch, 4);
        }

        const u32 cb = mem_r32(ch + 0x10);
        if (cb) {
            const u16 t = (u16)(mem_r16(ch + 6) - 1);
            mem_w16(ch + 6, t);
            if (t == 0) {
                const u16 r = (u16)callg(cpu, cb, ch, 0);
                mem_w16(ch + 6, r);
                if (r == 0) {
                    mem_w16(buf + 2, 0);
                    mem_w16(buf + 0, 0);
                    callg(cpu, kDeQueue, 1);
                    callg(cpu, kFlush, buf);
                }
            }
        }
    }

    callg(cpu, kCheckQueue);
    callg(cpu, kPPCSync);
}

} // namespace

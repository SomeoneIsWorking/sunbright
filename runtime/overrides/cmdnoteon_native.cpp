// TSeqParser::cmdNoteOn (0x80320f30, GMSE01) — PC-native port.
//
// Why: the silent-BGM bug lives in the note-on decision path. The song seq builds,
// ticks at the correct rate (1137/s vs oracle 1162/s), cursors track the oracle, and
// every BankMgr::noteOn that runs SUCCEEDS — yet only ~0.5 notes/s ever reach it.
// cmdNoteOn is the fork: tie-mode/connect-case routes a note to gateOn (modulate the
// existing channel — silent if none) instead of noteOn (allocate + start a voice).
// Owning this decision natively makes it observable and fixable in the port.
//
// Faithful port of the decomp (reference/sms JASSeqParser.cpp), verified against the
// USA emitted code (generated func_80320f30): same field offsets (transpose +0x3C0,
// NoteMgr at +0xB4: chans[8]+0xB4 / baseTime+0xE4 / connectCase+0xE8 / lastNote+0xE9 /
// beforeTie+0xEA), same callees:
//   exchangeRegisterValue 0x8031d004, seqTimeToDspTime 0x8031bfac,
//   TTrack::noteOn 0x8031a818, TTrack::gateOn 0x8031abdc,
//   TNoteMgr::getChannel 0x80319c08 (unnamed in map), TChannel::setKeySweepTarget 0x80312a44.
//
// Args: r3 = TSeqParser* (unused), r4 = TTrack*, r5 = note byte. Returns r3 = retCode.

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr u32 kExchangeRegisterValue = 0x8031d004u;
constexpr u32 kSeqTimeToDspTime      = 0x8031bfacu;
constexpr u32 kTrackNoteOn           = 0x8031a818u;
constexpr u32 kTrackGateOn           = 0x8031abdcu;
constexpr u32 kSetKeySweepTarget     = 0x80312a44u;

// TTrack field offsets (GMSE01, verified against emitted code)
constexpr u32 kSeqCur      = 0x004;   // TSeqCtrl::mCurrentFilePtr
constexpr u32 kSeqWait     = 0x008;   // TSeqCtrl::mWaitTimer
constexpr u32 kNoteChans   = 0x0B4;   // TNoteMgr::unk0[8]
constexpr u32 kBaseTime    = 0x0E4;   // TNoteMgr::mBaseTime
constexpr u32 kConnectCase = 0x0E8;   // TNoteMgr::mConnectCase
constexpr u32 kLastNote    = 0x0E9;   // TNoteMgr::mLastNote
constexpr u32 kBeforeTie   = 0x0EA;   // TNoteMgr::mBeforeTieMode
constexpr u32 kTranspose   = 0x3C0;   // unk3C0 (emitted: +960)
constexpr u32 kPauseFlag   = 0x3CD;   // unk3CD
constexpr u32 kFlags3C1    = 0x3C1;   // unk3C1

inline u8 read_byte(u32 trk) {
    const u32 cur = mem_r32(trk + kSeqCur);
    mem_w32(trk + kSeqCur, cur + 1);
    return mem_r8(cur);
}

// Call a recompiled guest function; args in r3.., result from r3.
inline u32 callg(CPUState& cpu, u32 fn, u32 a, u32 b, u32 c = 0, u32 d = 0, u32 e = 0) {
    cpu.gpr[3] = a; cpu.gpr[4] = b; cpu.gpr[5] = c; cpu.gpr[6] = d; cpu.gpr[7] = e;
    call_ppc(cpu, fn);
    return cpu.gpr[3];
}

SUNBRIGHT_OVERRIDE(ov_cmdNoteOn, 0x80320f30u) {
    static const bool dbg = getenv("SUNBRIGHT_DBG_SEQ") != nullptr;
    const u32 trk  = cpu.gpr[4];
    const u8  note = (u8)cpu.gpr[5];

    u8 key = (u8)(note + mem_r8(trk + kTranspose));

    const u8 flag = read_byte(trk);
    if (flag & 0x80)
        key = (u8)(callg(cpu, kExchangeRegisterValue, trk, note) + mem_r8(trk + kTranspose));

    u8 sweep_key = 0;
    if ((flag >> 5) & 0x2) {                 // key-sweep form: target = key, start = lastNote
        sweep_key = key;
        key = mem_r8(trk + kLastNote);
    }

    u8 vel = read_byte(trk);
    if (vel >= 0x80)
        vel = (u8)callg(cpu, kExchangeRegisterValue, trk, vel - 0x80);

    u8  voice;                                // gate/voice slot (0 = lead note-on form)
    u8  timebase;
    u32 dur;
    if (!(flag & 0x7)) {
        voice    = 0;
        timebase = read_byte(trk);
        if (timebase >= 0x80)
            timebase = (u8)callg(cpu, kExchangeRegisterValue, trk, timebase - 0x80);
        dur = 0;
        const u8 nbytes = (flag >> 3) & 0x3;
        for (u8 i = 0; i < nbytes; ++i)
            dur = (dur << 8) | read_byte(trk);
        if (nbytes == 1 && dur >= 0x80)
            dur = callg(cpu, kExchangeRegisterValue, trk, dur - 0x80);
    } else {
        voice = flag & 0x7;
        if ((flag >> 3) & 0x3)
            voice = (u8)callg(cpu, kExchangeRegisterValue, trk, voice - 1);
        dur      = 0xFFFFFFFFu;
        timebase = 100;
    }

    const u8 connect = (flag >> 5) & 0x3;
    mem_w8(trk + kConnectCase, connect);

    const u8 pause_gated = mem_r8(trk + kPauseFlag) && (mem_r8(trk + kFlags3C1) & 0x10);
    const u8 before_tie  = mem_r8(trk + kBeforeTie);

    s32 dsp_time = (s32)dur;
    if (before_tie) {
        if (connect & 1)
            dsp_time = -1;
        if (dsp_time != -1)
            dsp_time = (s32)callg(cpu, kSeqTimeToDspTime, trk, (u32)dsp_time, timebase);
        if (!pause_gated)
            callg(cpu, kTrackGateOn, trk, voice, key, vel, (u32)dsp_time);
    } else {
        if (dsp_time != -1)
            dsp_time = (s32)callg(cpu, kSeqTimeToDspTime, trk, (u32)dsp_time, timebase);
        if (connect & 1)
            dsp_time = -1;
        if (!pause_gated)
            callg(cpu, kTrackNoteOn, trk, voice, key, vel, (u32)dsp_time);
    }

    if (dbg) {
        static unsigned long n = 0;
        if ((n++ % 16) == 0 || (connect == 0 && !before_tie))
            fprintf(stderr,
                    "[cmdnote] trk=%08x key=%02x vel=%02x dur=%u conn=%u tie=%u gated=%u path=%s -> r3=%08x\n",
                    trk, key, vel, dur, connect, before_tie, pause_gated,
                    before_tie ? "gateOn" : "noteOn", cpu.gpr[3]);
    }

    mem_w32(trk + kBaseTime, dur);
    mem_w8(trk + kBeforeTie, (connect & 1) ? 1 : 0);

    if (connect & 0x2) {                      // key-sweep continuation
        if (dsp_time == -1)
            dsp_time = (s32)callg(cpu, kSeqTimeToDspTime, trk, dur, timebase);
        const u32 chan = mem_r32(trk + kNoteChans + 0);   // getChannel(0)
        if (chan)
            callg(cpu, kSetKeySweepTarget, chan,
                  (u8)(sweep_key + mem_r8(trk + kTranspose)), (u32)dsp_time);
        key = sweep_key;
    }

    mem_w8(trk + kLastNote, key);

    if (dur == 0xFFFFFFFFu) { cpu.gpr[3] = 0; return; }

    mem_w32(trk + kSeqWait, dur ? dur : 0xFFFFFFFFu);     // wait(dur); 0 → -1 (wait-for-noteoff)
    cpu.gpr[3] = 1;
}

} // namespace

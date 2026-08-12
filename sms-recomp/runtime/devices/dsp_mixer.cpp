// dsp_mixer.cpp — the DSP voice renderer: the last missing link in sms-recomp's audio chain.
//
// WHAT WAS MISSING. The game's JAS sequencer runs (it is recompiled PPC like everything else) and
// fills a Voice Parameter Block per active voice. The MIXING is done by the GameCube DSP, a
// coprocessor whose microcode is not PPC and therefore is not recompiled. dev_aid.cpp already
// beats the audio heartbeat at the hardware's own rate — 70 blocks per cycle, 57.2 wraps/s =
// 32000/560 exactly — and delivers each wrap's interrupt, which is what drives the guest's
// updateDac and keeps the VPBs current. But nothing ever wrote samples into the DMA buffer, so
// every one of those blocks was 32 bytes of silence. This is the code that fills them.
//
// This is NOT a DSP emulator, and deliberately so: it is the same voice renderer the decomp
// runtime already proved audible (sms-boot/runtime/jas_kernel_native.cpp, title BGM landed
// 2026-07-17), re-pointed at GUEST memory. The decomp reads its own native DSPBuffer objects;
// here the VPBs live in guest RAM in guest layout, big-endian, so every field is read through the
// guest accessors at the offsets RE'd from the DOL rather than through a C++ struct.
//
// WHY THE OFFSETS ARE RE'D HERE AND NOT TAKEN FROM THE DECOMP HEADER. JASDSPInterface.hpp's
// `Channel` sub-struct comments its members at 0x0/0x4/0x8/0xC, which would make it 16 bytes and
// put `unk10[6]` at 0x10..0x70 — straight over `unk50`, which the same header places at 0x50. The
// two cannot both be right. Disassembling the setters settles it:
//
//   setMixerVolume     @0x80315444:  rlwinm r4,r4,3  ; addi r4,r4,0x10 ; sth r5,2(r4)
//   setMixerInitVolume @0x80315420:  rlwinm r4,r4,3  ; sth r5,4(r4) ; sth r5,2(r4)
//
// `bus << 3` — the stride is EIGHT, target volume at +2 and current at +4. The header's inner
// comments are stale. That mattered: reading the decomp's layout would have taken the volume from
// the wrong halfword of the wrong bus, which does not crash and does not read as a layout bug —
// it reads as a mixer that is merely quiet or lopsided.
//
// setWaveInfo @0x803152c8 confirms the rest at the offsets used below: 0x118 wave, 0x64 block
// samples, 0x100 block bytes, 0x11C length, 0x102 loop flag, 0x110/0x114 loop bounds.
//
// DIAGNOSTICS: SBR_LUCENT_DEBUG=dspmix. Every count printed carries its denominator, because
// "0 voices rendered" has several causes with opposite fixes — the channel table not allocated
// yet, every channel free, every voice paused, or a wave the decoder declines.

#include "mmio.h"
#include "intrinsics.h"
#include "guest_sched.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

extern u8* g_ram_base;
extern "C" const u8* sbr_aram_base();
extern "C" u32 sbr_aram_size();

namespace {

// ── GUEST LAYOUT ────────────────────────────────────────────────────────────────────────────────
// TDSPChannel: 64 entries of 0x14 bytes, reached through a pointer in small data. Both the stride
// and the pointer's location are read off TDSPChannel::updateAll @0x80314c60:
//
//   80314d50: lwz  r0, -0x5c20(r13)   ; TDSPChannel::DSPCH
//   80314d54: add  r28, r0, r29       ; + byte offset
//   80314d58: lbz  r0, 1(r28)         ; .unk1  (1 = free)
//   80314d5c: lwz  r31, 0xc(r28)      ; .unkC  (the VPB)
//   80314e50: addi r30,r30,1 ; cmplwi r30,0x40 ; addi r29,r29,0x14
constexpr u32 kDspchSdaOffset = 0x5c20;   // subtracted from r13
constexpr u32 kDspchStride    = 0x14;
constexpr int kDspChannels    = 64;
constexpr u32 kChFreeFlag     = 0x01;     // byte offset of unk1 within TDSPChannel
constexpr u32 kChVpbPtr       = 0x0C;

// DSPBuffer (the VPB), 0x180 bytes, guest offsets.
constexpr u32 kVpbPitch       = 0x004;    // u16, unity = 4096
constexpr u32 kVpbPlayStart   = 0x008;    // u16, 1 = key-on; the DSP consumes it (see below)
constexpr u32 kVpbPause       = 0x00C;    // u16, non-zero = paused
constexpr u32 kVpbBus0        = 0x010;    // Channel[6], stride 8: id+0, target+2, current+4
constexpr u32 kVpbBusStride   = 0x008;
constexpr u32 kVpbBusTarget   = 0x002;
constexpr u32 kVpbBusCurrent  = 0x004;
// The AUTO MIXER (setAutoMixer @0x803153d8, initAutoMixer @0x803153ac). When it is active the game
// does NOT write the six bus volumes at all — setMixerVolume even returns early — and the DSP
// derives them from a compact (volume, pan, fxmix) triple instead. Most voices use it.
constexpr u32 kVpbAmPan        = 0x050;   // u16: pan in the HIGH byte, sub-pan in the low
constexpr u32 kVpbAmFx         = 0x052;   // u16: fxmix in the high byte (aux send; not rendered)
constexpr u32 kVpbAmVolCur     = 0x054;   // u16 Q15, ramps toward the target
constexpr u32 kVpbAmVolTarget  = 0x056;   // u16 Q15
constexpr u32 kVpbAmEnabled    = 0x058;   // u16, non-zero = the auto mixer owns this voice
constexpr u32 kVpbBlockSamps  = 0x064;    // u16: 16 = AFC, 1 = PCM
constexpr u32 kVpbBlockBytes  = 0x100;    // u16: AFC 9 (hq) / 5 (lq); PCM 16 / 8 (bits)
constexpr u32 kVpbLoopFlag    = 0x102;    // u16
constexpr u32 kVpbLoopStart   = 0x110;    // u32, sample index
constexpr u32 kVpbLoopEnd     = 0x114;    // u32, sample index
constexpr u32 kVpbWaveAddr    = 0x118;    // u32, an ARAM address (NOT a main-RAM pointer)
constexpr u32 kVpbLength      = 0x11C;    // u32, length in SAMPLES

// ── AFC ─────────────────────────────────────────────────────────────────────────────────────────
// The same coefficient table and decoder as the decomp's proven renderer
// (sms-boot/runtime/jas_kernel_native.cpp). It MUST stay byte-identical to that one, and
// `tools/audio/afc_table_check.py` (wired into the commit gate) enforces it.
//
// This table was first transcribed BY HAND and entries 8-15 came out wrong, while the comment here
// asserted it was byte-identical — a claim nobody had checked. The result was not silence or a
// crash: every AFC block whose predictor index landed in the wrong half decoded with the wrong
// coefficients, so the waveform was continuous, in tune and in time, and simply wrong. It measured
// perfectly on every metric this file reports. Only an ear caught it, and only a direct diff
// against the proven copy located it. That is why the check is now automated rather than asserted
// in a comment.
const s16 kAfcCoef[16][2] = {
    {0, 0}, {2048, 0}, {0, 2048}, {1024, 1024},
    {4096, -2048}, {3584, -1536}, {3072, -1024}, {4608, -2560},
    {4200, -2248}, {4800, -2300}, {5120, -3072}, {2048, -2048},
    {1024, -1024}, {-1024, 1024}, {-1024, 0}, {-2048, 0},
};

// Decode `nSamples` AFC samples. The VPB length is a SAMPLE count: AFC packs 16 samples per block
// (9 bytes hq / 5 lq) and waves sit back-to-back in ARAM, so the byte span is
// ceil(nSamples/16)*blockBytes. Treating it as a byte count over-ran ~1.78x into the NEXT wave and
// produced audible aliasing — a defect the decomp arc already paid for once (2026-07-17).
void afc_decode(const u8* d, u32 nSamples, bool hq, std::vector<s16>& out) {
    int yn1 = 0, yn2 = 0;
    const u32 bs = hq ? 9 : 5;
    const u32 nBlocks = (nSamples + 15) / 16;
    out.reserve(out.size() + (size_t)nBlocks * 16);
    for (u32 blk = 0; blk < nBlocks; ++blk) {
        const u8* b = d + (size_t)blk * bs;
        const int delta = 1 << (b[0] >> 4);
        const int c0 = kAfcCoef[b[0] & 0xF][0], c1 = kAfcCoef[b[0] & 0xF][1];
        for (int i = 0; i < 16; ++i) {
            int nib;
            if (hq) {
                nib = (b[1 + i / 2] >> (i % 2 ? 0 : 4)) & 0xF;
                if (nib >= 8) nib -= 16;
                nib <<= 11;
            } else {
                nib = (b[1 + i / 4] >> (6 - 2 * (i % 4))) & 3;
                if (nib >= 2) nib -= 4;
                nib <<= 13;
            }
            int s = (delta * nib + yn1 * c0 + yn2 * c1) >> 11;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            yn2 = yn1; yn1 = s;
            out.push_back((s16)s);
        }
    }
    out.resize(nSamples);   // trim the partial last block
}

// ── HOST VOICE STATE ────────────────────────────────────────────────────────────────────────────
// On hardware the ucode keeps the playback cursor and the AFC predictor history inside the VPB.
// The renderer replaces the ucode, so that state lives here, keyed by channel index — the same
// choice the decomp made, and for the same reason: the ucode's exact write-back semantics are
// fiddly and nothing else reads them.
struct HostVoice {
    u32              waveAddr = 0;    // VPB wave address at last decode; a change = re-trigger
    std::vector<s16> pcm;
    double           cursor   = 0.0;
    bool             playing  = false;
    bool             loop     = false;
    size_t           loopStart = 0;
    size_t           loopEnd   = 0;
};
HostVoice g_voice[kDspChannels];

// The rendered stereo stream waiting to be handed to the DAC, interleaved host-endian s16. Produced
// one sub-frame at a time by the renderer and drained one AID block at a time by dev_aid.cpp. Both
// run on the same thread at a fixed 560-frames-produced / 560-frames-consumed ratio per AID cycle,
// so this neither grows nor starves; it is a hand-off, not a jitter buffer.
std::vector<s16> g_ring;
size_t g_ringRead = 0;
unsigned long g_starved = 0;

// Counters. Denominators, all of them — see the file header.
unsigned long g_frames = 0, g_noTable = 0, g_rendered = 0, g_decodes = 0;
unsigned long g_seenAllocated = 0, g_seenPaused = 0, g_seenNoWave = 0, g_seenUndecodable = 0;
unsigned long g_keyOns = 0;
// Which mixing path each rendered voice took, and the observed range of the pan byte. The pan
// SCALE is the one thing here not pinned by disassembly — kAmPanCentre is an assumption, so the
// observed range is printed and a value outside it would show up as a number rather than as a
// mix that merely sounds off-centre.
unsigned long g_amVoices = 0, g_explicitVoices = 0;
unsigned g_amPanMin = 0xFFFF, g_amPanMax = 0;
constexpr u16 kAmPanCentre = 64;   // JAudio pan is 0..127 with 64 centre; verified by the range below
unsigned long g_loopFlagged=0, g_loopRejZero=0, g_loopRejBig=0, g_loopRejOrder=0;
long g_peak = 0;

void decode_voice(u32 vpb, HostVoice& hv) {
    hv.pcm.clear();
    hv.loop = false;
    const u32 waveAddr = sb_r32(vpb + kVpbWaveAddr);
    const u32 nSamples = sb_r32(vpb + kVpbLength);
    const u8* aram = sbr_aram_base();
    if (waveAddr == 0 || nSamples == 0 || aram == nullptr) return;

    const u16 blockSamps = sb_r16(vpb + kVpbBlockSamps);
    const u16 blockBytes = sb_r16(vpb + kVpbBlockBytes);

    // Refuse to read past the end of ARAM rather than walking off it. A wave whose length field is
    // wrong is a real possibility while the sequencer is mid-update, and the difference between
    // "declined it" and "read garbage" must not be silent.
    const u64 span = (blockSamps == 16)
                         ? ((u64)((nSamples + 15) / 16)) * (blockBytes == 9 ? 9u : 5u)
                         : (u64)nSamples * (blockBytes == 16 ? 2u : 1u);
    if (waveAddr + span > sbr_aram_size()) {
        ++g_seenUndecodable;
        return;
    }
    const u8* data = aram + waveAddr;

    if (blockSamps == 16) {                       // AFC
        afc_decode(data, nSamples, blockBytes == 9, hv.pcm);
    } else if (blockSamps == 1 && blockBytes == 16) {   // PCM16, big-endian
        hv.pcm.resize(nSamples);
        for (u32 i = 0; i < nSamples; ++i)
            hv.pcm[i] = (s16)(u16)((data[i * 2] << 8) | data[i * 2 + 1]);
    } else if (blockSamps == 1 && blockBytes == 8) {    // PCM8
        hv.pcm.resize(nSamples);
        for (u32 i = 0; i < nSamples; ++i) hv.pcm[i] = (s16)((s8)data[i] << 8);
    } else {
        // setOscInfo's generated-waveform path lands here (wave address 0 normally, but an
        // unexpected format would too). Counted, not guessed at.
        ++g_seenUndecodable;
        return;
    }
    ++g_decodes;

    const size_t total = hv.pcm.size();
    hv.loop      = sb_r16(vpb + kVpbLoopFlag) != 0;
    hv.loopStart = sb_r32(vpb + kVpbLoopStart);
    hv.loopEnd   = sb_r32(vpb + kVpbLoopEnd);
    if (hv.loop) {
        ++g_loopFlagged;
        if (hv.loopEnd == 0) ++g_loopRejZero;
        else if (hv.loopEnd > total) ++g_loopRejBig;
        else if (hv.loopStart >= hv.loopEnd) ++g_loopRejOrder;
        static int shown = 0;
        if (shown < 8) {
            ++shown;
            lucent::debug("dspmix", "looped wave: total={} loopStart={} loopEnd(unk114)={} "
                                    "start+end={} len(unk11C)={}",
                          total, hv.loopStart, hv.loopEnd, hv.loopStart + hv.loopEnd,
                          sb_r32(vpb + kVpbLength));
        }
    }
    if (hv.loopEnd == 0 || hv.loopEnd > total || hv.loopStart >= hv.loopEnd) hv.loop = false;
}

} // namespace

// Render one DSP frame's worth of stereo samples into the guest DMA buffer at `destAddr`, in the
// big-endian interleaved form the DAC reads — i.e. exactly what the DSP would have left there.
// Writing into the guest buffer rather than bypassing it keeps ONE output path: dev_aid.cpp's
// existing byteswap/push/SBR_AUDIO_RAW tap is unchanged and still measures what is heard.
extern "C" void sbr_dsp_mix(u32 frames) {
    if (frames == 0) return;
    ++g_frames;

    static std::vector<s32> accL, accR;
    accL.assign(frames, 0);
    accR.assign(frames, 0);

    // The channel table is allocated by the guest during audio init, so before that this is a
    // legitimate "nothing to render yet" rather than a fault — but it is COUNTED, because a run
    // that never allocates it would otherwise be indistinguishable from a run that is simply quiet.
    const u32 dspch = sb_r32(gsched_cpu().gpr[13] - kDspchSdaOffset);
    if (dspch == 0) {
        ++g_noTable;
    } else {
        // One-shot: the table's ADDRESS and its first entries, raw. "All 64 channels are free" and
        // "the pointer is garbage that happens to read as free" produce the same counters, and the
        // only thing that separates them is looking at the bytes.
        static u32 s_shown = 0;
        if (s_shown != dspch) {
            s_shown = dspch;
            lucent::debug("dspmix", "channel table @0x{:08x} (r13=0x{:08x})", dspch,
                          (u32)gsched_cpu().gpr[13]);
            for (int i = 0; i < 4; ++i) {
                const u32 c = dspch + (u32)i * kDspchStride;
                lucent::debug("dspmix",
                              "  ch{}: unk0={:02x} unk1={:02x} unk2={:02x} unk3={:02x} "
                              "unk8=0x{:08x} unkC(vpb)=0x{:08x} unk10=0x{:08x}",
                              i, sb_r8(c), sb_r8(c + 1), sb_r8(c + 2), sb_r8(c + 3),
                              sb_r32(c + 8), sb_r32(c + 0xC), sb_r32(c + 0x10));
            }
        }
        for (int ch = 0; ch < kDspChannels; ++ch) {
            const u32 chan = dspch + (u32)ch * kDspchStride;
            HostVoice& hv = g_voice[ch];

            if (sb_r8(chan + kChFreeFlag) == 1) { hv.playing = false; continue; }  // free
            ++g_seenAllocated;
            const u32 vpb = sb_r32(chan + kChVpbPtr);
            if (vpb == 0) { hv.playing = false; continue; }
            if (sb_r16(vpb + kVpbPause) != 0) { ++g_seenPaused; hv.playing = false; continue; }
            const u32 waveAddr = sb_r32(vpb + kVpbWaveAddr);
            if (waveAddr == 0) { ++g_seenNoWave; hv.playing = false; continue; }

            // KEY-ON. DSPBuffer::playStart @0x8031520c sets unk8 = 1 and zeroes the ucode's own
            // sample cursor at unk68; on hardware the DSP acts on that flag, starts the voice from
            // sample 0, and clears it when it writes the VPB back. This renderer replaces the DSP,
            // so it does the same — including clearing the flag, which is not tidiness: leaving it
            // set would restart the voice on every one of the 400 sub-frames per second.
            //
            // Detecting a re-trigger as "the wave address changed" is NOT sufficient, and the way
            // it fails is instructive. A channel is allocated and freed per note (~5 notes/s here),
            // so the same instrument lands on the same channel constantly — same address, no
            // detected re-trigger, and the voice stays finished forever. That produced a run where
            // the sequencer was demonstrably healthy (19,300 sequencer ticks and 37 note-ons per 7 s,
            // channels allocated and freed in step) while the output was silent after 8 seconds.
            // Every counter upstream of the mixer looked perfect, because every one of them was.
            const bool keyOn = sb_r16(vpb + kVpbPlayStart) != 0;
            if (keyOn || waveAddr != hv.waveAddr) {
                if (waveAddr != hv.waveAddr) {   // decode only on a genuinely new wave
                    hv.waveAddr = waveAddr;
                    decode_voice(vpb, hv);
                }
                hv.cursor  = 0.0;
                hv.playing = !hv.pcm.empty();
                if (keyOn) {
                    sb_w16(vpb + kVpbPlayStart, 0);
                    // Start the ramp from SILENCE. Otherwise a channel reused for a new note begins
                    // at the previous note's level and the attack is a step — the same click the
                    // ramp above exists to remove, just once per note instead of once per sub-frame.
                    sb_w16(vpb + kVpbBus0 + 0 * kVpbBusStride + kVpbBusCurrent, 0);
                    sb_w16(vpb + kVpbBus0 + 1 * kVpbBusStride + kVpbBusCurrent, 0);
                    sb_w16(vpb + kVpbAmVolCur, 0);
                    ++g_keyOns;
                }
            }
            if (!hv.playing || hv.pcm.empty()) continue;

            const double step = (double)sb_r16(vpb + kVpbPitch) / 4096.0;  // unity = 4096
            if (step <= 0.0) continue;

            // Buses 0 and 1 are main L/R. Aux buses, filters and the Dolby positional mix are the
            // fidelity milestone and are deliberately absent; their omission is silence in those
            // paths, not a wrong value in this one.
            //
            // VOLUME IS A RAMP, NOT A LEVEL, and applying it as a level is audible. Each bus keeps
            // TWO volumes — `current` at +4 and `target` at +2 (setMixerInitVolume seeds both,
            // setMixerVolume writes only the target plus a ramp delay) — and the DSP walks current
            // toward target across the sub-frame rather than jumping. Reading the target and
            // applying it flat makes every envelope step, note-on and note-off an instantaneous
            // jump in the output: measured, 34% of all sample-to-sample jumps over 2000 landed
            // EXACTLY on the 80-sample sub-frame boundary, which is what "crackling" was.
            //
            // So the gain is interpolated across the chunk from current to target, and current is
            // written back — which is the DSP's own job, since it is the DSP that stores the VPB
            // back after rendering. The game's envelope then moves the target each sub-frame and
            // the result is a continuous ramp instead of a staircase.
            auto q15 = [](u16 v) -> float {
                const s16 s = (s16)v;
                return s > 0 ? (float)s / 32768.0f : 0.0f;
            };
            // ONE-SHOT: what the six bus slots actually route to. `setBusConnect` @0x803155b8
            // stores a routing CODE into unk10[bus].id from a table at 0x803e3000, so the slot
            // index is not itself a destination — assuming slot 0 = L and slot 1 = R is an
            // assumption, and this prints whether it holds.
            {
                static int shown = 0;
                bool sounding = false;
                for (int k = 0; k < 6; ++k)
                    if ((s16)sb_r16(vpb + kVpbBus0 + (u32)k * kVpbBusStride + kVpbBusTarget) > 0)
                        sounding = true;
                if (sounding && shown < 12) {
                    ++shown;
                    std::string b;
                    for (int k = 0; k < 6; ++k) {
                        const u32 e = vpb + kVpbBus0 + (u32)k * kVpbBusStride;
                        b += " [" + std::to_string(k) + "] id=0x" +
                             [](u16 v){ char t[8]; std::snprintf(t, sizeof t, "%04x", v); return std::string(t); }(sb_r16(e)) +
                             " tgt=" + std::to_string((s16)sb_r16(e + kVpbBusTarget)) +
                             " cur=" + std::to_string((s16)sb_r16(e + kVpbBusCurrent));
                    }
                    lucent::debug("dspmix", "ch{} buses:{}", ch, b);
                }
            }
            const u32 busL = vpb + kVpbBus0 + 0 * kVpbBusStride;
            const u32 busR = vpb + kVpbBus0 + 1 * kVpbBusStride;
            float gL0, gR0, gL, gR;
            const bool autoMixer = sb_r16(vpb + kVpbAmEnabled) != 0;
            if (autoMixer) {
                // THE AUTO MIXER, and this is why most of the mix was missing. Reading only the six
                // explicit bus volumes looked correct — the fields are real, the layout was RE'd
                // from the setters, and the handful of voices that do write them came out right. It
                // just left every auto-mixed voice at gain zero. Measured: live voices carrying
                // full-scale decoded audio (samples of +-10989, +-7475) with cur=0 tgt=0 on both
                // main buses, so they rendered and contributed nothing.
                const u16 pan = (u16)((sb_r16(vpb + kVpbAmPan) >> 8) & 0xFF);
                float x = (float)pan / (float)kAmPanCentre * 0.5f;   // 0 = hard left, 1 = hard right
                if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
                ++g_amVoices;
                if (pan < g_amPanMin) g_amPanMin = pan;
                if (pan > g_amPanMax) g_amPanMax = pan;
                const float v0 = q15(sb_r16(vpb + kVpbAmVolCur));
                const float v1 = q15(sb_r16(vpb + kVpbAmVolTarget));
                // Linear pan for v1. The ucode uses a table and an equal-power curve; that is a
                // fidelity refinement, and getting these voices AUDIBLE at roughly the right
                // position is the correctness step. Stated rather than left to be inferred.
                gL0 = v0 * (1.0f - x); gR0 = v0 * x;
                gL  = v1 * (1.0f - x); gR  = v1 * x;
            } else {
                ++g_explicitVoices;
                gL0 = q15(sb_r16(busL + kVpbBusCurrent));
                gR0 = q15(sb_r16(busR + kVpbBusCurrent));
                gL  = q15(sb_r16(busL + kVpbBusTarget));
                gR  = q15(sb_r16(busR + kVpbBusTarget));
            }
            const float dgL = (gL - gL0) / (float)frames;
            const float dgR = (gR - gR0) / (float)frames;

            // Which of the two possible zeros this is: no VOLUME, or silent SAMPLES. They have
            // opposite fixes and the render count cannot tell them apart.
            {
                static long n = 0;
                if ((++n % 20000) == 0) {
                    s16 mx = 0;
                    for (s16 v : hv.pcm) { const s16 a = v < 0 ? (s16)-v : v; if (a > mx) mx = a; }
                    lucent::debug("dspmix", "live ch{}: gainL={:.4f} gainR={:.4f} (cur={} tgt={}) "
                                            "pcm |max|={} len={} step={:.3f} cursor={:.0f} "
                                            "sample@cursor={} contribution={:.1f}",
                                  ch, gL, gR, (s16)sb_r16(busL + kVpbBusCurrent),
                                  (s16)sb_r16(busL + kVpbBusTarget), mx, hv.pcm.size(), step,
                                  hv.cursor, (size_t)hv.cursor < hv.pcm.size() ? hv.pcm[(size_t)hv.cursor] : (s16)0,
                                  ((size_t)hv.cursor < hv.pcm.size() ? hv.pcm[(size_t)hv.cursor] : (s16)0) * gL);
                }
            }
            const size_t len = hv.pcm.size();
            const size_t boundary = hv.loop ? hv.loopEnd : len;
            double c = hv.cursor;
            for (u32 i = 0; i < frames; ++i) {
                size_t idx = (size_t)c;
                if (idx >= boundary) {
                    if (!hv.loop) { hv.playing = false; break; }
                    const double span = (double)(hv.loopEnd - hv.loopStart);
                    do { c -= span; } while ((size_t)c >= boundary);
                    idx = (size_t)c;
                }
                const s16 s0 = hv.pcm[idx];
                const s16 s1 = (idx + 1 < len) ? hv.pcm[idx + 1] : s0;
                const float samp = s0 + (s1 - s0) * (float)(c - (double)idx);  // linear interp
                accL[i] += (s32)(samp * (gL0 + dgL * (float)i));
                accR[i] += (s32)(samp * (gR0 + dgR * (float)i));
                c += step;
            }
            hv.cursor = c;
            // The ramp completed, so current IS the target now. Written back because the DSP writes
            // the VPB back after rendering; skipping it would leave `current` stale and the next
            // sub-frame would ramp from the wrong place every time.
            if (autoMixer) {
                sb_w16(vpb + kVpbAmVolCur, sb_r16(vpb + kVpbAmVolTarget));
            } else {
                sb_w16(busL + kVpbBusCurrent, sb_r16(busL + kVpbBusTarget));
                sb_w16(busR + kVpbBusCurrent, sb_r16(busR + kVpbBusTarget));
            }
            ++g_rendered;
        }
    }

    // Mix in 32-bit headroom, clamp once at the end, and append to the HOST ring the DAC is fed
    // from — deliberately NOT into the guest's DMA buffer.
    //
    // Writing the guest buffer was the obvious model (it is where the DSP puts its output) and it
    // was wrong. The guest's own audio path re-interleaves DSPBuf's triple buffer into that same
    // buffer between our store and the DMA read, and DSPBuf holds silence here because DsyncFrame2
    // has no DSP behind it — so ~17% of our sub-frames were overwritten with zeros. In the output
    // that is an 80-sample hole punched into continuous music every few milliseconds, which is
    // exactly what "crackling" sounded like.
    //
    // Measured, and this is the pair of numbers that settles it: read straight back from guest
    // memory after the store, 0.76% of chunks were silent; in the stream the DAC actually emitted,
    // 18.1% were. Same chunks, two different readers, and the difference is entirely what the guest
    // wrote in between. Mixing into a host buffer removes the shared-write window rather than
    // racing for it, and nothing in the guest reads its DAC buffer back.
    for (u32 i = 0; i < frames; ++i) {
        s32 l = accL[i], r = accR[i];
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        if (l > g_peak) g_peak = l;
        g_ring.push_back((s16)l);
        g_ring.push_back((s16)r);
    }

    if ((g_frames % 4000) == 0) {   // ~10 s at 400 sub-frames/s
        lucent::debug("dspmix",
                      "{} sub-frame(s): {} voice-renders, {} key-on(s), {} wave decode(s), peak {}. "
                      "Channel scan saw {} allocated, {} paused, {} with no wave, {} undecodable; "
                      "{} frame(s) found NO channel table at all. Voices mixed: {} via the AUTO MIXER (pan byte seen in [{}, {}], centre assumed {}), {} via explicit bus volumes. DAC hand-off starved {} time(s) "
                      "(a non-zero count means blocks were emitted with no rendered audio behind "
                      "them, which is audible). Loops: {} flagged, rejected "
                      "{} zero-end / {} past-end / {} out-of-order.{}",
                      g_frames, g_rendered, g_keyOns, g_decodes, g_peak, g_seenAllocated, g_seenPaused,
                      g_seenNoWave, g_seenUndecodable, g_noTable, g_amVoices, g_amPanMin == 0xFFFF ? 0u : g_amPanMin, g_amPanMax,
                      (unsigned)kAmPanCentre, g_explicitVoices, g_starved, g_loopFlagged, g_loopRejZero, g_loopRejBig, g_loopRejOrder,
                      g_rendered == 0
                          ? "   <-- NOTHING RENDERED. Read the scan counts: all-free means the "
                            "sequencer never allocated a channel, all-paused means it did and the "
                            "game muted them, no-wave means allocated voices carry no sample."
                          : "");
    }
}

// Hand one AID block (8 stereo frames) to the caller. Returns false when the renderer has not
// produced that audio yet — which must never happen in steady state (one AID cycle produces exactly
// as many frames as it consumes) and so is counted and reported rather than quietly filled.
extern "C" bool sbr_dsp_take(s16* out, u32 frames) {
    const size_t need = (size_t)frames * 2;
    if (g_ring.size() - g_ringRead < need) {
        ++g_starved;
        return false;
    }
    std::memcpy(out, g_ring.data() + g_ringRead, need * sizeof(s16));
    g_ringRead += need;
    // Compact once the consumed prefix dominates, rather than erasing per block.
    if (g_ringRead >= 4096) {
        g_ring.erase(g_ring.begin(), g_ring.begin() + (long)g_ringRead);
        g_ringRead = 0;
    }
    return true;
}

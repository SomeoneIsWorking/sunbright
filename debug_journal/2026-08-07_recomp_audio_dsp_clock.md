# 2026-08-07 — sms-recomp has audio: the mixer was easy, the DSP's CLOCK was the work

User: *"can you add audio also?"*, then, on the first version, *"audio is super slowmo"*.

`sms-recomp` is now audible — music and sound effects. The renderer is
`runtime/devices/dsp_mixer.cpp`; the interrupts that drive it are in `dev_aid.cpp`. Full status and
measurements in `docs/audio/recomp_plan.md`.

## The mixer was the small half

The decomp runtime already had a proven native voice renderer (title BGM audible since 2026-07-17,
`sms-boot/runtime/jas_kernel_native.cpp`). Porting it to the recomp is mechanical in shape — read
the same VPB fields out of GUEST memory, big-endian, instead of out of native C++ objects — and the
AFC decoder is kept byte-identical to the decomp's on purpose, because it is the component with a
verified test vector and two divergent copies of a decoder make a fidelity bug unattributable.

**One thing there could not be taken from the decomp: the field offsets.**
`JASDSPInterface.hpp` comments `Channel`'s members at 0x0/0x4/0x8/0xC, which makes it 16 bytes and
puts `unk10[6]` at 0x10..0x70 — straight over `unk50`, which the same header places at 0x50. Both
cannot be true. The setters settle it:

    setMixerVolume     @0x80315444:  rlwinm r4,r4,3 ; addi r4,r4,0x10 ; sth r5,2(r4)
    setMixerInitVolume @0x80315420:  rlwinm r4,r4,3 ; sth r5,4(r4) ; sth r5,2(r4)

`bus << 3` — the stride is EIGHT, target at +2, current at +4. The decomp is self-consistent because
its own setters fill its own struct; nothing there crosses to guest memory, so the stale comments
never hurt it. Reading them here would have taken the volume from the wrong halfword of the wrong
bus — which does not crash and does not read as a layout bug. It reads as a mixer that is quiet.

## The four bugs, and why each one lied

JAS clocks its entire render pipeline off DSP interrupts. There is no DSP. Each missing interrupt
failed in a way that pointed somewhere else.

### 1. No frame completion — one frame, then a permanent stall

`DSPBuf::process` hands a frame to the DSP and sets `dspstatus = 1`. The only thing that ever clears
it is the audio thread receiving the completion interrupt and calling `DSPBuf::finishDSPFrame`
(`JASAudioThread.cpp:95`, message 1 when `intcount` drains). Without it the idle-kick in the read
path — `if (dspstatus == 0) finishDSPFrame()` — can never fire again.

The probe that found it walked the whole path and printed every count on ONE line:

    MSBgm::startBGM=2  TSeqParser::mainProc=30  TTrack::noteOn=0
    BankMgr::noteOn=0  TDSPChannel::alloc=0     updateAll=1

That single `1` is the diagnosis. The game had asked for music, the sequencer had started, and it
ran exactly one frame and stopped forever. Had the probe only reported the thing I first went
looking at — "no voices are allocated" — the conclusion would have been "the sequencer never runs",
which is the opposite of true.

### 2. No SUB-frame interrupts — everything at 1/7 tempo

`gSubFrames` is 7. Per DSP frame the hardware raises seven interrupts: the first six run
`DSPBuf::updateDSP`, and updateDSP is what runs `Kernel::subframeCallback` and
`TDSPChannel::updateAll` — **the sequencer's clock**. The seventh runs `finishDSPFrame`.

`process(UNK1)` calls `updateDSP` once at its end, so supplying only `finishDSPFrame` gave the
sequencer ONE tick per frame instead of seven. Exactly 1/7 tempo: *"super slowmo"*.

**The pitch was correct the whole time**, because pitch comes from the resample step
(`vpb->unk4 / 4096`) and not from this clock. That is the useful signature: a bug that changes tempo
without changing pitch is a clock bug, and looking in the mixer for it is wasted time.

An AID cycle now runs a whole frame's interrupts — six `updateDSP` then one `finishDSPFrame` — and
renders the frame's 560 samples in seven 80-sample chunks between them, so voice parameters take
effect at sub-frame granularity as they do on hardware. `gSubFrames` is read live from
`r13-0x73cc` (the value the guest itself passes to `setDSPSyncCount`), not hardcoded to 7.

### 3. Re-trigger by "the wave address changed" — silent after 8 seconds

This is the one worth remembering, because **every counter upstream of the mixer was perfect while
it was happening**: 19,300 sequencer ticks and 37 note-ons per 7 s, channels allocated and freed in
step, 466,437 voice-renders — and the output was silence from 8 s onward.

A channel is allocated and freed per note, so the same instrument lands on the same channel
constantly. Same wave address, no detected re-trigger, and the voice stays finished forever. It only
appeared *after* bug 2 was fixed, because at 1/7 tempo channels were reused too slowly to expose it —
so fixing a real bug made the symptom worse, which is a shape worth expecting rather than panicking
at.

The real signal is `DSPBuffer::playStart` @0x8031520c setting **`unk8 = 1`** and zeroing the ucode's
own sample cursor at `unk68`. The renderer acts on that flag and clears it, which is what the DSP
does when it writes the VPB back — and not clearing it would restart every voice on all 400
sub-frames per second.

### 4. Rendering into the GUEST's DMA buffer — crackling

Reported as *"tempo, speed, pitch are fine but it is crackling"*.

Writing the mixed samples into the guest's AI DMA buffer is the obvious model — it is where the DSP
puts its output — and it is wrong. The guest's own audio path re-interleaves DSPBuf's triple buffer
into that same buffer between our store and the DMA read, and DSPBuf holds SILENCE here because
`DsyncFrame2` has no DSP behind it. Roughly one sub-frame in six was overwritten with zeros: an
80-sample (2.5 ms) hole punched into continuous music every few milliseconds.

**The measurement that settled it** was reading our own output back from guest memory immediately
after the store and comparing with the stream the DAC actually emitted — the same chunks, two
readers, with only the guest running in between:

| | silent 80-sample chunks |
|---|---|
| read back straight after the store | **0.76%** |
| in the emitted stream | **18.1%** |

Two numbers that could not both be true of a mixer producing silence. Before this, two plausible
theories had already been measured and killed: a per-sub-frame volume STEP (fixed by ramping
`current` toward `target`, which is what the hardware does — but the boundary jumps did not move),
and the loop bounds being misread (`unk114` measured as a genuine loop END, ~= total, start < end,
zero rejections). Neither was it.

The renderer now mixes into a HOST buffer that `dev_aid.cpp` drains one AID block at a time. This
removes the shared-write window instead of racing for it, and nothing in the guest reads its DAC
buffer back. Production and consumption are locked at 560 frames per AID cycle, so the hand-off
neither grows nor starves; a starve is counted and reported.

| | before | after |
|---|---|---|
| silent 80-sample chunks | 18.1% | **0.56%** (= the content's own silence) |
| sample jumps > 2000 | 293 | **107** |
| ...of those, on the sub-frame boundary | 37.4% | **3.7%** (uniform = 1.2%) |
| max adjacent-sample delta | 5,434 | **3,857** |

## Verification

155 s captured (`SBR_AUDIO_RAW`), Delfino Plaza:

| | |
|---|---|
| non-silent seconds | **154 of 154 (100%)** |
| peak / rms / clipped | 8,504 / 993 / **0** |
| adjacent-delta / rms | **0.083** (white noise ≈ 1.4, so it is a waveform, not noise) |
| spectral peak-to-median | **737** |
| beat autocorrelation | **64.7 BPM** — half-time of ~129, musically plausible |
| clock | 57 wraps/s, 7 sub-frames each, `badSub=0`; `updateAll` at 391/s against a hardware 400/s |

The tempo check is deliberately one that could have failed loudly: a 7x error would have landed the
beat at ~20 or ~900 BPM, both outside the 60-220 BPM window the estimator searched.

**Not established by any of this: whether it sounds RIGHT.** Numbers can confirm a waveform, a
harmonic series and a plausible tempo; they cannot confirm the arrangement, the balance, or a wrong
instrument. That is the user's ear, and the capture was sent for it.

## Residuals, measured not assumed

- **Centre-panned** — L and R are bit-identical. v1 reads bus 0/1 target volumes; pan/aux routing
  goes through `setBusConnect`'s `connect_table` and the Dolby pan matrix, which are the fidelity
  milestone.
- **DC offset ~-420 that VARIES with the music** (per-second std 457, range -1998..+276), so it
  rides on the voices rather than being a fixed bias in the mixer. Consistent with looped AFC waves
  decoded once without restoring the loop predictor/history (`unk104`/`unk106`) — the same
  simplification the proven decomp renderer makes.
- Streamed audio (DTK / movie soundtracks) is a separate path, untouched and still silent.

## Lesson

Four times in one session the counters upstream of the defect were healthy, and each time that was
the reason the defect was hard to see. The probe that finally worked printed the WHOLE
chain on one line with every count side by side — `startBGM / mainProc / noteOn / alloc / updateAll`
— so the answer was "the first zero, left to right" rather than a hypothesis to test. A probe that
had reported only the stage I suspected would have confirmed the wrong story each time.

The crackle added a second lesson: when a producer and a consumer disagree about the same bytes,
**read the bytes back at both ends**. Every counter inside the mixer said it had produced audio, and
it had; the samples were destroyed after it finished. No amount of instrumentation inside the
renderer could have found that, and two reasonable theories were measured and discarded first
because they were about the renderer. The one measurement that worked compared the two readers.

And one instrument lied outright along the way: a per-sub-frame silence counter reported 100% silent
while the same run reported a peak of 11,550. Those cannot both be true, so the instrument was wrong
and everything it had said was discarded rather than reasoned about. A separate lucent format string
had a mismatched argument count and printed a stray integer instead of the fields asked for — the
same silent-no-op class as the earlier doc edits, and the reason every string replacement in this
session is now asserted.

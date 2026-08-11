# Audio in sms-recomp — the plan (2026-07-23)

## Premise correction (READ FIRST)

**"Audio is silent by omission in BOTH runtimes" is FALSE.** The DECOMP runtime (`sms-boot`) has a
working native voice renderer with **audible title BGM**, landed 2026-07-17:
`sms-boot/runtime/jas_kernel_native.cpp` (native `DsyncFrame2`, bit-exact AFC/ADPCM decoder,
PCM8/16, linear resample, L/R bus mix, ARAM rebasing). Verified output: peak 6162, rms 287, 20 live
voices. **In-repo audio ORACLE for the same scene:** `scratch/wav/title_bgm_2026-07-17.wav`.

Only **sms-recomp** is silent. `docs/codemap.md` was stale on this and is corrected.

## Why sms-recomp is silent

The game's JAS sequencer runs (recompiled PPC) but only PREPARES voice blocks and DSP command lists.
The mixing happens on the GameCube DSP — a coprocessor whose microcode is not PPC, so not recompiled.

**The concrete dead link:** the audio-DMA (AID) registers `0xCC005030–0xCC00503A` (in the DSP block,
NOT the AI block) are claimed by `dev_aram.cpp` and stored as inert halfwords. Nothing counts blocks
down, nothing raises AID. That is the missing heartbeat.

Also note: `dev_ai.cpp`'s header comment is wrong — AI (`0xCC006C00`) is DTK/streaming and the sample
counter, NOT the DAC DMA engine.

`dev_dsp.cpp` strips `MAIL_FULL` on write, so every voice command the guest mails is discarded, and
`MailFromDSP` is permanently 0. The four `native_dsp.cpp` overrides deliberately sever the chain
(no-op `__OSInitAudioSystem` / `__DSP_boot_task`, faked task completion) — documented seams, but the gate.

## Prior art (resurrect, don't re-derive)

- `9283f44^:runtime/overrides/zelda_ucode_native.cpp` (393 lines) — an SMS-specific Zelda-ucode mail
  state machine driving Dolphin's `ZeldaAudioRenderer`, with root causes already in-comment.
- `aid_native.cpp`, `jas_driver_native.cpp`, `dsp_update_native.cpp`, `native_dsp_regs.cpp`.
- `extern/dolphin_fork/Source/Core/Core/HW/DSPHLE/` — full DSPHLE incl. `UCodes/Zelda.cpp` (in the Dolphin fork, not this repo) (1964 lines).
- SMS's ucode confirmed: CRC `0x56D36052` → `ZeldaUCode`, flags `SYNC_PER_FRAME | NO_CMD_0D`,
  standard protocol, ARAM present. `JASDSPInterface.hpp` even cites `Zelda.cpp:683` for the 0x180-byte VPB.

Licensing is a non-issue for this project (user directive).

## STATUS — AUDIBLE (2026-08-07). Steps 0-4 landed.

`sms-recomp/runtime/devices/dev_aid.cpp` is the AID engine + delivery + the DSP frame's interrupts;
`sms-recomp/runtime/devices/dsp_mixer.cpp` is the voice renderer. Measured, SBR_FASTBOOT, 155 s:

| gate | result |
|---|---|
| steps 0-3 (2026-07-23) | engine + delivery + tap beating at the hardware rate, backlog stable, **every sample zero** |
| clock | 57 wraps/s, 7 sub-frames each, `badSub=0`; `TDSPChannel::updateAll` at **391/s** (hardware: gSubFrames x 57.2 = 400) |
| renderer | 911,156 voice-renders, 6,619 key-ons, 5,544 wave decodes, **0 undecodable** |
| output | **100% of seconds non-silent**, peak ~8,800, rms 1,150, **0 clipped**, silent sub-frame chunks **0.56%**, sample jumps >2000 **107 in 60 s** with no boundary alignment, beat autocorrelation **64.7 BPM** = half-time of ~129, musically plausible |

### FOUR bugs, and each one hid behind healthy-looking counters upstream

The mixer was the easy part. What cost the time was that JAS's pipeline is clocked entirely by DSP
interrupts that do not exist here, and each missing piece failed in a way that looked like something
else.

**1. No frame completion — one frame, then a permanent stall.** `DSPBuf::process` hands a frame over
and sets `dspstatus = 1`; only the audio thread receiving the DSP's completion interrupt and calling
`DSPBuf::finishDSPFrame` ever clears it (`JASAudioThread.cpp:95`). Without it the idle-kick
(`if (dspstatus == 0) finishDSPFrame()`) can never fire again. Measured: `MSBgm::startBGM=2
TSeqParser::mainProc=30 TTrack::noteOn=0 TDSPChannel::alloc=0 updateAll=1`. The game HAD asked for
music and the sequencer HAD started — one frame, then nothing, forever. Supplied one
`finishDSPFrame` per AID cycle.

**2. No SUB-frame interrupts — everything at 1/7 tempo.** `gSubFrames` is 7. Per frame the DSP
raises 7 interrupts: the first 6 run `DSPBuf::updateDSP` (which is what runs
`Kernel::subframeCallback` and `TDSPChannel::updateAll` — **the sequencer's clock**) and the 7th runs
`finishDSPFrame`. `process(UNK1)` calls `updateDSP` once at its end, so supplying only
`finishDSPFrame` gave one sequencer tick per frame instead of seven. Reported as *"audio is super
slowmo"*. **The pitch was correct throughout**, because pitch comes from the resample step and not
from this clock — a bug that changes tempo without changing pitch points at the clock and nowhere
near the mixer. An AID cycle now runs a whole frame of interrupts, rendering the 560 samples in
seven 80-sample chunks between them, as the hardware does.

**3. Re-trigger detected as "the wave address changed" — silent after 8 s.** A channel is allocated
and freed per note, so the same instrument lands on the same channel constantly; same address, no
detected re-trigger, and the voice stays finished forever. This one is worth remembering because
**every counter upstream was perfect while it was happening** — 19,300 sequencer ticks and 37
note-ons per 7 s, channels allocated and freed in step, 466,437 voice-renders — and the output was
silence. It only appeared once bug 2 was fixed, because at 1/7 tempo channels were reused too slowly
to expose it. The real signal is `DSPBuffer::playStart` @0x8031520c setting **`unk8 = 1`** (and
zeroing the ucode cursor at `unk68`); the renderer acts on that and clears it, as the DSP does.

**4. Rendering into the guest's DMA buffer — crackling.** The guest's audio path re-interleaves
DSPBuf's (silent) triple buffer into that same buffer between our store and the DMA read, blanking
~1 sub-frame in 6: an 80-sample hole in continuous music every few ms. Found by reading our own
output back from guest memory right after the store (**0.76% silent**) and comparing with the
emitted stream (**18.1% silent**) — same chunks, two readers, only the guest in between. The mixer
now renders into a HOST buffer that `dev_aid.cpp` drains a block at a time. Silent chunks 18.1% ->
0.56%, boundary-aligned jumps 37.4% -> 3.7% (uniform 1.2%). Two earlier theories were measured and
killed first: per-sub-frame volume stepping (the ramp is right and is kept, but the jumps did not
move) and misread loop bounds (`unk114` is a genuine loop END; zero rejections).

**A layout trap worth keeping.** `JASDSPInterface.hpp` comments `Channel`'s members at 0x0/0x4/0x8/
0xC, making it 16 bytes and putting `unk10[6]` at 0x10..0x70 — over `unk50`, which the same header
places at 0x50. Both cannot be right. `setMixerVolume` @0x80315444 settles it: `rlwinm r4,r4,3` —
stride **8**, target volume at +2, current at +4. The recomp reads guest memory, so it uses the RE'd
offsets; the header's would have read the wrong halfword of the wrong bus, which does not crash and
does not look like a layout bug — it looks like a mixer that is merely quiet.

### Measured residuals (v1 scope)

- **Centre-panned**: L and R are bit-identical. v1 reads bus 0/1 target volumes only; pan/aux routing
  goes through `setBusConnect`'s `connect_table` and the Dolby pan matrix.
- **DC offset** ~-420 mean that VARIES with the music (per-second std 457), so it rides on the voices
  rather than being a fixed bias — consistent with looped AFC waves decoded once without restoring
  the loop predictor/history (`unk104`/`unk106`), the same simplification the decomp renderer makes.
- Streamed audio (DTK / movie soundtracks) is a separate path and untouched.

### Still deferred (the fidelity milestone)
Aux buses 2-5 and the FxlineConfig delay lines, IIR/FIR filters, Dolby positional mix, oscillator/
synth voices, HardStream, DTK, THP audio. None gate music or SFX.

## Minimal path to first sound (each step independently verifiable)

0. **Measure.** Counters on DSP mails, AID register writes, and the four overrides. Expect: init
   fires once, `AIInitDMA` writes 0x5030/0x5032/0x5036 once, `AIStartDMA` sets bit 15, then traffic
   STOPS. *Falsifier:* if `AIInitDMA` never fires, `AudioThread::start()` isn't reaching audioproc —
   that's a different, upstream problem.
1. **AID engine** (~60 lines). Narrow `dev_aram.cpp` to end at 0xCC005030; new `dev_aid.cpp` modelling
   `DSP.cpp::UpdateAudioDMA`: latch source/blocks on the 0→1 Enable edge, consume 32-byte blocks,
   re-arm + raise on wrap. Drive from the frame seam, paced against `aurora_audio_queued_frames()` —
   NOT free-running (documented past failure: ~75x free-run).
2. **Deliver AID** (~40 lines). Copy `dev_di.cpp::deliver_complete`'s synchronous-handler idiom; call
   `__AID_Callback` via the LIVE pointer at `0x8040E94C`. Level-triggered: coalesce, never burst.
3. **Output tap** (~30 lines). Byteswap each block BE→host, push 560-frame buffers to
   `aurora_audio_open(32000,2)` / `aurora_audio_push`. Add `SBR_AUDIO_RAW=<path>`.
   *Verify with silence:* a stable, non-climbing `aurora_audio_queued_frames()` backlog is the pacing
   proof — get it here, where it's unambiguous, before samples exist.
4. **Samples.** Revert the four `native_dsp.cpp` overrides; make the mailbox real (latch on the LO
   write; DSP→CPU FIFO); implement the boot handshake (`0x8071FEED` then the documented sequence);
   port `ZeldaAudioRenderer` (keep AFC + PCM8/16 + resampler + 6-dest volume-ramp mix + StoreVPB;
   DROP Dolby/reverb/filters for v1); port the ucode state machine.
   **⚠️ Also port `dsp_update_native.cpp`:** `TDSPChannel::updateAll` (0x80314c60) has a DSP-overload
   limiter that fires every frame under instantaneous HLE and force-stops every voice (measured 357
   forceStops/run). It will look exactly like a mixer bug.

**Key sequencing insight:** `DSPBuf::process`'s idle-kick (`if (dspstatus == 0) finishDSPFrame()`)
means the DSP-mail INTERRUPT path is not required for the first frame to render — `updateDac` alone
drives `finishDSPFrame` → `DsyncFrame2`. First audio is reachable with a synchronous mailbox and no
DSP interrupt delivery at all.

**Verify against the oracle:** peak ~6000 / rms ~300, adjacent-delta << peak (smoothness), FFT shows
harmonics not noise — cross-check `scratch/wav/title_bgm_2026-07-17.wav`.

## Deferrable
Reverb, IIR/FIR filters, Dolby positional mix, aux buses 2-5, oscillator/synth voices, HardStream,
DTK streaming, THP audio, savestates. None gate first sound.

## Open / unverified
- Whether the JAudio `audioproc` thread reaches its message loop in sms-recomp today (step 0 settles it).
- Guest addresses of `DsyncFrame2`/`DSPReleaseHalt`/`DspBoot` — the hand-written JAudio asm blob at
  `0x80336f58–0x80337cc0` has NO symbols. Only 0x80337580 and 0x80337ca0 are known. Route A doesn't
  need them; a `DsyncFrame2`-override shortcut would (and would be RE debt).
- `__DSPHandler` address (static, unsymbolised) — recover by observing `__OSSetInterruptHandler(7,…)`.

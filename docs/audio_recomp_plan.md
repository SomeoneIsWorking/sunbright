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
- `extern/dolphin_fork/Source/Core/Core/HW/DSPHLE/` — full DSPHLE incl. `UCodes/Zelda.cpp` (1964 lines).
- SMS's ucode confirmed: CRC `0x56D36052` → `ZeldaUCode`, flags `SYNC_PER_FRAME | NO_CMD_0D`,
  standard protocol, ARAM present. `JASDSPInterface.hpp` even cites `Zelda.cpp:683` for the 0x180-byte VPB.

Licensing is a non-issue for this project (user directive).

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

# Native audio arc — plan (2026-07-15)

Goal: sound at the title (and everywhere) through the ONE-RUNTIME architecture:
the decomp's own JASystem stack runs single-threaded, and the one missing native
piece — the DSP-ucode voice renderer — is ported to C++. Output through
`aurora::audio` from `sb_audio_frame()` (`sms-boot/runtime/audio_out.cpp`).

## What exists / what's missing

- Game side (JAIBasic/MSound sequencer, banks, BARC loader) is C++ and largely
  runs natively already — `sms_boot_audio.cpp` reimplements the dead Vload path
  from the real BARC table (M3-era work, still linked).
- `JASystem::AudioThread::audioproc` returns null under SMS_NATIVE_PLATFORM, so
  the JAS KERNEL (Kernel::init, Driver::init, updateDac, DSPBuf pipeline,
  TDSPChannel::updateAll) never runs → silence by omission.
- The DSP work itself is `DsyncFrame2(subframes, bufL, bufR)` (dspproc.h) — PPC
  mailbox driver for the GC DSP ucode. No native body exists. THIS is the port.

## The key identification

`DSPInterface::DSPBuffer` (JASDSPInterface.hpp) is byte-for-byte the **Zelda
ucode VPB** (voice parameter block) — the decomp itself links Dolphin's
`UCodes/Zelda.cpp` (ZeldaAudioRenderer). SMS ships the Zelda-class JAudio ucode;
the native mixer = a C++ reimplementation of that ucode's per-voice render loop
over the decomp's live DSPBuffer array:

per 5ms DAC frame (x subframes):
  for each active voice (DSPBuffer, BE fields):
    - fetch/decode samples: AFC/ADPCM (coef table), PCM8/PCM16 — decoders exist,
      PROVEN, in the recovered recomp-era engine (scratch/audio_ref/
      native_jas_recomp_era.cpp — AFC decode verified bit-perfect by ear vs ROM)
    - resample by pitch ratio (u16 fixed-point), maintain fractional position
    - volume ramps + 6-bus mixer (L/R/aux gains, targetVolume->currentVolume)
    - write back voice state (position, loop, predictor/history)
  mix buses -> dsp_buf L/R; Kernel::vframeWork imixcopy -> dac[]; push to aurora.

## Milestones

1. **Kernel wiring** — run Kernel::init + Driver::init at first sb_audio_frame
   (no DspBoot/AI/threads); per video frame run the updateDac cadence
   (32kHz: 32000/60 = 533 samples/frame; dacSize matches GC 5ms blocks — keep
   the real constants from JASRate.hpp). DSPBuf::process runs with a native
   DsyncFrame2 that renders SILENCE + one-time [STUB-CALLED] (documented seam,
   loud, temporary within the arc) — proves the sequencer/channel stack ticks
   (BMS parses, channels allocate, VPBs fill).
2. **Voice renderer v1** — PCM + AFC decode, linear resample, volume ramps,
   L/R buses only (no aux/reverb/filters). Title BGM audible. Unit test from RE:
   AFC decode against a reference decode of a known .aw wave (the jingle test
   vector from the recomp era, docs/audio_data_formats.md recipe).
3. **Fidelity** — aux buses/effects (FxlineConfig delay lines), IIR/FIR filters,
   dolby, HardStream (attract-movie audio path), pacing against
   aurora_audio_queued_frames.
4. Delete the SB_DBG_AUDIO-era placeholders; ear-check + waveform A/B vs a
   Dolphin audio dump (tools/audio? use WAV verification rules —
   adjacent-sample delta energy, not RMS alone).

## References

- `scratch/audio_ref/native_jas_recomp_era.cpp` (recovered from git 5736439^:
  runtime/native_jas.cpp) — proven AFC decoder, IBNK/WSYS parsing, ADSR.
- `scratch/audio_ref/native_audio_engine.md` + `audio_data_formats.md` — the
  recomp-era engine docs (also recovered; formats + verified test vectors).
- Dolphin `Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp` — the same ucode's HLE,
  field-by-field reference for the VPB semantics (consult for layout/semantics;
  implement from the decomp + RE, keep provenance clean).
- `JASAiCtrl.cpp` (updateDac/vframeWork/mix* — already decompiled), `JASDSPBuf.cpp`
  (triple-buffer pipeline), `JASDSPChannel.cpp`, `JASDSPInterface.cpp`.

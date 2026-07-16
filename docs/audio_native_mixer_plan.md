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

## M2 input, mapped (2026-07-15 RE — the renderer's data source)

The decomp's now-native sequencer fills the voice state; M2 just renders it. Source of truth
= `JASystem::TDSPChannel::DSPCH[64]` (static in JASDSPChannel.cpp), 64 voices. Each
`DSPCH[i].unkC` is a `DSPInterface::DSPBuffer*` = the **Zelda-ucode VPB**
(JASDSPInterface.hpp; the decomp itself notes it matches Dolphin `UCodes/Zelda.cpp`).
Per-voice render params (set by DSPBuffer::setWaveInfo/setOscInfo/setPitch/setMixerVolume
in JASDSPInterface.cpp):
- `unk110` (s16*) = decoded/encoded wave base; `unk114` = length; `unk118` = loop/osc info.
- format decided in setWaveInfo (param_2): PCM16 / PCM8 / **AFC** (the common case) — decode
  with the PROVEN `afc_decode()` in scratch/audio_ref/native_jas_recomp_era.cpp:205
  (verified bit-exact vs ROM by ear; coefficients in docs/audio_data_formats.md).
- pitch = setPitch(u16) (fixed-point ratio) → fractional resample step.
- volume = the `Channel unk10[6]` bus entries (targetVolume/currentVolume ramps) + pan.
- `unk10A` = active/gate flag; `unk2` = the per-frame "needs work" bit (updateAll clears it).

So M2's DsyncFrame2(subframes, bufL, bufR): for each DSPCH[i] whose VPB is active, decode
its wave (AFC/PCM), advance a fractional sample cursor by the pitch step across the frame's
sample count, apply the volume ramp + 2-bus (L/R) pan, and accumulate into bufL/bufR. Reuse
the reference engine's afc_decode + resample math; drive it from the live VPB, NOT the
reference's own BMS sequencer (that's the decomp's job now, running natively). DSPCH is
static — expose it via a small extern accessor (or render inside the JAS namespace) rather
than duplicating state.

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

## M2 VPB field map — RE'd from the setters (2026-07-17)

Implementing DsyncFrame2 (the seam in sms-boot/runtime/jas_kernel_native.cpp:103-113, which
currently memsets bufL/bufR to silence) needs the exact DSPBuffer VPB semantics. RE'd from
JASDSPInterface.cpp setters (the fields the native sequencer fills each frame):

- **Pitch**: `unk4` (u16, clamped to 0x7fff) — `setPitch`. Fixed-point resample ratio; the
  reference engine's step = pitch / 0x?  (confirm scale vs native_jas_recomp_era resample).
- **Mixer buses**: `unk10[6]` = `Channel{ u16 id; u16 targetVolume; u16 currentVolume; u16 unkC }`.
  - `setMixerVolume(bus, vol, dpage)`: `targetVolume=vol`, ramp delta in `unkC` hi byte. Gated
    by `unk10A` (returns early if set — the lock flag).
  - `setMixerInitVolume` seeds current=target immediately (no ramp).
  - `id` = bus routing code from `setBusConnect`'s `connect_table[12]`
    ({0,0x0D00,0x0D60,0x0DC0,0x0E20,0x0E80,0x0EE0,0x0CA0,0x0F40,0x0FA0,0x0B00,0x09A0}) — maps
    logical bus → DSP output (L/R main + aux). For v1 (L/R only) use buses 0/1.
- **Wave / format**: `setWaveInfo(Wave_*, dataPtr)`:
  - `unk118 = (s16*)dataPtr` — encoded sample base.
  - format index = `Wave_.unk1`; `unk64 = COMP_BLOCKSAMPLES[fmt]` (AFC fmt0/1 = 16, PCM = 1),
    `unk100 = COMP_BLOCKBYTES[fmt]` ({9,5,8,16,1,1,1,1}). **fmt ≤ 1 → AFC, else PCM** — matches
    `native_jas_recomp_era.cpp:393` (`wv.fmt<=1 → afc_decode(hq = fmt==0)`).
  - loop: `unk102 = Wave_.unk10` (loop flag); if looping, `unk110 = Wave_.unk14` (loop base),
    `unk114 = Wave_.unk18` (loop length), `unk104/unk106 = Wave_.unk20/unk22` (loop
    predictor/history for AFC continuation). Non-loop: `unk114 = unk11C = Wave_.unk1C` (length).
  - `unkB0[16]` zeroed at setWaveInfo (AFC decode history/coef scratch).
- **Osc (synth)**: `setOscInfo` sets `unk118=0, unk64=16, unk100=param` — a generated waveform
  path, distinct from sampled. v1 can skip (rare); render silence + note.
- **Gate/pause**: `unk10A` (mixer lock), `unkC` (pause flag via setPauseFlag).
- **Filters** (M3, skip in v1): `unk120[8]` FIR, `unk148[4]` IIR, `unk108` filter mode,
  `unk150` dist.

### Not yet pinned (needed before coding v1) — next RE step
- The per-voice SAMPLE CURSOR / fractional position: the ucode maintains playback position +
  loop wrap in the VPB (likely `unk68`/`unk6C`, the ucode's writeback addr) and the
  predictor/history for AFC continuation across frames. Decide: read/write those VPB fields, OR
  keep a host-side Voice[64] state (as native_jas_recomp_era.cpp does) keyed by DSPCH index +
  re-trigger detection. Host-side state is cleaner (the ucode's exact writeback addr semantics
  are fiddly) — confirm the re-trigger signal (playStart/unk10A transition, or unk2 "needs work").
- `DSPCH[64]` access: `JASystem::TDSPChannel::DSPCH` is static in JASDSPChannel.cpp — add a
  small extern accessor (plan §M2) rather than duplicating.

### v1 scope (next iteration)
PCM16 + AFC (reuse `afc_decode` @ native_jas_recomp_era.cpp:205, proven), linear resample by
`unk4` step, L/R (buses 0/1) volume-ramp mix into bufL/bufR, host-side Voice[64] state. No
aux/filters/osc. Target: title BGM audible. Unit test: afc_decode vs the recomp-era test vector.

## M2 RE cont'd (2026-07-17): pitch scale nailed + voice liveness + decode confirmed

- **Resample step = `unk4 / 4096.0`** (unity pitch). RE'd: `JASChannel.cpp:898`
  `unk98 = 4096.0f * (unkA0 * (unk50 * unk8C))`, and `unk98` is exactly the value passed to
  `DSPBuffer::setPitch` (JASChannel.cpp:150/157/183 → `buf->setPitch(channel->unk98)`), which
  stores it in `unk4` (clamped 0x7fff). So step 1.0 = play at native rate; 8192 = +octave;
  max ≈ 8.0. This is THE resample ratio — do not guess it.
- **Voice liveness** (from `TDSPChannel::updateAll`, JASDSPChannel.cpp:247-278): render
  `DSPCH[i]` when `DSPCH[i].unk1 != 1` (allocated, not free) AND its VPB (`DSPCH[i].unkC`) has
  a wave (`unk118`/`unk110` set) AND not paused (`unkC == 0`). `unk10A` = mixer gate.
  `DSPCH` is `TDSPChannel::DSPCH` (static, JASDSPChannel.cpp:13), 64 entries.
- **Decode**: reuse `afc_decode` (native_jas_recomp_era.cpp:205, proven bit-exact) for fmt≤1
  (fmt0 hq=9B/16smp, fmt1=5B/16smp); PCM16/PCM8 for fmt≥2. Decode the WHOLE wave once to a
  cached s16 vector (reference approach), then resample from the cache with a fractional cursor
  — avoids per-frame AFC predictor-continuation bookkeeping.

### v1 is RE-ready to CODE (next iteration). Remaining v1 decisions (RE-grounded defaults):
- **Volume**: `unk10[bus].currentVolume` (u16, Q15 → gain = current/32768); ramp `currentVolume`
  toward `targetVolume` per frame (delta in `unkC` hi byte). v1 may step the ramp linearly.
- **Bus→L/R**: `unk10[6]` buses; v1 uses bus 0 = L, bus 1 = R (main stereo), skip aux/dolby.
  Exact routing (`unk10[].id` from setBusConnect's connect_table) + the ucode's mix math is the
  Zelda.cpp reference — defer exact aux/effect routing to M3, verify L/R vs a Dolphin audio dump.
- **Host state**: `struct HostVoice { s16* pcm-cache; u32 pcmLen; double cursor; u16 lastPitch;
  u32 genTag; float rampL, rampR; }` [64], keyed by DSPCH index; re-decode when the wave base
  (`unk118`) changes (re-trigger detection). Add `extern "C" DSPBuffer* sb_jas_dspch(int i)` +
  count accessor in JASDSPChannel.cpp rather than duplicating DSPCH state.
- Wire into `dsyncFrame2Native` (jas_kernel_native.cpp:103), replacing the memset: zero
  bufL/bufR, then for each live voice accumulate `cache[cursor] * gain` into L/R advancing
  cursor by step, wrapping at loop (`unk102` set → loop base `unk110`/len `unk114`) or stopping
  at end. Clamp mix to s16.

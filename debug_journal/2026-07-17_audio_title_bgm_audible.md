# Title BGM is AUDIBLE — two root causes: BE-swapped pitch + un-rebased ARAM wave pointer (2026-07-17)

**Result:** the title-screen BGM now produces real audio through the M2 native voice
renderer. `SB_AUDIO_RAW` capture (production path, `SB_DBG_AUDIO` off): peak **6162**,
rms **287**, DC ≈ 0, smooth waveform (adjacent-delta ~29 « peak), spectral peaks at bass
~70 Hz + harmonics ~2790 Hz — musical, not noise. No crash (runs full turbo window).
20 DSP voices go live, 7–9 playing concurrently. WAV: `scratch/wav/title_bgm_2026-07-17.wav`.

This closes the last blockers after the sequencer + SE-port-UAF work: renderer, pipeline
driving, LP64, IBNK/WSYS banks, sequencer, SE-crash were all already fixed; the two bugs
below were what kept the output silent (peak 0).

## Bug 1 — per-sound default PITCH read as a big-endian f32 without swapping → pitch 0 → silence

DSP pitch (`JASChannel::unk98 = 4096·unkA0·unk50·unk8C`) was always 0 because `unkA0` (the
track pitch multiplier) was 0. Trace, top-down:

- `unkA0 = unk4->mPitch` where `unk4` = the track's embedded `mChannelUpdater` (a `TChannelMgr`).
- `TTrack::updateTrack`/`updateTrackAll`: `curPitch = pitchToCent(...)` (always ≈1.0, never 0)
  then `curPitch *= mOuterParam->unk8` when outer switch 2 is on. **`outer->unk8` (outer pitch)
  was 0.** Volume worked because its analogous outer value was 1.0.
- `outer->unk8` ← `setParam(2, args->mTrackPitch)` ← `outerInit`: `args->mTrackPitch =
  param_1->unk10` (the `JAISeqUpdateData` seq-track pitch). **`sud->unk10` was 0** while
  `sud->unkC` (vol), `unk18` (pan), `unk20` (tempo) were all correct — so init ran; only pitch
  was wrong.
- `sud->unk10` is overwritten each frame by the pitch envelope
  (`JAIBasic::checkPlayingSeqTrack`, `r30 & 0x100000`): `pitch = ∏ seqParam->unk394[j].unk4`.
  `unk394[1].unk4` was **0** (init sets all to 1.0), so the whole product → 0. One zeroed
  per-route pitch multiplier kills the aggregate.
- `unk394[1]` = route-1 pitch, set via `setSeqInterPitch(1, target, 0)` ←
  `JAISound::setPitch(pitch, 0, 1)` from **`JAIBasic::setSeExtParameter`** (and
  `MSound.cpp:771`): `sound->setPitch(((JAISoundInfo*)…)->unk8, 0, 1)`.

**Root cause:** `JAISoundInfo` lives inline in the **big-endian AAF blob** (same as the
`getSoundSwBit` case — `unk0` is already `__builtin_bswap32`'d). `unk8` is the per-sound
default pitch, an **f32 stored BE**, read raw. A BE `1.0f` (`0x3F800000`) reinterpreted LE is
`0x0000803F` — a denormal ≈ 0. `initMoveParameter(target≈0, time=0)` writes `unk4 = 0`
immediately. Volume/fxmix (`unkC`/`unkD`) are single **bytes**, need no swap — which is
exactly why volume worked and pitch didn't.

**Fix:** byte-swap `unk8` on read at both call sites (`sb_soundinfo_pitch_be` in
`JAIBasic.cpp`; inline swap in `MSound.cpp`), guarded by `SMS_NATIVE_PLATFORM`.
Verified: `unkA0` 0 → 1.0, `unk98` 0 → 3235 (real note pitch).

## Bug 2 — M2 voice renderer dereferenced the wave's ARAM address as a host pointer → SIGSEGV

Fixing bug 1 un-gated the renderer mix loop (`step = unk4/4096 > 0` now), which immediately
SIGSEGV'd inside `DsyncFrame2`. `DSPBuffer::unk118` (the wave sample pointer) is set by
`setWaveInfo` to the **raw ARAM address** (`0x634c0`-class, ~0.4 MB — far too low for a host
pointer). On GC the DSP reads sample data straight out of ARAM; here ARAM is a host buffer
(`extern/aurora/lib/dolphin/AR.cpp` `sAramBuffer`, base = `ARGetStorageAddress()`), so the
ARAM address must be rebased to `sAramBuffer + addr` before dereference (`ARGetBaseAddress()`
is 0 on native, so it's a direct offset).

**Fix:** `sb_aram_to_host()` in `jas_kernel_native.cpp` rebases `unk118` in `sb_decode_voice`
only (the liveness/retrigger checks keep the raw value as an identity token). Verified:
voices decode real PCM, no crash, audible output.

## Also fixed en route — setSeqPortargs LP64 field scatter (JAISystemInterface.cpp)

`setSeqPortargsF32/U32` addressed `TPortArgs` as a flat 4-byte-word array from `&mTrack`
(`((f32*)&s->unk4)[param_3]`), correct only when `mTrack` is a 4-byte GC pointer. On the
64-bit host `mTrack` is 8 bytes, so every index ≥1 is off by one f32 slot and the
per-parameter pushes scatter into the wrong fields (pitch→volume, pan→pitch, …). Replaced the
array index with a named-field map (`sb_portarg_slot`, GC word index → field), guarded by
`SMS_NATIVE_PLATFORM`. This wasn't the silence cause (the outer pitch came from `outerInit`'s
direct assignment, bug 1) but was a real latent corruption of the seq-port fields.

## M2 v2 (same day) — wave length is SAMPLES not bytes (over-read fix) + wave looping

The v1 "mild aliasing" was a real correctness bug. `DSPBuffer::unk11C` (and the loop fields)
are in **SAMPLES**, not bytes. Proof: the instrument waves sit back-to-back in ARAM
(0x42520, 0x486c0, … gap 0x61A0 = 24992 B); with `len=44374` as bytes the wave would overrun
the next one, but as samples the AFC byte span `(44374/16)*9 = 24957` fits the gap exactly
(~30 B block padding). The v1 decode read `unk11C` *bytes* → ~1.78× over-read into the
adjacent wave → high-frequency garbage (loud-1s ZCR 5201).

Fix (`jas_kernel_native.cpp`): `sb_afc_decode` now takes a sample count, decodes
`ceil(nSamples/16)` blocks (AFC state carries across), trims to `nSamples`; PCM paths use
samples too. Loop fields (all sample units): `unk102` = loop flag (0xFFFF on every title
instrument), `unk110` = loop-start sample, `unk114` = loop-end sample. Mix loop wraps
`loopEnd → loopStart` while a looped voice plays (guarded: disable loop unless
`0 < loopStart < loopEnd ≤ total`).

Verified: loud-1s **ZCR 5201 → 1038** (aliasing gone), clean spectral peaks A#4 ~466 Hz +
D5 ~587 Hz, sustained notes hold (loud-region sustain 24/24 windows vs sparse before),
DC ≈ 0, no crash. WAV: `scratch/wav/title_bgm_looped_2026-07-17.wav`.

## Amplitude is FAITHFUL, not a bug (RE'd, do not "fix" it)

The conservative level (loud-1s peak ~1669/32767) is correct GC behavior — ruled out as a bug:

- Per-voice `Channel::targetVolume` = `vol[0..1] × Driver::getMixerLevel()`
  (`updateMixer`, JASChannel.cpp:368). `getMixerLevel()` = `MAX_MIXERLEVEL`, set by
  `setMixerLevel(inputGainDown, …)` to `channel_level × 16384`. SMS calls it with
  `inputGainDown = 0.5` (JAIBasic.cpp:77) → mixerLevel = **8192**. So a full-vol voice tops
  out at targetVolume 8192, a deliberate −6 dB of summing headroom.
- The DSP applies that volume **Q1.15** (÷32768): the SMS DSPBuffer is the Zelda ucode VPB
  (`Channel{id,targetVolume,currentVolume}` — comment cites Dolphin Zelda.cpp#683), and
  Dolphin's Zelda HLE applies voice volume in "1.15 fixed format" (`ApplyVolumeInPlace_1_15`,
  explicit comment "Each of these volumes is in 1.15 fixed format"; the ÷2¹⁵/2¹⁶ `shift_factor`
  is the *dolby* path only). The M2 renderer's `q15(v)=v/32768` matches exactly.

So `0.5 (input gain) × note volumes`, summed with headroom, is the intended quiet level.
Do NOT scale the output up — that's hand-tuning away faithful behavior.

## Notes / next

- Resampler is still linear-interp (fine at ZCR ~1kHz); revisit only if a specific artifact
  shows. SE/other-scene audio coverage not yet checked.
- Diagnostics used to localize this (trkAll/updTrk/pitchEnv/outerInit-ROOT dumps) were
  investigation scaffolding and removed; the persistent `SB_DBG_AUDIO` voice-state/frame/DECODE
  dumps stay.

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

## Notes / next

- M2 v1 resampler is linear-interp play-once (no loop yet); polyphonic mix ZCR is a bit high,
  likely mild aliasing — refine in M2 v2 (loop points via `unk110`/`unk102`, better resample).
- Diagnostics used to localize this (trkAll/updTrk/pitchEnv/outerInit-ROOT dumps) were
  investigation scaffolding and removed; the persistent `SB_DBG_AUDIO` voice-state/frame/DECODE
  dumps stay.

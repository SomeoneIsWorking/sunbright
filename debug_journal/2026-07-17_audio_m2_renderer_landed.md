# 2026-07-17 — Audio M2 voice renderer IMPLEMENTED; blocked on upstream (no DSP voices allocated)

Implemented the M2 DSP voice renderer (docs/audio_native_mixer_plan.md), replacing the
milestone-1 silence seam in `dsyncFrame2Native` (sms-boot/runtime/jas_kernel_native.cpp).

## What landed (RE-grounded, builds clean)
- `extern "C" sb_jas_dspch_vpb(int)` accessor in JASDSPChannel.cpp — exposes the static
  `TDSPChannel::DSPCH[64]`, returning the VPB (`DSPBuffer*`) for a live channel (`unk1 != 1`).
- `dsyncFrame2Native`: for each live voice, decode its wave to a cached s16 PCM buffer
  (proven `afc_decode` for AFC — VPB `unk64==16`, hq=`unk100==9`; PCM8/16 for `unk64==1`,
  `unk100`=bits), resample by `unk4/4096` (unity from JASChannel:898) with a fractional
  cursor + linear interp, apply Q15 L/R volume (`unk10[0]/[1].targetVolume`), 32-bit-headroom
  mix, clamp. Host-side `HostVoice[64]` state (PCM cache + cursor), re-decode on wave change.
  v1 limitations (documented, NOT hacks — correct partials): play-once (loop = next), no
  volume ramp (uses target level), L/R only (aux/effects = M3).
- `SB_AUDIO_RAW=<path>` in `onDacBuffer` — appends the interleaved s16 stereo DAC output to a
  raw PCM file, even headless (device push still skipped). Verification harness (plan §M2.4).
  Analyze: `ffmpeg -f s16le -ar 32000 -ac 2 -i <path> out.wav`, or numpy RMS/peak.
- `SB_DBG_AUDIO` per-voice diagnostic in the renderer (live/playing counts, per-voice
  wave/pitch/format/volume).

## BLOCKER found (upstream of the renderer): zero DSP voices allocated
Ran to the title (SB_NO_FASTBOOT) and to stage-15 (SB_STAGE=15), SB_DBG_AUDIO=1: the renderer
is active ("[audio] DsyncFrame2 native voice renderer active") but **max live voices = 0**
across the whole run — every DSPCH channel stays free (`unk1 == 1`). Captured DAC output is
pure silence (peak 0). The BGM sequence IS loaded (`[sms_boot_audio] loaded seq idx=0
len=58880 from /AudioRes/sequence.arc`), but no notes reach DSP voice allocation.

So the renderer can't be verified yet — there is nothing to render. The gap is in the
sequencer → channel-alloc → DSPCH path (the JAIBasic/MSound sequencer layer), NOT in M2.
Also noted: DsyncFrame2 is called far less often than updateDac (~29k) — only a handful of
calls — worth confirming the DSPBuf::process → DsyncFrame2 cadence is right.

## NEXT (sequencer-side, next iteration)
1. Is the title BGM actually STARTED? Trace MSound/JAIBasic title BGM trigger (is startBGM/
   seq-play called for the option scene, or is the attract silent by design?). Check a scene
   with known BGM if the title is silent.
2. Is the sequencer PROCESSING the loaded seq (note-on events)? Instrument the JASChannel /
   TDSPChannel::alloc path — are JASChannel voices active but not reaching DSPCH::alloc, or is
   the sequencer not running at all? (smnUse counter, alloc call count.)
3. Confirm DsyncFrame2 call cadence vs updateDac (why so few calls).
Once voices allocate, the renderer should produce audio and M2 v1 can be ear/waveform-verified.

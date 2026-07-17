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

## Update (same day): pipeline DEADLOCK + LP64 crash fixed — pipeline now runs; next gap = no wave on notes

Root-caused and fixed TWO bugs that made the "no DSP voices" symptom, both surfaced by
actually driving the renderer:

### 1. DSPBuf triple-buffer pipeline DEADLOCK (the "0 voices" root cause)
`DsyncFrame2` AND the sequencer advance (`Kernel::subframeCallback` via `DSPBuf::updateDSP`)
BOTH live in `process(DSPBUF_EVENTS_UNK1)` = `finishDSPFrame`. But M1 only drove
`mixDSP` = `process(UNK2)`, whose idle-kick calls `finishDSPFrame` ONLY `if (dspstatus == 0)`
— and `dspstatus` is set to 1 by the first `finishDSPFrame` and only reset inside
`finishDSPFrame`'s buffer-full branch (never reached). So the whole producer path (voice
render + sequencer tick) ran EXACTLY ONCE per boot, then deadlocked. Evidence: `startSeq`
fired 8×, `updateDac` 38285×, but `DsyncFrame2` logged only "call 1". M1's "self-driving
idle-kick" was never actually verified.
FIX (sms-boot/runtime/jas_kernel_native.cpp): drive `DSPBuf::finishDSPFrame()` explicitly per
DSP tick in `sb_jas_kernel_frame` (the native equivalent of retail's DSP-done interrupt). Now
`DsyncFrame2` runs continuously (32000+ calls/run), sequencer advances.

### 2. LP64 pointer truncation in the DSP-channel back-link (crash once pipeline ran)
Driving the pipeline exposed a SIGSEGV in `Driver::updatecallDSPChannel` (via
`TDSPChannel::updateAll`). `TDSPChannel::getLogicalChannel()` returns `(TChannel*)unk8`, but
`unk8` was declared `u32` — and the logical channel is passed in as `(u32)(uintptr_t)this`
(JASChannel.cpp:716/731/776), TRUNCATING the 64-bit host pointer. `getLogicalChannel` then
reconstructs a garbage pointer → `channel->unk4` faults. Benign on 32-bit GC; fatal on host.
FIX: widen the channel-pointer path `u32 -> uintptr_t` (`unk8`, `alloc(u32,uintptr_t)`,
`allocate(uintptr_t)`, `free(*,uintptr_t)`, and drop the `(u32)` truncation at the 3 call
sites). Also reordered `updatecallDSPChannel`'s `mgr = channel->unk4` to AFTER the null check
(was a null-deref-before-check, same host-crash class). uintptr_t == u32 on GC (faithful).
VERIFIED: crash gone, pipeline runs the full frame budget without core-dumping.

### NEXT gap: channels allocate but carry NO wave
With the pipeline running, the SB_DBG_AUDIO channel-state trace shows `alloc=1 wave=0` — ONE
DSP channel is allocated but its VPB `unk118` (wave base) is never set, and only one at a time
(not the many voices a BGM would use). So note-ons reach channel allocation but `setWaveInfo`
is not reached / the wave lookup returns nothing. Next: trace `TTrack::noteOn` ->
`BankMgr::noteOn` -> instrument/wave resolution — is the WAVE BANK (IBNK/WSYS / .aw archives)
loaded, and does noteOn find an instrument+wave? (alloc=1 is likely a held control/SE channel,
not a sounding note.) The renderer + pipeline are ready; the remaining gap is the
sequencer→bank→wave binding.

## Update 2: full silence chain traced to the FINAL blocker — IBNK not loaded (needs BE swap)

With the pipeline running (deadlock + LP64 fixes above), traced why channels still carry no
wave (`alloc=1 wave=0`). Instrumented BankMgr::noteOn:
- `noteOn` is called only ~5x, ALWAYS `bank=255 (0xFF, unassigned) inst=240 (0xF0)`. inst
  0xF0 > 0xEF routes to `noteOnOsc` (the procedural OSCILLATOR path, setOscInfo — no sampled
  wave). So the tracks start (20+ startSeq) but issue almost no real notes, and those default
  to osc with no instrument bank.
- Root: the INSTRUMENT BANKS (IBNK) are never loaded. JAIBasic.cpp:253-258 comment documents
  it: cid-3 WSYS wave banks ARE wired to unk54 + BE-swapped in registWaveBankWS, but cid-2
  IBNK instrument banks -> `unk50` is LEFT NULL — "registBankBNK needs its own IBNK BE swap
  before it can be wired (game null-guards unk50 -> silent instruments, no crash)". So
  BankMgr::getBank returns null for every real note -> no instrument -> no wave.

### The complete audio-silence chain (this session):
1. DSPBuf pipeline deadlock (producer ran once) -> FIXED (drive finishDSPFrame per tick).
2. LP64 TDSPChannel::unk8 pointer truncation -> SIGSEGV -> FIXED (u32->uintptr_t).
3. Sequences (Vload/sms_boot_audio) + WSYS wave banks load fine.
4. **IBNK instrument banks NOT loaded (unk50 null, pending IBNK BE swap)** <- FINAL blocker.

### NEXT (the remaining M2-audible task): implement the IBNK BE swap + wire cid-2 -> unk50
- IBNK (JASBNKParser / createBasicBank) is a big-endian instrument-bank format; on the LE host
  it needs a position-aware BE swap (like WSYS's in registWaveBankWS) before registBankBNK can
  parse it. Wire cid-2 blobs into unk50 in the aaf/cid loop (JAIBasic.cpp:259+), then the
  existing registBankBNK(i, unk50[i]) + assignWaveBank loop (JAIBasic.cpp:411-422) binds them.
- Once instruments load, noteOn resolves real waves -> setWaveInfo sets unk118 -> the (landed)
  renderer decodes+mixes -> title BGM audible. THEN also fix the latent LP64 truncation
  `chan->unk14 = (u32)(uintptr_t)wave` (TChannel::unk14 is u32; same class as unk8) which will
  bite the moment real wave pointers flow — widen TChannel::unk14 + setWaveInfo's param to
  uintptr_t and verify no truncation.
- Diagnostics kept (SB_DBG_AUDIO): noteOn CALLED/FAIL reasons, startSeq trace, per-frame
  alloc/wave/paused/live channel counts.

## Update 3: IBNK BE-swap + cid-2 wiring LANDED — banks now load (bank 255->0); note-issuance is the next gap

Implemented the IBNK instrument-bank BE swap + wiring (the blocker from Update 2):
- `BNKParser::sb_ibnk_swap_to_host` (JASBNKParser.cpp) — position-aware in-place BE swap of the
  whole IBNK, mirroring createBasicBank's traversal: header vir-number@0x08 + mInstOffsets[0x80]
  @0x24 + mPercOffsets[12]@0x3B4, then follow each into TInst (vol/pitch f32, osc/rand/sense/
  keymap offsets), TOsc (+ osc-envelope s16 triple tables via getOscTableEndPtr semantics),
  TRand/TSense/TKeymap/TVmap, and TPerc/TPmap (incl. u16 release table@0x308). Shared
  oscillators (findOscPtr dedups) are swapped at most once via a HostPtrSet — double-swap would
  undo them. The analogue of sb_wsys_swap_to_host.
- `registBankBNK` (JASBankMgr.cpp): call sb_ibnk_swap_to_host FIRST, before the vir-number read
  and createBasicBank.
- cid-2 wiring (JAIBasic.cpp): build the null-terminated `unk50` table (data ptr + wave-bank
  index) from the aaf cid-2 list, exactly like cid-3->unk54. Was previously skipped.

VERIFIED (partial): no crash over 604s (swap is structurally sound); `noteOn` now gets
`bank=0` instead of `bank=255` — the vir2phy mapping is populated (the @0x08 swap fixed the
setVir2PhyTable read), so instrument banks register and resolve. Real progress.

STILL SILENT — next gap: notes reaching noteOn are still `inst=0xF0` (osc path) and only ~5 of
them. So the main melodic note-ons (real programs 0-127) aren't being issued by the sequencer,
OR they fail in TTrack::noteOn before BankMgr::noteOn (my instrument only counts the latter).
NEXT: instrument TTrack::noteOn — count total note attempts + raw program/note; determine why
programs read 0xF0 (program-change not processed? track parse stalled? note data not reached?).
Also still pending once real waves flow: the latent LP64 `chan->unk14 = (u32)(uintptr_t)wave`
truncation (TChannel::unk14 u32 -> uintptr_t).

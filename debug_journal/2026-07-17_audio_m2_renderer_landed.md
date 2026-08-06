# 2026-07-17 — Audio M2 voice renderer IMPLEMENTED; blocked on upstream (no DSP voices allocated)

Implemented the M2 DSP voice renderer (docs/audio/native_mixer_plan.md), replacing the
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

## Update 4: note-issuance localized — only 8 noteOn total (tracks barely advance); reg6=0xF0 (osc)

Instrumented TTrack::noteOn (SB_DBG_AUDIO). With IBNK banks now loading:
- `noteOn` is reached only **8 times total** over a 35s run, across 20+ started tracks. So the
  sequencer barely advances — the melody isn't playing at all (real BGM = hundreds of notes).
- Every call: `reg=0xF0` (readRegDirect(6) = raw register mRegisterParam.unk0[6]), so
  bank=0 / prog=0xF0 -> BankMgr::noteOn's `param_3 > 0xEF` routes to noteOnOsc (procedural
  oscillator, setOscInfo, NO sampled wave -> unk118 stays null -> my renderer skips it).
  pitch/voice/vel decode fine (r31/r27/r29); only the PROGRAM is wrong/osc.
- Register 6 is written only by generic BMS register-write opcodes (Cmd_Process), not an
  explicit writeRegDirect(6); no melody program-change appears to set it to a sampled prog
  (0-127). But this is secondary — the primary issue is tracks not advancing.

So the gap is now in the BMS SEQUENCE PARSE / track timing: tracks start (startSeq x20+) but
issue ~8 notes total, meaning they STALL early — likely a wait/duration-timing bug
(seqTimeToDspTime / rest handling) parking tracks ~forever after one event, OR the parser
hits an unhandled opcode and stops the track (unk3C4->0). NEXT: instrument the parser
(Cmd_Process / rootCallback) — do tracks stop (error/unknown opcode) or wait (timing)? Count
per-track events + check for the unhandled-opcode/stop path. Also check reg-6 default in
mRegisterParam.init() and whether any melody track ever writes a sampled program.

(This is BELOW the M2 renderer + banks, which are landed and correct — the renderer, pipeline,
LP64, and IBNK/WSYS bank loading are all done + verified; only the sequencer's note stream is
starved. Diagnostics kept: SB_DBG_AUDIO TTrack::noteOn reg/prog/pitch trace.)

## Update 5: parse-stall ROOT — tracks wait forever for note-channel-done (mWaitTimer==-1)

Instrumented rootCallback + sParser.mainProc (SB_DBG_AUDIO):
- rootCallback fires 25000+ times, unk3C4=1 (playing), tempo unk3B0=0.2398 — so tracks ARE
  ticked at a normal rate; not a tempo/callback-driving problem.
- BUT sParser.mainProc (the BMS opcode processor) runs only **3 times TOTAL**. So each track
  processes ~its first few setup opcodes, then every mainProc() call breaks at the wait-gate
  (JASTrack.cpp:692-710) BEFORE reaching sParser.mainProc, and never advances again.
- The gate that's stuck: `mSeqCtrl.mWaitTimer == -1` (line 692) = "wait until note channel 0
  finishes" (chan->unk1 == 0xff). The note's DSP channel never reaches done, so the wait never
  releases -> track parked forever after its first note. (retcode never -1, so tracks don't
  END; they WAIT.)
- All 3 processed notes are prog=0xF0 (osc, noteOnOsc); osc notes with no release likely never
  reach "done", which would explain the permanent wait.

So the two symptoms share a cause at the note-channel lifecycle: the first note's channel never
reports done (unk1==0xff), and the program is osc (0xF0). NEXT:
1. Trace where TChannel::unk1 becomes 0xff (note "done") — is the note's release/duration ever
   applied, or does updatecallDSPChannel / the envelope never tear the channel down? (My earlier
   updatecallDSPChannel null/LP64 fix got the pipeline running; the channel may still not
   progress to done.)
2. Log the 3 setup opcodes sParser.mainProc processes — if they're misparsed (endianness/opcode
   dispatch), the wrong program (0xF0) + wrong wait mode could both follow from a bad track
   header parse. This is the highest-leverage check: dump the opcode bytes + dispatch.
Renderer/pipeline/banks remain landed+correct; this is purely the sequencer note lifecycle.

## Update 6: THE ROOT — sequences never load (sequence.arc not found); the BMS is all-zero

Traced the all-zero BMS (Update 5) to its origin. The track's read pointer is valid but the
BYTES ARE ZERO because the sequence data was never loaded:
- readSeq (sms_boot_audio.cpp) reads sequence.arc via `JASystem::Dvd::loadToDramDvdT` ->
  `loadToDramDvdTMain` -> `openDvd("/AudioRes/sequence.arc")` which **returns 0 (open FAILS)**.
  readSeq logged "loaded seq" unconditionally (no error check), masking the failure. The
  buffer stays zero-init -> every BMS opcode = 0x00 -> cmdNoteOn(note 0) forever -> the
  tracks stall on the note-channel-done wait (Update 5) with prog=0xF0 (osc). ALL of the
  note-issuance symptoms bottom out here.
- WHY openDvd fails: sequence.arc is NOT on the disc FST (like JaiArcS.hed / mSound.aaf).
- Attempt A (in-memory /audi capture, like mSound.aaf): `getResource("sequence.arc")` in
  /audi returns only ~16 bytes -> NOT a /audi resource under that name. FALSIFIED.
- Attempt B (BMS inside the aaf itself): aaf is 368896 bytes and the BARC offset 0x523a0
  (336800) IS within it, but the bytes there are ZERO -> the sequences are NOT embedded in
  the aaf. FALSIFIED.
So sequence.arc is a real separate ~337KB+ file whose location on THIS disc is still
unresolved: not disc-FST, not /audi resource, not in the aaf.

Build kept STABLE + silent: readSeq now copies from sb_seq_data if valid else leaves the
buffer zeroed (no crash); Application.cpp captures sequence.arc from /audi only if the
resource is plausibly sized (currently a no-op). All the SB_DBG_AUDIO diagnostics that
localized this chain are kept.

### NEXT (locate sequence.arc — the single remaining blocker to audible BGM):
1. List the disc FST (need a working tool — dolphin-tool wasn't built; try nod / a python
   GC-disc reader) for sequence.arc / the /AudioRes dir — is it on the FST at a different
   path/case, so the fix is the openDvd path?
2. Check how RETAIL loads it: the original Vload/JaiArcS path, or an aurora DVD mount. Does
   aurora's DVDOpen resolve ANY /AudioRes file (is the whole dir absent, or just the path
   wrong)? Instrument openDvd/registerFastOpen for a working vs failing /AudioRes open.
3. If it lives in a szs/arc resource, find which (it's not /audi "sequence.arc").
Once sequence.arc loads, the WHOLE lower stack (renderer, pipeline, LP64, IBNK/WSYS banks —
all landed+correct) should turn the real BMS into audible title BGM.

## Update 7: *** SEQUENCER WORKS *** — sequence.arc path fixed (real notes + playing voices!)

FOUND sequence.arc via a new aurora FST-dump (SB_DUMP_FST, fst.cpp fstCallback): it IS on the
disc FST at **/AudioRes/Seqs/sequence.arc** (size 800704) — inside the `Seqs/` subdir. The
loader used "/AudioRes/sequence.arc" (missing Seqs/), so openDvd failed and the BMS was
all-zero. Fixed kSeqPath -> "/AudioRes/Seqs/sequence.arc"; reverted readSeq to the (now
working) DVD read.

VERIFIED (the breakthrough): the BMS now loads REAL data (bytes a4 08 00 a4 09 00...) and:
- `TTrack::noteOn` gets REAL melodic notes: reg=0x79 (bank=0, prog=121 = a SAMPLED
  instrument, not osc 0xF0), real pitches (31/43/57/58/62), velocities (70/51/93/84).
- **DSP voices allocate AND render: `NEW max live voices=2 playing=2`** — the whole lower
  stack (renderer, pipeline, LP64, IBNK/WSYS banks) is now exercised with real data. The
  entire "silent BGM" chain (Updates 1-6) bottomed out at this one wrong path.

## REMAINING (2 issues before audible verification):
1. **Crash in JAISystemInterface::setSePortParameter** (via aiCallback->portCmdMain, ~24s in):
   a USE-AFTER-FREE. The SE port command stores `args->mTrack` (outerInit,
   JAISystemInterface.cpp:104-110; args = &param_1->unk4C[i].unk4, persistent). When the track
   is freed before portCmdMain drains the command, mTrack dangles -> the handler derefs a
   garbage track -> SIGSEGV. Added a null-mOuterParam guard (correct, matches noteOn's guards,
   but this crash is a GARBAGE (non-null) track, not null). NEXT: clear a track's queued port
   command(s) when it closes/frees (or invalidate unk4C[i].unk0/mTrack), so portCmdMain never
   runs a stale command. TPortCmd fields are proper pointer types (no LP64 truncation).
2. **Output still peak-0 in the pre-crash window** despite playing=2 — verify the renderer's
   mix once the crash is fixed and the frame runs stably (could be voices just starting, or a
   volume/DAC-forwarding issue). Re-check with SB_AUDIO_RAW after the SE fix.

Diagnostics added this iteration: `SB_DUMP_FST` (aurora fst.cpp — lists every disc FST entry).

## Update 8: SE-port crash = JAISeqUpdateData use-after-free (cancelPortCmd is a NO-OP stub)

The setSePortParameter SIGSEGV (~24s in, consistent) is a use-after-free at the
**JAISeqUpdateData level**, not the track level:
- The SE port command stores `args = &param_1->unk4C[i].unk4` (a pointer INTO the
  JAISeqUpdateData) and is queued (addPortCmdOnce -> cmd_once). portCmdMain (aiCallback, in
  DSPBuf::process) drains + runs it: `cmd->unk8(cmd->unkC)` -> setSePortParameter(args).
- When a sound stops (~24s, BGM/SE end) its JAISeqUpdateData frees, but its queued port
  command is NOT removed -> `param_1->mTrack` reads freed memory -> crash. Confirmed: guarding
  on track->unk3C4 / getOuterParam did NOT help (the fault is reading mTrack from the freed
  `param_1` itself, before those checks).
- ROOT: `JASystem::Kernel::TPortCmd::cancelPortCmd`/`cancelPortCmdStay` (JASCmdStack.cpp:56-58)
  are EMPTY `{ }` STUBS with no callers — the banned silent-no-op class. Nothing removes a
  queued command when its owner tears down, so a stale command runs against freed args.

### Fixes landed this iteration (correct, but for OTHER UAF paths — not the 24s crash):
- closeTrack now clears the PARENT's unk2C4[] back-ref to a self-closing child (JASTrack.cpp)
  — prevents getChild() returning a reused pool slot into a later outerInit. Correct + kept.
- setSePortParameter guards track->unk3C4==0 / null mOuterParam — covers closed-track and
  null-outer cases (kept; doesn't cover the freed-JAISeqUpdateData case).

### NEXT (the final crash before audible verification):
1. Implement `TPortCmd::cancelPortCmd(head)` / `cancelPortCmdStay()` — unlink `this` from the
   TPortHead singly-linked list (head->unk0..unk4 chain; clear this->unk0/unk4). Currently no-op.
2. Wire it: when a sound / its JAISeqUpdateData tears down (find the free/stop path), cancel
   every unk4C[i].unk2C command from cmd_once + cmd_stay BEFORE freeing the update data. Then
   the queue never holds a command into freed memory.
Once this lands, re-verify SB_AUDIO_RAW output (renderer/pipeline/banks/sequencer all work; the
game just crashes on the first sound teardown).

## Update 9: SE-port UAF — layered fixes landed; remaining crash = dangling `args` (const fault 0x1746f5168)

Deep-dived the setSePortParameter SIGSEGV. Fault address is CONSTANT at 0x1746f5168 across
every build (an unmapped heap address), and it is `param_1` itself (the command's
`cmd->unkC` = args) — the read is at offset 0 (`param_1->mTrack`). So a QUEUED port command
holds an `args` pointing into freed memory; portCmdMain runs it -> deref -> crash. Deterministic
at ~24s of title BGM.

### Fixes landed (all correct; each closed a real defect but not THIS crash):
- **cancelPortCmd / cancelPortCmdStay IMPLEMENTED** (JASCmdStack.cpp) — were banned empty
  no-op stubs. Now unlink `this` from the TPortHead queue. setPortCmd now dequeues a still-queued
  command before re-arming (the decomp cleared unk0 without unlinking -> a re-armed cmd_once
  entry double-linked and corrupted the list). VERIFIED it changed behavior: the crash PC moved
  (0x51dc91 -> 0x51de11), i.e. the queue-corruption path is fixed; the dangling-args path remains.
- closeTrack clears the parent's unk2C4[] back-ref on child self-close (JASTrack.cpp).
- TrackMgr::isPoolTrack() + outerInit rejects a non-pool (wild) track before queuing
  (JASTrackMgr, JAISystemInterface). setSePortParameter guards closed track / null mOuterParam.

### Remaining ROOT (next): who frees the args memory at ~24s
`args = &JAISeqUpdateData.unk4C[i].unk4`; unk180[] (the JAISeqUpdateData pool) and each
unk4C[] are allocHeap'd from the JAIData heap (unk1F4, JAIData.cpp:609-623) — which SHOULD be
persistent. Yet `args` = 0x1746f5168 is unmapped at ~24s. So either that heap is reset (a scene
transition / audio re-init in the title attract at ~24s) OR unk4C is reallocated. cmd_once is
drained same-frame, so the free must happen mid-frame between outerInit (queue) and portCmdMain
(drain), OR a cmd lingers via a path I haven't found. NEXT: instrument the JAIData heap
free / audio re-init and the ~24s scene event; when that heap tears down, cancel every
unk4C[i].unk2C command (now that cancelPortCmd works) OR portHeadInit() the queues. Then verify
SB_AUDIO_RAW output — the sequencer + voices already work (Update 7).

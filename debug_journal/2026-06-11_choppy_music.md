# Choppy JAS music under recomp (one-frame notes) — investigation state

## Symptom
THP audio fine; JAS music = brief note onsets ("first frames of sounds"). Oracle sustains.

## Established (measured)
- DSP frame cycle healthy after syncdsp_native fix (intr7 flows, audioproc thread alive).
- Diff harness: NO per-function recomp divergence (only benign OSEnableInterrupts r3).
- /vpb (CH_BUF 0x8040E5B8, 64×0x180 VPBs): oracle ~12+ concurrent voices at title; recomp 1-3.
  Voice volumes/params look sane while alive — voices just END early. Low ids reused ⇒
  channels freed quickly (notes die), not pool exhaustion.
- Note-event trace (SUNBRIGHT_DBG_NOTE, call_ppc ring): noteOn ~6/s (musical rate ✓);
  noteOff storm ×8 bursts = TTrack::allNoteOff from closeTrack (lr 8031c998).
  closeTrack callers: cmdCloseTrack (seq command, lr 803200bc) + mainProc track-end.
  openTrack ≈ closeTrack ≈ 2-4/s — CHILD TRACK CHURN. TrackMgr::allocNewRoot = 0 (no song
  restarts).
- TTrack tick phase/rate math healthy (rate .24/call, ~50 mainProc/s — normal tempo).
- Native ttrack tick NOT the cause (A/B with recompiled tick: same starvation; old spin gone).
- Root track (80625848) parked in the standard BMS idle loop (`80 14 / e7 00 14 / 88 ff ff /
  c8 ->0x22`) — INTENTIONAL; not a bug. Title song parts = child opens (c1 nn, offs 0x37d,
  0x194f, 0x3d67, 0xab6b, ...) reached via seq interrupts.

## Open question
Why do child tracks close after ~one phrase? Either (a) child stream hits cmdCloseTrack /
end -1 early (position/loop misread inside the CHILD streams), or (b) the section-driver
(seq interrupt timer, cmdIntTimer/e7 path) cycles sections too fast.

## Next
Compare child-track lifetimes oracle-vs-recomp by polling TrackMgr's regist table (find its
.bss address from registTrack 8031df48 disasm) — the probe works in BOTH builds (no call_ppc
needed). Then decode the child stream at its close position.

## Tools added this session
/vpb endpoint, SUNBRIGHT_DBG_NOTE call trace (note/track lifecycle + ms), SUNBRIGHT_TICK_LOG
(existing), VPB-from-RAM methodology (CH_BUF). Dolphin --wrap doesn't work for same-TU calls
(FetchVPB attempt — kept as documentation).

## Session 2 corrections (late night)
- DEAD END: TrackMgr regist-table-at-0x8040E038 parsing was wrong (ASCII junk) — discard those
  conclusions ("15 children", "11 concurrent tracks").
- DEAD END: stop/restart-loop hypothesis — flag=3 (TTrack::stopSeq) deaths were ONE-TIME
  legitimate stops of boot-era seqs, not a loop. No TrackMgr::allocNewRoot churn (0 restarts).
- Diff harness (incl. DIFF_ALL per-call) shows recomp functions CLEAN.
- The "frozen root" 80625848 = the normal BMS parked-root idle loop (80 14 / e7 / 88 ff ff /
  c8 jump-back) — NOT a bug.
- BGM track 8062ddb8 under recomp: frozen ~4.5s then giant catch-up bursts (oracle: ~188ms
  cadence) — its root stopped being ticked: Kernel::subframeCallback (80316e00) unregisters a
  per-seq tick callback when the tick returns -1; roots 8062ce78/80630008 got flag=3 (legit
  stops). The 4.5s-burst track belonged to a STOPPED seq — also explainable as legit.
- ARAM compare (new /aram endpoint): recomp missing ~300KB at 0xEB0000-0xEEFFFF (wScene bank
  region). Upload chunk map: streamer completed 20 chunks (wScene_1) ending 0xE9A680, never
  started the next bank. WaveBankMgr::loadWave (80310994) / MSound::loadWave (80015640) NEVER
  fire under recomp. Wave-id global (r13-24536 = 0x8040E1E8) = 0x212 (valid). The consumer
  func_802bb920 (real entry; 802bc10c is an OVERLAP-ORPHAN emitted copy — same bytes emitted in
  two functions, callers: func_802b76f4/802b77ec/802b77fc/802b7898) never called.
- ★ CAVEAT discovered at checkpoint: oracle autostart runs reach GAMEPLAY while recomp sits at
  the file-select card dialog → the ARAM diff may be scene-skew (gameplay loads more banks),
  not a loader bug. MUST scene-sync before trusting the ARAM hole. The user heard chopping at
  the TITLE (banks present) → the voice-level investigation at the title remains primary.

## Overlap-entry recompiler issue (separate, real)
Discovered entries emit the SAME code bytes inside multiple functions (func_802bc10c overlaps
func_802bb920; earlier 8031204c vs 80311f78 loop-head). Tracing/overrides on the orphan copy
see zero traffic. Worth a recompiler-level dedupe/containment pass eventually.

## Next concrete steps
1. Scene-synced compare AT THE TITLE (no A-spam past title): /aram + /vpb + audio RMS, both
   builds — settle whether title-era banks/voices differ at all.
2. If voices still die at title with banks equal: instrument TChannel ADSR/oscillator envelope
   per note (JASOscillator offsets from decomp) under recomp; compare against expected attack/
   sustain shape.
3. Tools added: /aram (FNV over ARAM), SUNBRIGHT_DBG_NOTE extended (wave-load + seq lifecycle),
  SUNBRIGHT_TICK_RATE (per-root tick accounting + -1 returns), SUNBRIGHT_NO_TTRACK_NATIVE.

## FRONTIER (end of session 2) — the gate that skips scene sound init
Per-frame scene state machine func_80299838 (runs 2887×, the TApplication/director boot-stage
step) gates on the SETUP THREAD 0x803FCBE8:
  1. OSIsThreadTerminated(803FCBE8) [80348374: state==8||state==0 → true] — PASSES (state=0).
  2. OSJoinThread(803FCBE8, &exitval) [80348d08]; then `if (exitval != 0) return 4` — TAKES THE
     ERROR PATH every frame (sound init 802b76f4 → MSound::loadWave(0x212) → wScene bank load
     NEVER runs ⇒ scene instruments missing ⇒ the audible chopping).
Hypotheses for exitval != 0 (ranked):
  a. state already 0 ⇒ thread was ALREADY JOINED once (or our detached-exit cleared it) ⇒ this
     join FAILS and never writes exitval ⇒ stack garbage at r1+344 ⇒ nonzero ⇒ error path.
     (Our native_os_thread_exit sets MORIBUND=8 for joinable; live state reads 0 ⇒ someone
     joined earlier, or double bookkeeping.)
  b. the setup body's r3 at return (used as exit value in guest_thread_body →
     native_os_thread_exit(cpu, thr, cpu.gpr[3])) is garbage/nonzero under the C-call model.
NEXT STEP (concrete): add exit-val + joiner logging: print exit_val in the body-exit log; trace
OSJoinThread (80348d08) callers/results for 803FCBE8 (who joins first, what value). Then fix:
either preserve the body's true return value, or make the join/exitval semantics faithful
(e.g. keep exitval retrievable after first join, per SDK). Note the SDK: OSJoinThread on an
already-reaped (state 0, detached) thread returns FALSE without writing exitval — the GAME
expects join-once; a premature join by our runtime is the likely culprit (grep native_os for
OSJoinThread handling / who could double-join).

## Session 2 final state (context limit)
- TRUE setup-thread exit values are ALL 0 (the earlier 0x803fcbe8 values were a logging bug —
  r3 captured AFTER bookkeeping; fixed in dolphin_hook). Stored T_VAL(+728)=0 ✓.
- OSIsThreadTerminated(803FCBE8) returns TRUE (state=0 path → r3=1, disasm-verified).
- ANOMALY to resolve first thing next session: OSJoinThread (80348d08) trace produced ZERO
  events while func_80299838 ran 2887× — either the join isn't reached at the title (the 2887
  count may predate the title; the state machine may have completed/halted differently), or
  the trace window missed it. NEXT: single run, trace 80299838 + 80348374 + 80348d08 +
  802b76f4 TOGETHER with ms timestamps, and dump the state machine's STAGE variable (the
  store after the 802b76f4 call: `r0=1` → find its target address in the emitted body of
  func_80299838 around // 802998bc-802998d0) to learn which stage the machine is stuck in at
  the title. Then root-cause that stage's gate.
- All tooling for this is in place (SUNBRIGHT_DBG_NOTE list in dolphin_hook.cpp — just extend
  the address list; /tracelog windowed; /r).

## Session 2 closing addendum (stage-flag finding)
- func_80299838 runs every frame at the title (lr=802A6170 = TApplication gameLoop dispatcher)
  but bails BEFORE the IsTerminated gate: stage flag obj+608 (obj=0x80902A40, the current
  director/app stage object) is ALREADY 1.
- Flag writers: ctor-ish func_80296df4 (initializes 588/589/592/604/608 with one value) and the
  success path in func_80299838 itself (after 802b76f4). +604 reads 0 while +608 reads 1 ⇒ the
  success path DID run once — EARLY (logo era, loading wScene_1 = the 20 observed chunks) —
  before the wave-id global became 0x212 (title/select id).
- WORKING THEORY: the scene-change re-init (new director / re-armed stage machine per scene)
  doesn't happen for the title/select scene under recomp — the wave id updates but no stage
  re-runs the load. Next: map the director lifecycle (who re-creates/re-arms the stage object
  at scene changes; compare flag+608 timeline against scene transitions; find the scene-change
  path that should reset it (or call loadSceneWave directly) and why it's skipped).
- Method note: trace windows are ring-limited — capture from seq 1 for boot-era events.

## Session 3 (early morning) — corrections + tooling lessons
- ★ FIVE EXECUTION CONTEXTS: call_ppc, bridge JIT-entry (Run), interpreter (interp_run_until),
  raw-JIT mid-function, and tail_ppc→recomp DIRECT DISPATCH. Tracers now cover call_ppc + bridge
  + interp + tail-to-recomp (jnote/inote tags). Raw-JIT mid-function remains invisible by nature
  — but zero non-recomp tails this session means raw-JIT exposure is currently nil.
- CORRECTION: the "wave id global = 0x212" was a MISREAD — the value at 0x8040E1E8 increments
  (0x20A→0x212 = +8): something unrelated aliases it via pointer writes. The wave-id setter
  family (802bb920 + 31 case copies) NEVER executes in any covered context.
- Stage flag (0x80902A40+608, word at 0x80902CA0): cycles 1→0→1 at ~24-26s (a real per-scene
  re-arm + completion: logo→title). NO further cycle at the title→select transition — the
  select screen's wave bank (wScene_10.aw in oracle FileMon) is loaded by a DIFFERENT path tied
  to the option/select screen load, not this stage machine.
- Tail histogram: ZERO non-recomp tail targets over a 200s run (de-JIT state is clean).
- New freeze observed once (watchdog 137, "Core state → Paused", pc=80002ff8) in tails2 run —
  possibly unrelated/intermittent; freeze dump at scratch/watchdog/freeze_20260611_033114.txt.

## NEXT (concrete)
1. Find the oracle's wScene_10.aw trigger: in pure Dolphin run with FileMonitor, the load happens
   with option.szs/scene/option.szs — find the game function that loads the OPTION scene's wave
   (likely the select/option screen init calling JAIBasic::loadSceneWave directly, or an MSound
   call from the boot director's menu-stage). Use the now-complete five-context tracers on
   the recomp side at the file-select arrival moment with autostart (the chain WILL show if it
   starts); what's missing is WHO should start it — compare with static callers of
   loadSceneWave (803017b0)/loadGroupWave (80301884) in generated code (grep call sites, then
   identify their containing REAL functions and trace those).
2. The select screen under recomp shows the CARD dialog (format) which may also gate its init
   differently than oracle (oracle had a formatted card from the start of its run).
   Consider pre-formatting the card image before comparisons (run once, let it format, keep
   the .raw) so both builds see identical select-screen flow.

## Session 3 verification (formatted card)
- With the now-persistent formatted card: ARAM hole PERSISTS (0xEC0000 region nonzero=22378 of
  256KB), voices still ~1 concurrent, RMS still choppy. The card alone doesn't fix it.
- ACTIVE LEAD: the menu/select wave load is save-flag-gated (reference/sms/src/System/
  MenuDir.cpp:174-176: TFlagManager getBool(0x30007) → gpMSound->loadWave(MS_WAVE_UNK128)).
  Decomp polarity may be imprecise; the lesson is the menu sound flow depends on save/flag
  state. Next: trace TFlagManager::getBool + MenuDir's load call (find their addresses via
  generated call sites of 80015640 inside the MenuDir region ~802e-802fxxxx?) at the select
  arrival with the five-context tracers; read flag 0x30007's backing storage.

## Session 3 final (04:05) — chain almost closed
WITH SUNBRIGHT_DBG_WAVE (direct-stderr tracer; ring polling DROPS sparse events under flood —
use stderr for low-rate chains): ALL THREE wave loads are REQUESTED:
  (0,0)=w1stLoad ✓ streams fully; (2,0x10)=wScene_10 loader obj 8066a040 REGISTERED at boot but
  its FIRST DVD READ never issues (zero chunks, zero DVD reads — the ARAM hole 0xEA-0xEE);
  (1,0)=wScene_1 ✓ streams fully at the scene change (the 0xE02680..0xE9A680 marcher).
Also CONFIRMED: the scene sound-init runs (loadWave 0x20A and 0x212 fire from lr=802BC278 — the
shared-tail call site; all earlier "never called" results were tracer blind spots).
- native_aram now defers ARQ callbacks ISR-style (ordering faithfulness; did NOT fix this bug
  but is more correct; keep).
- NEXT (precise): why does WaveArcLoader obj 8066a040's stream never start? Read
  JASystem WaveArcLoader source (reference/sms JASystem; loadWave lr=80310a0c context inside
  WaveBankMgr::loadWave 80310994) — find what kicks the FIRST chunk (its own thread? a DVD-T
  call? a finish-callback of the PREVIOUS file?). w1stLoad (file 1) completes → should kick
  queued file 2 — that handoff is the broken link (file 3 works because requested later, when
  the loader is idle). Suspect the loader's completion→dequeue-next path; trace its functions
  (neighbors of 80310694) with SUNBRIGHT_DBG_WAVE.

## Session 3 true-final (04:10) — the last link, explicit
WaveArcLoader::loadWave (decomp JASWaveArcLoader.cpp) = checkFileExtend + heap alloc +
**Dvd::loadToAramDvdT(0, "/Banks/wScene_10.aw", dest, 0, extent, flagPtr, 0)** → posts a TDvdCall
to the JAS DVD THREAD's message queue (JASDvdThread mq; OSSendMessage(&mq, cs, **blocking=1**)).
The JAS dvd thread (OSThread 8040AC00, prio 3 — earlier misidentified as "audio kernel thread")
sits parked on that queue's receive (T_QUEUE=803FD8D8) FOREVER — the posted message for file 2
is never serviced = the ARAM hole = silent select-scene instruments = THE CHOPPY MUSIC.
Files 1/3 work because their CALLERS spin-wait (`while(!done)`) → poll-yield pumps processing
(verify!), or their sends occurred while the thread was already awake.
**NEXT (first thing):** confirm with /r: the dvd-thread mq (base near 803FD8D0; queueReceive
+8=803FD8D8) usedCount ≥1 while thread parked. Then fix the wake: OSSendMessage→OSWakeupThread
on that queue under nthr from the BOOT-thread context (compare with the working audioproc mq
sends from ISR context). Possibly the same join/wake class as everything else tonight — or the
blocking-send path (flags=1) differs from noblock in the recompiled OSSendMessage and its sleep/
wake interplay needs the native seam. Fix → wScene_10 streams → hole fills → MUSIC.

## 04:15 correction — queue is EMPTY, not stuck
The dvd-thread mq (base 803FD8D8): usedCount=0 with the thread parked → the file-2 TDvdCall was
CONSUMED (or never enqueued), yet no DVD reads followed. JASDvdThread mechanism (decomp
JASDvdThread.cpp): callers copy a TDvdCall struct + fn ptr into a SHARED call-stack slot
(getCallStack()) and OSSendMessage(&mq, cs, 1). Back-to-back boot posts (file-1 chain + file 2)
may CLOBBER a slot before the thread runs it, or the thread dispatched a stale fn.
**NEXT SESSION:** read JASDvdThread.cpp fully (getCallStack rotation, dvdProc receive loop);
trace dvdProc's dispatches (which fn ptr + args it executes per message, SUNBRIGHT_DBG_WAVE
style); follow file 2's TDvdCall from post to dispatch. The fix likely = native port of the
JAS dvd thread proc or its call-stack handoff (own the path), per the established pattern.

## Session 4 (04:40) — the cleanest finding yet + forced stop
CORRECTIONS (keep the record honest):
- "Two roots frozen at 4406 ticks" = the boot jingles legitimately ENDING (deterministic stream
  length), NOT a bug. stopSeq never fires at the no-input title.
- The ARAM 0xEB-0xEE "hole" = scene skew (oracle-in-gameplay loads more banks). wScene_10 is a
  ~64KB file and STREAMS COMPLETELY (the "strays" at 0xEFA760 are its data).
- Clean no-input A/B (same boot, no input): oracle = full music from sec 2; recomp = logo bling
  then silence with rare blips. THE defect is real and scene-clean.
★ THE FINDING: at the recomp title, the BGM root (80639888, ticking at full rate, stream cur
  deep at 0x8071E50B — it executed its child-opening commands) has **ALL 16 CHILD SLOTS NULL**.
  The song's parts never attach ⇒ silence. Suspect: TrackMgr::getNewTrack (8031de2c) pool
  exhaustion/failure (leak from earlier churn, or the native ttrack tick finish() path not
  freeing children), or cmdOpenTrack failing under recomp.
  NEXT: (1) oracle A/B of the same root's children (blocked tonight by GPU exhaustion);
  (2) read TrackMgr's pool state under recomp (globals from getNewTrack disasm; count free);
  (3) trace cmdOpenTrack (80320084's flow) / getNewTrack return at the title.
FORCED STOP: the machine hit VK_ERROR_OUT_OF_DEVICE_MEMORY after ~50 runs tonight — Vulkan
device memory exhausted (also the cause of the repeated "oracle probe didn't come up"
failures late in the night). Reboot or GPU settle needed before further runs; treat late-night
oracle-side nulls with suspicion.

## 04:45 — pool exonerated; corrected frontier statement
TrackMgr pool at the recomp title: seqRemain=0x98/0xB9 free — HEALTHY. getNewTrack cannot be
the blocker. Root 80639888 (NULL children, sparse noteOns) is most plausibly the SE/system
root — the blips ARE its sound effects. SIMPLEST TRUE STATEMENT: under recomp the TITLE BGM
SEQUENCE IS NEVER STARTED (no root created for it; allocNewRoot fired 0× all night), while the
oracle starts and plays it. The divergence is the GAME's decision/path to start the title BGM
(JAIBasic startSeq / MSound BGM start — find the title-music start call: likely
MSound::startSoundBGM-ish from the title director; gate suspects: save/option state, the
intro-THP completion state, or a sound-handle status read).
NEXT SESSION (after GPU reset/reboot):
1. Find the BGM-start API (MSoundBGM.cpp in decomp; funcs for startSound* with BGM ids) and
   trace it five-context on both builds at the no-input title.
2. Walk its guard upstream to the diverging game state.
Tools/lessons all in place; pool/alloc/seq/DSP/ARAM layers ALL exonerated with evidence.

## 05:00 FINAL frontier (precise, evidence-backed)
Chain at the no-input recomp title (all confirmed firing via DBG_WAVE stderr):
  MSBgm::startBGM(0x80010010) ×2 → JAIBasic::startSoundActor → startSoundDirectID ✓
  checkSceneWaveOnMemory → returns 1 (wave gate OPEN) ✓
  …then NOTHING: no handleToSeq/allocNewRoot/registTrack/TTrack::startSeq, ever.
Decomp (JAIBasic::startSoundBasic, case 0x80000000): the request is stored into the seq request
buffer (unk0->unk1FC.storeBuffer) and unk38 (current-BGM handle) tracks it; the per-frame DRAIN
(JAIBasic::checkStartedSeq, called from processFrameWork line ~502) must turn it into a playing
seq — it never does. The SECOND startBGM is then DROPPED by the gate
(unk38 id low-10-bits match) — explaining the double call with no retry effect.
NEXT SESSION (one step): trace JAIBasic::checkStartedSeq (find addr via funcs/generated callers
of it) and its internals — find why the stored seq request never advances (its own gates:
sequence data load state, seq parameter pool getSeqParametermeterPointer 8030262c, or the
processFrameWork branch that calls it). Fix per the path. SE sounds prove the rest of JAI works.

## 05:05 ★★ ROOT CAUSE FOUND (gate-level, evidenced)
processFrameWork (80301c3c) gates the ENTIRE seq pipeline on three conditions; at the recomp
title GATE A FAILS: the state byte at *(JAIBasic+56)+1 (sub-obj 806ACC28, word 0x03040A00,
byte[1]) reads **4**, the pipeline requires **3**.
INTERPRETATION (osdsp task model): the JAS DSP TASK yielded to the THP movie's DSP task
(osdsp_task.c: yield mails 0xDCD10002/0xDCD10005, task states) and NEVER RESUMED after the
movie — "Audio Resumed" (0xDCD10001 handler) never printed in any log. Post-movie, JAI stays
in the yielded state (4) forever: the seq pipeline never runs → no BGM ever starts after the
FIRST THP → exactly matches every observation (logo bling works = pre-movie; SE work = ungated
path; oracle resumes fine).
This also retro-explains the user's organic no-music experience (real play passes through THPs
too) — and SUNBRIGHT_SKIP_THP makes it deterministic (the skip bypasses the player teardown,
but the legit path ALSO fails to resume under recomp).
NEXT SESSION:
1. Confirm: trace the osdsp task-switch mails (0xCDD10001/0xDCD10001/2/5) around the THP
   start/end (dspmr/dspmb rings or DBG-style stderr); find where the resume handshake dies
   under recomp (the __DSP_exec_task switch, DSP_prior_yield, or the THP task's done mail).
2. Fix per the path: likely a native port of the osdsp task-switch (we own DSP mail dispatch
   already) or fixing the resume mail delivery. Verify: state byte returns to 3 post-movie;
   clean A/B title RMS sustained.

## 05:15 corrected root cause (the osdsp-yield reading was wrong)
- DSP_prior_yield (0x8040E708) is ALREADY 1 (running) — osdsp task switch is FINE.
- *(JAIBasic+56) = **unk38 = the CURRENT BGM JAISound handle** (not a DSP state object!).
  Its state byte (sound->unk1 at handle+1) is stuck at **4** ("stopping/fading").
- THE REAL CHAIN: a boot-era BGM was legitimately stopped; its JAISound handle entered state 4;
  the STOP COMPLETION (handle release → unk38 cleared, state → free) never happens under recomp
  (the per-frame stop/fadeout processing — checkStoppedSeq/checkFadeoutSeq — never finalizes).
  With unk38 occupied, every new BGM request (the title music, id low-bits matching or the
  unk38!=null gate) is DROPPED at startSoundBasic. Hence: zero BGM forever after the first stop;
  SE unaffected.
- ALSO corrected: my "processFrameWork gate" reading was too hasty — the head iterates the
  sound list; do not trust that interpretation without re-reading the emitted flow.
NEXT SESSION (concrete):
1. Read JAISound state machine (decomp JAIBasic/JAISound: unk1 states; who advances 4→released;
   likely checkStoppedSeq → JAISystemInterface::checkSeqActive → when seq fully gone, release
   handle + clear unk38).
2. Trace checkStoppedSeq (find addr) + the handle state byte (806ACC29 via /trace) around the
  boot stop; find the stuck condition (probably waiting on a seq-done flag the recomp seq side
  never sets — note the seq tracks DID close).
3. Diagnostics: /w poke handle state or unk38=0 at title to confirm BGM then starts (poke =
  confirmation only; fix per the path).

## 05:40 poke results — the gate is real but clearing it isn't enough at the title
Confirmation pokes via the new `/w` endpoint (all at the recomp title, RMS tail = per-second
audio RMS from SUNBRIGHT_DUMP_AUDIO):
- handle state byte 806ACC29: 4 → wrote 3 → reads back **4** (something rewrites it every
  frame — the stop path is actively re-asserting "stopping", not a one-shot stale value).
- DSP_prior_yield 0x8040E708 already 1; rewriting it changes nothing (osdsp exonerated, again).
- **unk38 (805F3A10) cleared to 0 → NO music starts.** RMS tail stays blip-only.
  Consistent with: the title fires startBGM only twice, EARLY (lr=8016d824, id 0x80010010),
  and never retries. So a poke after boot can't confirm via audio; the gate test would need
  unk38 cleared BEFORE those two early attempts (or a forced re-trigger of startBGM).
- Negative results are still informative: the per-frame rewrite of state-4 means the stop
  processing IS running every frame and considers the stop incomplete — i.e. it's waiting on
  a completion condition (seq-done/fadeout-done flag) that never becomes true under recomp.
  That condition is the next thing to find (checkStoppedSeq / TJASCFader path).

NEXT SESSION (unchanged plan, sharpened):
1. Find checkStoppedSeq addr + the exact condition it polls (decomp JAIBasic.cpp); trace that
   flag/seq under recomp during the boot-era stop.
2. Root-cause why it never completes (likely the seq side never signals done — note seq tracks
   DID close); fix per the debugging path (recompiler defect → fix+test, else native port).
3. Verify: handle releases (state 4 → free, unk38=0) during boot; then the title's two early
   startBGM calls succeed; clean no-input A/B RMS shows sustained music.
4. Machine note: GPU was exhausted (VK_ERROR_OUT_OF_DEVICE_MEMORY after ~50 runs) — reboot
   before the next run batch; late-night oracle runs were unreliable because of this.

## Day session — silent-BGM mechanism fully mapped (root cause narrowed to the cue write)
Corrections to earlier readings (IMPORTANT):
- **JAISound state 4 = PLAYING normally** (3=started, 5=fading). The "stuck stopping handle"
  reading was WRONG: checkStartedSeq sets 3→4 when checkSeqActiveFlag!=0 (my state poke 4→3 got
  re-promoted to 4 — proof checkStartedSeq runs). checkStoppedSeq's flag==0 release is normal
  end-of-song cleanup. The engine believes the title BGM IS playing.
- checkStartedSeq/trackInit "zero calls" were INLINING artifacts: the shipped binary inlines
  them into the caller (writePortApp calls show lr INSIDE trackInit's body at 8030d954).

Mechanism (traced with new SUNBRIGHT_DBG_SEQ stderr tracer + sampled BMS-cursor dumps):
- The BGM track tree builds CORRECTLY under recomp: root 80625848 opens 2 mid-parents,
  each opens 16 children (76 cmdOpenTrack); tempo 120 set; all faithful to the BMS bytes
  (verified byte-by-byte against live memory at 80716240).
- The root conductor then enters wait 0xFFFF → jmp-back **infinite sleep BY DESIGN**
  (bytes `88 ff ff / c8 00 …`); root parser ticking 2×/100s is CORRECT behavior.
- Each child sits in a BMS poll loop: `cb 00 00` (readPort0 → reg, mirrored into reg3 by
  writeRegDirect) / `c8 03 …` (exit if ==1) / `80 01` wait / jmp back. The song starts when
  **port0 import == 1** (the game's section cue).
- Under recomp ports 0/1 of the children read **0x00FF**, rewritten every frame (clobbers pokes).
  Poking port0=1 in the gap freed child 806263b8 → it advanced to the next gate (mechanism
  CONFIRMED: the cue value is what's missing).
- Game APIs that write track ports (JAISound::setTrackPortData 8030b5e0 / setSeqPortData
  8030b330) — **zero calls under recomp**. setSeqPortargsU32/cmdChild/ParentWritePort — zero.
  writePortAppDirect writes only ports 0xe/0xf (value 0) from 80015ba8/bb8 (MSSeCallBack init).
- sCallBackFunc = 800158A8 = MSSeCallBack::setParameterSeqSync (SE water-filter etc.;
  cmd 0x0C → 0xFF traffic is healthy SE logic, unrelated).

OPEN: who writes the 0xFF to ports0/1 each frame, and who (in the oracle) writes the section
cue (1). Oracle memory comparison in progress — note track heap addresses DIFFER under
DISABLE_RECOMP; locate via TrackMgr handle table (ptr @0x8040E6C0, count @0x8040E6C8, SDA
r13-23296/-23288).

Infra gotchas today: /r probe reads under DISABLE_RECOMP initially looked dead (was the
GPU-failed runs, not the probe); VK_ERROR_OUT_OF_DEVICE_MEMORY recurred on oracle runs at
3D-scene entry while plain VRAM usage is only 3.3/12.8 GB — NOT global VRAM pressure;
recomp runs unaffected; investigating (suspect: descriptor-pool sizing under the
JIT-timing path; do NOT overlap two sunbright instances — also breaks probe port).

## ★★ ROOT CAUSE FOUND AND FIXED — music plays (2026-06-11 11:25)
**Root cause: TDSPChannel::updateAll's DSP-overload limiter.** updateAll (0x80314c60) measures
OSGetTick() deltas between the 8 DSP subframe syncs; when history[0]/delta < DSP_LIMIT_RATIO it
calls breakLowerActive(126) — force-stopping every voice below priority 126 (all music). On
hardware the syncs are evenly paced and this never trips; under the hybrid, Dolphin's DSP HLE
delivers sync mails INSTANTLY → tiny/erratic deltas → the limiter fired every frame, killing
every voice within a frame of starting. Hence: "single-frame samples", play=131/stop=131
symmetry, forceStop storm from lr=80314d48 (inside updateAll), THP/stream audio unaffected.
Same disease class as the audioproc intcount==0 suicide: HLE instantaneity vs HW pacing.

**Fix (own the behavior):** PC-native port `runtime/overrides/dsp_update_native.cpp` — faithful
64-voice loop + tick bookkeeping, overload limiter dropped (no DSP to overload on PC).
**Verified:** WAV RMS sustained 250–4000 for 280s across all attract cycles (was flat ~10 with
1-second blips). 

Exonerated along the way (kept as owned native ports + tools):
- Sequencer fully healthy: tree builds, ticks at oracle rate (1137 vs 1162/s), cursors track
  the oracle, durations parse right. The "wait-loop children" of the master se.bms ARE idle by
  design (oracle identical); the music seq is a separate root on the song BMS, restarted per
  attract cycle. cmdNoteOn ported native (cmdnoteon_native.cpp) — decisions sane.
- BankMgr::noteOn never fails (bank/wave lookups fine). TOscillator ported native
  (oscillator_native.cpp) — envelope math identical, not the killer.
- New tools: /jas (track-tree walk), /vpb wave-source fields, SUNBRIGHT_WATCH_WADDR write-watch
  (dladdr names the emitted writer), DBG_SEQ tracer + [noteon]/[sync] result probes.
- Gotchas: shipped binary INLINES checkStartedSeq/trackInit/cmdNoteOn call sites — entry-count
  traces undercount; oracle track addresses differ per run; oracle under Vulkan kept dying with
  VK_ERROR_OUT_OF_DEVICE_MEMORY at 3D-scene entry (VRAM NOT exhausted; OGL backend works —
  workaround SUNBRIGHT_BACKEND=OGL for oracle runs; cause still open); never overlap two
  sunbright instances (probe port + transient GPU pressure).

## Post-music sweep (same day)
- THP-transition NULL-read: NO LONGER REPRODUCES (200s + 420s autostart runs, zero
  wild/fatal/trap). Likely cured by one of the intervening native ports (audioproc/syncDSP/
  CARD/updateAll). Removed from the open list; reopen only with a fresh repro.
- [fiforeg] CPCtrl per-frame log was ungated (~200k lines/run) → now behind SUNBRIGHT_DBG_FIFOREG.
- Widescreen fades FIXED: TSMSFader::draw/drawFadeinout fill the caller's 4:3 TRect; under
  widescreen the 2D plane maps 0..640 to the center 4:3, so fades left the side thirds unfaded.
  Native wrapper (overrides/fader_widescreen.cpp) widens the rect x-extents by (w/6+1) per side
  around the recompiled call. VERIFIED via framedump: mid-fade frames dim edge-to-edge
  (was: bright pillars). Circular shine-wipe geometry not yet checked (needs a shine event /
  headed eyes).

## Gameplay verification sweep (afternoon)
- **Headless gameplay reached and driven**: autostart → menus → airstrip; /pad moves Mario;
  new probe endpoint **/shot?on=1|0** toggles burst frame-dump on demand (continuous
  SUNBRIGHT_DUMP falls behind the GPU on long runs → FIFO backs up → VI stalls → watchdog
  kill; two freeze reports were exactly this, not a guest bug).
- **3D-skin bug NO LONGER REPRODUCES**: Mario renders correctly on the airstrip (screenshot
  verified — cap/overalls/proportions all right). The "invisible Mario" earlier was him
  swimming behind the pier (user called it). Memory + index updated to RESOLVED.
- force_jit over the J3DModel block (802dddf0-802df844) froze boot — force_jit remains
  diagnostics-only and unreliable for visual bisection; not needed anyway.

## Save import + widescreen culling (late afternoon)
- **Dolphin save imported**: the user's Delfino-Plaza save lived in Dolphin's GCI-folder format
  (GC/USA/Card A/01-GMSE-…gci), not the .raw our native CARD serves. New tool
  `tools/gci_import.py` splices a .gci into the .raw (same-block-count entry replacement,
  both dir copies + checksums updated, .bak first). Import verified: entry+banner intact across
  multiple boots, card mounts clean, no format prompt. In-game load check left for a headed run
  (headless title→file-select menu timing is fiddly; attract loop kept bouncing my captures).
- **Widescreen 3D culling fixed**: game culls actors via SetViewFrustumClipCheckPerspective
  (802260cc) with the camera's 4:3 aspect while the GPU projection is widened → edge pop-in.
  Override scales the aspect ×4/3 (cull_widescreen.cpp), the game's own math does the rest.
  Smoke-tested (gameplay renders normally); edge-case proof needs headed eyes.
- **Open: intermittent boot/menu freeze class** — ~3-4 of ~15 runs today froze (watchdog dumps:
  one VIWaitForRetrace blocked, two DSP-mail waits at pc=80315f6c, one FlushGpu spin under
  DUMP backpressure which is explained). NOT deterministic; not tied to today's ports (600s runs
  with all ports survived). Needs its own session: collect the watchdog dumps, classify, RE the
  DSP-mail wait path. NEXT after that: screenspace effects under widescreen (TScreenTexture /
  TMirrorModelManager EFB-copy rects — survey started, addresses 8022d360 / 80192d60 region),
  then the 60fps model-interpolation project (docs/model_interpolation.md).

## Title-screen mystery SOLVED + save import VERIFIED IN-GAME (evening)
Why /pad "didn't work": THREE stacked causes, all fixed/understood:
1. **/pad combos were case-sensitive** — every `do=START`/`do=A` sent today parsed to 0 bits and
   was silently dropped (fixed: tolower; commit earlier).
2. **Stale-instance port theft** — a wedged old run held :17654; new run's probe logged
   "bind failed: Address already in use" and every command went to the zombie. RULE: after
   pkill, wait for process death before relaunching (and check the bind line in the log).
3. **Menu input timing** — the title needs Start HELD long (~1s+); 300ms presses fall through.
   3000ms hold worked instantly.
With those fixed, drove headless: title → file select (**imported save VISIBLE: file A ☀×01**)
→ jumped into block A → START → **DELFINO PLAZA loaded and playing** (Mario+FLUDD, Piantas,
lives ×3 from the save, graffiti portal rendering). gci_import.py chain fully verified.
File-select recipe (for the next session): hold start 3000 at title; left 900; a 400 (block A);
a 500 (START); ~20 s scene load.

## 60fps interpolation — stage 1 (capture) LANDED + user visual findings
- `overrides/interp_capture.cpp` (SUNBRIGHT_INTERP=1): hooks J3DModel::viewCalc 0x802deeb8,
  snapshots every model's joint world matrices (mNodeMatrices +0x58, jointNum modelData+0x1C)
  into a host double buffer keyed by J3DModel*. Verified live: 32 models/227 joints (title),
  124/631 (busy scene), ~640 snaps/s, pointer IDs stable, 5s expiry. The doc's "need a symbol
  map" blocker is obsolete — sms_gmse01_funcs.txt names everything. NEXT (stage 2): prev→cur
  slerp + draw replay with overwritten mNodeMatrices, present pacing between VI swaps
  (docs/model_interpolation.md §4-5).
- USER VISUAL FINDINGS (Plaza orbit shots): (1) the location-name banner BACKDROP (scene-entry
  "DELFINO PLAZA" pan-in) is not widescreen-accommodated — it's the known "backdrops must
  EXPAND to fill 16:9, not centre-squeeze" class (same fix shape as the fader: widen the fill
  rect; see docs/model_interpolation.md 2D-element classification). (2) the dock tower/column
  "splits at the waterline unnaturally" + flat gray open sea = the screenspace WATER surface
  effect (refraction/reflection EFB-copy) not rendering right in Plaza — top suspect list:
  TScreenTexture (8022d360 replace), TMirrorModelManager (80192d60), sea J3D material with
  indirect EFB texture. Both queued behind interp stage 2 / the DSP-mail dispatcher.

## Deadlock class IDENTIFIED (scene-entry freezes)
Watchdog dump 14:44: emu thread parked in nthr::block, blocking guest lr=802b36b8 (TCardManager
region) during Plaza scene entry — the scene-entry card access (autosave state read) blocks a
guest thread whose WAKE never arrives under native threading + native CARD. Matches the pattern:
today's BLOCKED-class freezes cluster at scene loads/transitions. Earlier DSP-mail-wait freezes
(pc=80315f6c vframeWork) are the second member of the lost-wake family. FIX DIRECTION (user-
approved "can't deadlock by construction"): single native dispatcher owning DSP mails; for the
card path, audit native_card completion → OSSendMessage/OSResumeThread wake delivery under nthr
(suspect: completion posted before the waiter parks → wake lost; needs a token/condvar handoff
like the GX PE-token fix). NEXT SESSION: this is the top item — it gates all interactive work.

## Deadlock analysis (full mechanism, evening session 2)
Latest freeze (14:49) decoded end to end:
- Main/render thread: nthrt_block_drain (VIWaitForRetrace frame barrier) — waits for all other
  Ready threads to reach block points.
- Card-delay thread: func_802b35cc = TIMED OSYieldThread loop (OSGetTick elapsed vs duration —
  the card "loading" minimum-display delay at scene entry). Needs CoreTiming/TB to advance.
- nthrt_yield_current runs idle_run (time advance) ONLY when ready_count()==0; yielding thread
  stays Ready otherwise.
- THE SMOKING GUN: watchdog dispatch counters all +0 (recomp/interp_steps/poll_yield) — NOBODY
  executes. Not a guest-logic wait: the nthr scheduler itself stopped dispatching. With threads
  parked in nthr::block and the idle handler not stepping (interp_steps +0), this is a
  HOST-LEVEL LOST WAKE in nthr's token handoff (cv notify lost / missed wakeup between block()
  release and the scheduler's pick loop), racing exactly at the block_drain + timed-yield +
  scene-load thread-churn window. The earlier vframeWork DSP-mail and FlushGpu-in-idle freezes
  are the same scheduler stall observed from different parked frames.
NEXT (first move of the fix): extend the watchdog freeze dump to print EVERY GuestThread's nthr
state (Ready/Blocked/DrainWait/seq), the token holder, and each host thread's backtrace — that
names the exact lost edge. Then redesign the handoff so it can't deadlock: single condvar +
generation counter, every state transition publishes under the lock, scheduler re-checks
readiness after every wait (no naked notify), timed-yield threads get a deadline the scheduler
owns (it advances CoreTiming itself when the only Ready work is a future deadline).

## Deadlock ROOT CAUSE (structural, confirmed by code read + dump correlation)
nthr's cv handoff is sound (notify under lock + predicate recheck). The hole:
grant_token_locked, when nothing is Ready, calls g_idle_handler() UNLOCKED — and the idle
driver can block UNBOUNDEDLY inside Dolphin: CoreTiming::Idle() → Fifo::FlushGpu() waits for
the GPU thread, which itself can be waiting for CPU-side PE-token dispatch (the drawsync
class) → AB-BA across our token loop and Dolphin's GPU sync. The whole scheduler hangs inside
the idle call: dispatch counters all +0, every guest thread parked in cv.wait — exactly the
watchdog dumps (one literally shows #7 FlushGpu #8 CoreTiming::Idle under nthrt_yield_current).
Clusters at scene loads (max FIFO + token traffic + thread churn).

FIX (can't-deadlock-by-construction):
1. The idle driver must never make an unbounded blocking Dolphin call. Replace CoreTiming
   Idle()/FlushGpu inside idle_run with BOUNDED slices: advance CoreTiming by fixed event
   quanta; between slices, PUMP the PE-token/drawsync dispatch (the existing
   g_ds_token_dispatch machinery) so the GPU's CPU-side dependency always progresses.
2. Add a watchdog-visible heartbeat in the idle driver (slice counter) so a future stall
   names itself.
3. Then the timed-yield (card delay 802b35cc) and vframeWork mail waits complete naturally —
   they only needed time to keep advancing.
Watchdog enhancement (do first): freeze dump prints every GuestThread {state, ready_seq,
os_thread, blocked-at lr} + token holder — one dump = full scheduler picture.

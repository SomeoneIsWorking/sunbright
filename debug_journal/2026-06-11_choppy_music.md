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

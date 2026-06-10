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

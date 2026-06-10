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

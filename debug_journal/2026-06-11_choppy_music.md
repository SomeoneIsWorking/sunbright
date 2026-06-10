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

# 2026-07-10 — SB_TRACE_SEQ unified frame trace: total order proves the phase-1 stale-flush is the structural divergence vs retail

## Instrument (permanent, env-gated)

`sb_trace_seq()` (`sms-boot/runtime/trace_seq.cpp`) — ONE process-wide atomic sequence
counter. With `SB_TRACE_SEQ=1`, four event families prefix every diagnostic line with
`seq=N`, giving an exact happens-before total order across families (retrace stamps alone
cannot interleave them — a projection write and the draws it covers can sit in different
retrace buckets, the "camera-1 write stamped 1432 / draws stamped 1434" conflict):

1. present boundary — `[trace] present-enter / aurora-end-frame / aurora-begin-frame /
   present-exit` (`sms-boot/runtime/frame_seam.cpp`)
2. perform dispatch — `[plist-order] seq=…` (`reference/sms/src/JSystem/JDrama/JDRViewObj.cpp`)
3. projections — `[proj-dbg] seq=…` + NEW `[posmtx0-dbg]` (every `GXLoadPosMtxImm` to
   `GX_PNMTX0`) (`extern/aurora/lib/dolphin/gx/GXTransform.cpp`)
4. drawbuf flushes — `[dbhead] seq=…` (`reference/sms/src/JSystem/J3D/J3DGraphBase/J3DDrawBuffer.cpp`)

New gates added for paced-run feasibility (unthresholded flood dominates wall clock: 75 s
paced only reached retrace 788): `SB_DBHEAD_DBG_AFTER=<retrace>` (drawbuf), and
`[posmtx0-dbg]` reuses `SB_PROJ_DBG_AFTER` as its threshold. Extraction script:
`scratch/logs/extract_window.py`; window: `scratch/logs/frametrace_window.txt`.

Capture: `SB_HEADLESS=1 SB_STAGE=15 SB_SCENARIO=0 SB_PROJ_DBG=1
SB_PLIST_ORDER_DBG_AFTER=1985 SB_DBHEAD_DBG_AFTER=1985 SB_TRACE_SEQ=1`, paced, 80 s.

## The unified frame (present interval retrace 2000, seq 79385→80188)

```
seq 79385  present-exit                                  <- frame begins
seq 79386  "Draw Buffer Group" (unk40, phase 1) flags=0x8
seq 79389  [dbhead ph1] Sky Xlu   6 pkts   \
seq 79391  [dbhead ph1] MapOpa    7 pkts    |  ALL under the PREVIOUS frame's
seq 79393  [dbhead ph1] MapXlu    2 pkts    |  final fader ORTHO (seq 79381,
seq 79404  [dbhead ph1] MirrorOpa 14 pkts   |  diag [0.004464,-0.003125,-.5,-.5])
seq 79406  [dbhead ph1] MirrorXlu 2 pkts    |  — NO projection write of this
seq 79414  [dbhead ph1] LensFlare 11 pkts   |  frame has happened yet
seq 79424  [dbhead ph1] LightOpa  6 pkts   /
seq 79428  "鏡カメラ" (mirror camera) flags=0x10
seq 79429  [proj] P mirror  diag=[1.5236, 2.0503]        <- FIRST proj this frame
seq 79459  [dbhead ph4] MirrorOpa 14 pkts  } last writer = mirror P (79429)
seq 79461  [dbhead ph4] MirrorXlu 2 pkts   }
seq 79469  "camera 1" flags=0x10
seq 79470  [proj] P world   diag=[2.0416, 2.7475]
seq 79481  [dbhead ph4] Sky Xlu   6 pkts   \
seq 79485  [dbhead ph4] MapOpa    7 pkts    | last writer = camera-1 P (79470)
seq 79496  [dbhead ph4] LightOpa  6 pkts    |
seq 79569  [dbhead ph6] MapXlu    2 pkts   /
seq 79604  [proj] P world (camera 1, screen-post pass)
seq 79682  [proj] O  (Screen 2D <TOrthoProj>)
seq 79686  [proj] O  (後処理 <TOrthoProj>)
seq 79693  [dbhead ph6] LensFlare 2 pkts     last writer = ORTHO 79686 (sun-glow; retail matches: O@22288 then draws)
seq 79695  [proj] P world (camera 1)
seq 79701  [dbhead ph6] LensFlare 11 pkts    last writer = camera-1 P (79695)
seq 79712/79719/79760  [proj] O  (2D UI <TOrthoProj> tail)
seq 79763+ 0x3001 movement/calc pass; 80078+ 0x480 frameInit+setDrawBuffer collections
seq 80076/80169/80176  [proj] P world (camera-1 re-primes between collections)
seq 80184  [proj] O fader  diag=[0.004464,-0.003125,-.5,-.5]   <- LAST event before present
seq 80185  present-enter → 80186 aurora-end-frame (fifo drain+render) → 80188 present-exit
```

Identical structure in the two adjacent frames (79382-ff and 80188-ff of the same window).

## Answers

- **Last projection writer per 3D flush:** phase-1 flushes (Sky/Map/MapXlu/Mirror/LensFlare/
  Light) → previous frame's FADER ORTHO. Phase-4 Mirror → mirror P. Phase-4 Sky/MapOpa/Light
  and phase-6 MapXlu → camera-1 world P. Phase-6 LensFlare first flush (2 pkts) → 2D ortho
  (faithful; retail shows the same O-then-draw at 22288), second (11 pkts) → camera-1 P.
- **Straddle verdict: YES.** The projection state covering the phase-1 flush is written
  before present N (fader ortho, last command of the previous interval); the flush's draws
  are emitted after present N. State and draws are in different Aurora frames/fifos; the
  carry-over rides Aurora's persistent `__gx` state.
- **First structural difference vs retail**
  (`scratch/oracle/fifo/title_press_start_vi_stable_gxseq.txt`): retail's frame is
  `[mirror P (seq 8, diag 1.5236/2.0503 — same values) → first draw 1081 → world P 4311 →
  draws → 2D orthos → tail P,P,O(26652)]` — **no draw exists before the first projection,
  and every buffer is drawn ONCE**. Native emits an EXTRA full flush of all 7 non-empty
  draw buffers (48 packets) at the head of every present interval, before any projection
  write, under the carried-over fader ortho — then draws the SAME buffers with the SAME
  packet counts again in phases 4/6 (where retail's actual draws are). The phase-4/6
  emission and the frame tail match retail structurally; the phase-1 stale duplicate is the
  sole structural extra.
- **Implied fix location: drawbuf flush timing/content — NOT frame-seam placement, NOT
  dispatch order.** Seam placement is equivalent to retail's frame split (both boundaries
  sit between the tail fader ortho and the mirror-camera perspective; retail's head has no
  draws). Dispatch order matches the decomp/retail (mirror → world → 2D). What differs is
  that on GC the phase-1 `unk40` flush finds EMPTY buffers while natively they still hold
  the frame's packets — the buffer clear/repopulate lifecycle (frameInit 0x480 vs entry
  timing) diverges natively. Next step: find why `J3DDrawBuffer::frameInit`'s clear does
  not leave the buffers empty at the next phase-1 (who repopulates between seq 80078's
  frameInit and 80192's phase-1 flush natively, and why GC's equivalent window stays empty).

This also supersedes the "carry-over is the root cause" framing of
`2026-07-10_projection_carryover_root_cause.md` §4: the carry-over is real but only matters
because the phase-1 flush should have nothing to draw. Retail carries the same tail-ortho
state across its frame boundary — harmlessly, because nothing draws before seq-8's
perspective.

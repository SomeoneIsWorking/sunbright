# 2026-07-14 — Ghost pass RE: dispatch fully verified faithful; GC's ph1 emptiness UNRESOLVED

Goal: RE why native's frame head re-draws every world DrawBuf under stale ortho (the
"phase-1 ghost", `SB_SKIP_GHOST` probe) when retail's stream has no such pass. Result:
**every code link in the dispatch chain is now byte-verified faithful against the US DOL**
— the divergence is runtime buffer STATE (GC's buffers must be empty at ph1; native's are
full), and the emptying mechanism on GC remains UNIDENTIFIED. A CPU-side Dolphin
instrument is required (see "Next instrument" below). Recorded here so no future session
re-walks the falsified ground.

## Renames landed (was: unk34/38/3C/40 in TMarDirector)

- `unk34` → `mPerformListPreEntry` — built by `TMarDirector::preEntry()`: per-frame
  clear+bind (0x480) of every world DrawBuf + actor-group entry (0x204) + camera (0x10).
  Performed with mask -1 at the END of each `direct()` call (the ENTRY branch), right
  BEFORE the present seam (verified: [trace] present sits between the fills and ph1).
- `unk38` → `mPerformListGraffito`, `unk3C` → `mPerformListPollution` — the two
  graffiti/pollution EFB-capture lists from `initECTGft` (both EMPTY at title).
- `unk40` → `mPerformListDrawBufGroup` — holds ONE entry: (drawBufferGroup, filter 8).
  drawBufferGroup = the 34 named DrawBufObjs + 8 TLightDrawBuffer bufs.
  (`TGCConsole2::unk34[19]` in MarDirectorDirect.cpp is a DIFFERENT class's field —
  left as unk34.)

## Verified-faithful (US DOL disasm, byte-level — do NOT re-check these)

- `TMarDirector::direct` render branch @0x80299bf4: performs +0x40 (ph1 group), +0x38,
  +0x3C, +0x1C (GX), conditional +0x20 (Silhouette), +0x24 (GXPost) — ALL with mask -1
  (`li r4,-1`). ENTRY branch performs +0x34 with -1 too. Loop order per call:
  RENDER first (if armed) → movement×N → ENTRY → break.
- `TDrawBufObj::perform` @0x802f830c: bit 0x80→frameInit, 0x400→j3dSys binds (unk18&3 /
  &4), 0x8→j3dSys.unk4C=unk18 + draw(). Exact decomp match.
- `J3DDrawBuffer::drawHead` @0x802efb08: pure walk of mBuffer chains calling packet
  vtable draw. NO consume-on-draw. `J3DMatPacket::draw` (~0x802edc30): checkThing
  (shape unk30 enables) → j3dSys tex/packet → matDL load → shape draws. NO disable.
- `TPerformList` load/dispatch, `TViewObj::testPerform` mask semantics, `MActor::perform`
  (0x2 calcAnm / 0x4 viewCalc / 0x200 entry): faithful.
- Nobody sets `unkC` (TViewObj disable mask) on the group or the DrawBufObjs; unkC is
  NOT loaded from disc (TNameRef::load reads name only).
- No shape-packet enable (unk30) is toggled per frame anywhere in the decomp; set to 1
  in ctor only. `drawClear()` only nulls link pointers, on ENTRY.

## Retail stream ground truth (title_settled.dff, post-merge draw indices)

Full frame = **195 draws**: mirror pass #0-72 (vp 256², P, prj[1.52 2.05] = the 52°-ish
mirror camera; content = a distinct object cluster at world z≈17250, NOT the sky scene)
→ world pass #73-146 (vp 640x448, P, prj[2.04 2.75]; sun/glare, 202v sky dome, cloud
letters at (305.4,-1043.4,-353.4), seagulls #132-143) → 2D tail #147-194 (ortho glyph
quads + a few P draws = GXPost ChrOpa + screens). NO frame-head whole-scene pass.
(SB_DRAW_DUMP's 200-draw cap in the non-windowed path hid this; fixed in aurora —
SB_DRAW_DUMP_FRAME now uncaps both windowed and non-windowed modes.)

## Native lifecycle ground truth (SB_TRACE_SEQ + SB_DBHEAD_DBG_AFTER + [dbclear]/[dbfill-first])

Per settled frame (~121 seq units): ph1 flush (Sky Xlu 6 pkts, MapOpa 7, MapXlu 2,
Mirror 14+2, LensFlare 11, LightOpa 6 — THE GHOST, stale ortho) → Mirror bufs cleared+
refilled (GX-list 0x480 + 鏡シーン 0x206) → ph4 flushes (Mirror, Sky, MapOpa, Light —
correct cameras) → ph6 (MapXlu, LensFlare refill+flush, 2D) → ENTRY pass: [dbclear] of
every world buf + refills (Sky via TSky::perform→MActor::entry, backtraced with new
`SB_DBFILL_BT=<name substring>`) → present → next ph1.
So natively the world bufs are FULL at ph1 (filled by the pre-present ENTRY), and ph1's
draw-only dispatch (mask -1 & filter 8 = 8) flushes them under whatever GX state the
2D tail left = the ghost. The SAME dispatch on GC emits NOTHING.

## Falsified hypotheses (do not re-chase)

- Ghost = extra/wrong dispatch in direct() — NO: dispatch masks byte-identical to US.
- drawHead/MatPacket consume-on-draw on GC — NO: pure walks, byte-verified.
- Light-manager forwarding of 0x80 clears to the group — NO: light perform only touches
  its own mLightSets; addChildGroupObj only INSERTS light bufs into the group.
- Whole-set 0x80 clears at the end of GX/GXPost lists — NO: only AfterIndirect gets 0x80.
- Mirror-scene packet migration (intrusive next-links moving world packets into Mirror
  bufs on GC, emptying Sky/Map) — UNSUPPORTED: retail mirror segment is a different
  model set, not the sky scene re-entered.
- unkC disable mask set on the group at title — no setter exists.
- retail world segment = the ph1 flush under the ENTRY-set world camera — NO: capture
  order is mirror THEN world; ph1 dispatches BEFORE the GX list's mirror section.
- Movement-list `&= 0x200` alternation as an entry source — movement filters (0x3001)
  never pass 0x200.

## Open question (the actual remaining mystery)

Given identical code and identical dispatch, why are GC's world DrawBufs EMPTY at ph1?
Something between the GX-list draws and the next ph1 empties them on GC but not natively
— OR the ENTRY pass on GC does not fill them (but then the GX-list Sky/Map 0x8 draws
would emit nothing, and the dome/letters ARE in the retail world segment). Both horns
contradict verified code. The resolution requires OBSERVING GC-side buffer state.

## Next instrument (required before further theorizing)

CPU-side Dolphin oracle: break at `TDrawBufObj::perform` (0x802f830c) and/or
`J3DDrawBuffer::drawHead` (0x802efb08) at the settled title; log r4 (flags) and walk
r3's mBuffer to count packets per call. That directly answers "empty vs full at ph1"
and, if empty, WHEN they got emptied (watchpoint on a mBuffer slot). Dolphin 2503 has
no headless scripting; options: GDB-stub (Dolphin supports remote GDB) driven by a
script, or a temporary Dolphin-side patch. Budget this as its own arc.

## Impact assessment (why this can pause)

At settle the ghost is bit-identical to skip (proven earlier); the fly-in blocks were
the mBlack decomp bug (fixed separately). Remaining cost: double-draw perf + risk of
visible garbage in transitional frames + divergence from retail stream shape (matters
for FIFO-level parity). Not currently pixel-blocking for the settled-title oracle gate.

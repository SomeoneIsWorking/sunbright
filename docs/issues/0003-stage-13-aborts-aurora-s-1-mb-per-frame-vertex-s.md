---
id: 3
title: Stage 13 aborts: aurora's 1 MB per-frame vertex staging buffer overflows
status: resolved
symptom: SBR_STAGE=13 dies mid-frame with [aurora FATAL gfx] mapped ByteBuffer overflow: have 1048566 bytes (capacity 1048576), need 48 more, at drawIdx 1627
tags: recomp,aurora,render,stage13
created: 2026-08-11
updated: 2026-08-11
---

Found by a 15-stage sweep run after the arena fix (issue #1). Every other stage tried — 1..7, 8, 9,
10, 11, 12, 14, 21, 22, 23, 24 — boots and runs 150 presents clean; stage 13 is the only failure, and
it is not the arena bug: the abort is in aurora's graphics layer, not the guest.

    [aurora FATAL gfx] mapped ByteBuffer overflow: have 1048566 bytes (capacity 1048576),
      need 48 more -> 1048614 total.
      last draw: prim=0x98 fmt=0 verts=26 idx=72 vertBytes=182 drawIdx=1627 <- OVERFLOWED

So the scene fills a 1 MB per-frame vertex staging region and asks for 48 bytes more, on a frame with
at least 1,627 drawables. The panic itself is correct behaviour (fail fast, not a silent truncation).

Not yet known, and worth measuring before resizing anything: whether 1 MB is simply too small for
this stage's real geometry, or whether something is uploading the same data repeatedly. The draw
sizes in the tail are ordinary (3-26 verts each), which does not distinguish the two — 1,627 small
draws is a plausible scene AND a plausible duplication. Growing the buffer without answering that
would be the classic "make the symptom go away" fix.

Repro: SB_HEADLESS=1 SBR_MUTE=1 SBR_FASTBOOT=1 SBR_STAGE=13 SBR_QUIT_AFTER=150 ./run-recomp.sh
Log kept at scratch/logs/sweep_13.log.

### Resolution (2026-08-11)
Not a runaway — the index staging region was simply undersized, and by 5%.

Measured with the cap temporarily raised to 16 MB: stage 13 uses 1,101,044 bytes of indices per
frame against a 1,048,576-byte cap, and uses exactly that every frame, stable to the byte over 60
frames (verts 1,696,356 of 3 MB; uniforms 3.5 MB of 24 MB). A runaway would have climbed. Stage 12,
for contrast, uses 36,960 bytes — stage 13's first full-scene frame draws 1,585 drawables where
stage 12 draws 196.

Fixed in aurora (fcf54a3): IndexBufferSize is now derived from VertexBufferSize rather than picked
independently. GX indices are Uint16, a GX vertex with indexed attributes costs ~7 bytes, and
fans/strips expand to ~2.3 indices per vertex, so filling the 3 MB vertex buffer needs ~3 MB of
indices — three times the 1 MB reserved. Now 4 MB.

The overflow message named no region: it printed a bare capacity, and working out which of
verts/uniforms/indices/storage/textureUpload owned 1048576 meant grepping the constants. It now
names the region (verified by shrinking the index region to 256 KB and confirming it reads "the
index staging region"), reports AMBIGUOUS if two regions ever share a capacity, and says so if a
capacity matches none.

Pinna Park now renders — Mario, NPCs, the park, the HUD (scratch/screenshots/stage13.png).

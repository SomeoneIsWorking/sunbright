---
id: 3
title: Stage 13 aborts: aurora's 1 MB per-frame vertex staging buffer overflows
status: open
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

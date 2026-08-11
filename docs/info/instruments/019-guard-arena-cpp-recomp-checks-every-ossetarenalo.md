---
id: I019
kind: instrument
status: trusted
created: 2026-08-11
---

## Instrument

guard_arena.cpp (recomp) — checks every OSSetArenaLo against the caller's own guest stack pointer, so a heap published over a live stack aborts at boot instead of surfacing as corruption in one stage months later. Blind to any other route by which a heap could land on a stack (a JKRHeap created at a hand-picked address, a host-side allocation) and to a stack created inside an already-published arena; it sees OSSetArenaLo only.

## Validated by

SBR_ARENA_SELFTEST=1 feeds it arena_lo=0x80100000 against sp=0x80200000 and it aborts with the diagnosis — verified firing 2026-08-11. It also reports at shutdown when it never ran, so a run that never calls OSSetArenaLo cannot be misread as a pass.

## Known failure modes

(none recorded yet)

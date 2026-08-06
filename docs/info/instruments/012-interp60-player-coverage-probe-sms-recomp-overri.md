---
id: I012
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

interp60 PLAYER COVERAGE probe (sms-recomp/overrides/interp60_snapshot.cpp, report_player_coverage)

## Validated by

It LIED until 2026-08-06: it treated gpMarioPos (0x8040E10C) as an object pointer, read mPosition.x as a vptr, got 0, and printed 'the player is never snapshotted and never substituted, so no alpha can move him' — a confident negative its method could not have contradicted. Now derives the object as ptr-0x10, REFUSES loudly if the derived +0x00 is not a plausible vtable, and was validated against an independent method (SBR_INTERP60_ACTCENSUS, which names マリオ at 0x8136383c in the substitution set): both now agree, is_tactor=YES in_snapshot_table=YES.

## Known failure modes

(none recorded yet)

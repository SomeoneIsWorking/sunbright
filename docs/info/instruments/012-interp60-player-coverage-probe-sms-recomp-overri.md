---
id: I012
kind: instrument
status: DISTRUSTED
created: 2026-08-06
distrusted_on: 2026-08-12
---

## Instrument

interp60 PLAYER COVERAGE probe (sms-recomp/overrides/interp60_snapshot.cpp, report_player_coverage)

## Validated by

It LIED until 2026-08-06: it treated gpMarioPos (0x8040E10C) as an object pointer, read mPosition.x as a vptr, got 0, and printed 'the player is never snapshotted and never substituted, so no alpha can move him' — a confident negative its method could not have contradicted. Now derives the object as ptr-0x10, REFUSES loudly if the derived +0x00 is not a plausible vtable, and was validated against an independent method (SBR_INTERP60_ACTCENSUS, which names マリオ at 0x8136383c in the substitution set): both now agree, is_tactor=YES in_snapshot_table=YES.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-12

RETIRED, not caught lying. It probes sms-recomp/overrides/interp60_snapshot.cpp (report_player_coverage), which was deleted in 21aa561 when 60fps was rebuilt as one module — sms-recomp/frame_interp/ — on dusklight's record-and-replace model. The file is gone, so the instrument cannot be run at all. It was still marked 'trusted', which is the worst state for a registry entry to be in: a later session consults this list INSTEAD of searching, so 'trusted' on a non-existent probe sends someone to look for a file that is not there. Player coverage in the current stack is answered by the interpolation audit's own per-population reporting (SBR_LERP60=1 + interp_reports).

> Every result this instrument produced is suspect until it is re-validated.

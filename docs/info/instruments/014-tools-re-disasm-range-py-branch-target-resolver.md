---
id: I014
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/re/disasm_range.py branch-target resolver

## Validated by

It LIED until 2026-08-06: it rendered EVERY bl target as nearest-preceding-symbol+offset, so a call to an unnamed function that merely follows a known one read as a call INTO that symbol. funcs.txt has entry addresses only, no sizes, and the gaps are full of weak/inlined functions — so 'inside the gap after N' and 'inside N' were indistinguishable. It cost a confidently wrong RE conclusion: a bl to the unnamed TLiveActor::calcRootMatrix (0x80218370) read as a call to setGroundCollision, and TBaseNPC's calcRootMatrix override was written off as a motion-blend routine on the strength of it. A bl target is a function ENTRY, so it is now named ONLY on an exact match and otherwise reported as an unnamed function at its address; local branches keep the offset form. Validated against BOTH classes: an exact-entry call still resolves (bl 0x8022aa2c -> MsMtxSetXYZRPH), and the unnamed one no longer borrows a name.

## Known failure modes

(none recorded yet)

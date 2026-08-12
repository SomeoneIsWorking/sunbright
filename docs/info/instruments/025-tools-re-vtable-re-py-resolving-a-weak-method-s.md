---
id: I025
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

tools/re/vtable_re.py — resolving a WEAK method's US address by aligning two vtables on their shared inherited slots

## Validated by

Both classes, 2026-08-12. POSITIVE: TResetFruit::control via the sibling override control__14TMapObjGeneralFv aligns with 87 agreeing shared slots and resolves to 0x801e23b4, which disassembles as a function entry (mflr r0) opening a 14-case jump table on a u16 state at +0xfc — coherent with a control state machine. NEGATIVE: the same tool, given overrides from a class sharing only a shallow JDrama::TViewObj prefix, produced slot indices of -110/-43/-45 with 6-8 votes and addresses landing deep inside unrelated functions, and printed them WITHOUT COMMENT. It now REFUSES (exit 2) on a negative slot or fewer than 20 agreeing slots, and warns when the target does not begin with a function prologue; the refusal was verified to fire on those exact inputs while the 87-vote case still passes. USE IT ONLY with an override from a class with the SAME IMMEDIATE BASE — that is what gives the vote enough shared slots to lock onto. DOES NOT COVER: free functions (not virtual, no vtable), and it cannot tell a correct alignment from a wrong one that happens to score well.

## Known failure modes

(none recorded yet)

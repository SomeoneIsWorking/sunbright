---
id: C045
kind: claim
status: holds
created: 2026-08-12
tags: re
depends: tools/re/vtable_re.py#vtable_base
---

## Claim

MWCC vtable dispatch offsets include a +8 bias: the vptr points at the vtable OBJECT start (two leading zero words), so slot N lives at byte 8+4N. A dispatch offset must NOT be compared against a slot index counted from the first function pointer.

## Evidence

Scanned every word of the 7 US .text sections for 'lwz r12, X(r12)': the smallest X is 8 (174 sites); X=0 and X=4 occur ZERO times, across 118 distinct offsets. Corroborated by four independent offsets resolving to the right method under the rule and the wrong one without it: 0xa0=receiveMessage, 0xa4=getTakingMtx (TTakeActor vtable size 0xB4), 0x104=makeObjDead, 0x158=makeObjDefault, 0x1ec=calcCurrentMtx.

## What would falsify it

find any lwz r12,0(r12) or lwz r12,4(r12) used as a virtual dispatch in the US image, or a vtable whose first function pointer sits at object offset 0

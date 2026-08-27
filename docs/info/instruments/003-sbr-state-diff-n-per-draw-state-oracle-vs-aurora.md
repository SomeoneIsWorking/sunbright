---
id: I003
kind: instrument
status: DISTRUSTED
created: 2026-07-28
distrusted_on: 2026-08-27
---

## Instrument

SBR_STATE_DIFF=<n> — per-draw state oracle vs aurora (runtime/render/state_oracle.cpp)

## Validated by

KNOWN-POSITIVE: distinct unit ids per side over a frame are comparable (u0 97 vs 108, u1 40 vs 39, u2 11 vs 8, u3 10 vs 7), so aurora's textures[m].texObj.texObjId really is where it holds the bound texture. MUST pair draws by STREAM BYTE OFFSET — see the distrust note on ordinal pairing.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-27

Frames are paired by oldest closed queue position without a shared frame identity, then by stream offsets that repeat every frame. Delayed, missing, or reordered frame completion can silently compare different frames. Add a shared frame ID and pair on (frameId, streamOffset), with reordered/missing controls.

> Every result this instrument produced is suspect until it is re-validated.

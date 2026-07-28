---
id: I003
kind: instrument
status: trusted
created: 2026-07-28
---

## Instrument

SBR_STATE_DIFF=<n> — per-draw state oracle vs aurora (runtime/state_oracle.cpp)

## Validated by

KNOWN-POSITIVE: distinct unit ids per side over a frame are comparable (u0 97 vs 108, u1 40 vs 39, u2 11 vs 8, u3 10 vs 7), so aurora's textures[m].texObj.texObjId really is where it holds the bound texture. MUST pair draws by STREAM BYTE OFFSET — see the distrust note on ordinal pairing.

## Known failure modes

(none recorded yet)

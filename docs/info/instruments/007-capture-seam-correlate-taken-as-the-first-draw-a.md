---
id: I007
kind: instrument
status: DISTRUSTED
created: 2026-07-28
distrusted_on: 2026-07-28
---

## Instrument

Capture-seam correlate taken as the first draw AFTER the snapshot (superseded design)

## Validated by

NEVER VALIDATED — recorded so it is not rebuilt

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-07-28

ov_shape_draw deliberately runs the real J3DShape::draw FIRST (it needs the matrix loads the draw issues), so at snapshot time this shape's draws are ALREADY in the stream and the next draw belongs to the NEXT shape. Looking forwards reported '47% of drawables carry the wrong material' and got as far as a committed ROOT CAUSE plus a code change; the true answer with the correct correlate is ZERO of ~900.

> Every result this instrument produced is suspect until it is re-validated.

---
id: I010
kind: instrument
status: trusted
created: 2026-07-29
---

## Instrument

SBR_DUMP_COPY=<path> — reads an EFB copy surface back to RGBA and reports its mean alpha

## Validated by

Reads the actual surface rather than inferring from the frame; showed the copy was this port's own composited frame (a feedback loop) and that its alpha is 254.8/255, which is why the compositing quad fully replaced the frame. Cross-checked against the visible frame change when copy ordering was fixed.

## Known failure modes

(none recorded yet)

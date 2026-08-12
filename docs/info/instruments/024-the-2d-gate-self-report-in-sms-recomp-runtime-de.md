---
id: I024
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

The 2D-gate self-report in sms-recomp/runtime/devices/dev_gxfifo.cpp (sbr_gxfifo_report_2d_gate) — per-reason decline breakdown, clip-space residency scored between the indexed and non-indexed draw classes, and a no-extent count

## Validated by

Three ways, because the first two cannot fail on their own. (1) The decline reasons must SUM to the declined total and the report warns when they do not — that assertion is what found 14 draws returning early from the triangulator with no counter, a path nobody had named. (2) Residency is scored against a CONTROL CLASS, not an absolute threshold: draws with no per-vertex matrix index went through the already-working path and set the baseline (76.3%), and the newly-enabled indexed class is compared to it (5510/5510). (3) The no-extent count exists because residency ALONE is incapable of reporting failure here: a matrix resolved from wrong bytes collapses a draw onto one point, and under an ORTHOGRAPHIC projection that point is (P[3],P[7]) — a screen corner, inside the volume — so a total decode failure would score 100% resident. DOES NOT COVER: whether the captured geometry is the RIGHT geometry (nothing here compares against aurora), or per-vertex TexMtxIdx UVs, which are reported as a known gap unconditionally including at zero.

## Known failure modes

(none recorded yet)

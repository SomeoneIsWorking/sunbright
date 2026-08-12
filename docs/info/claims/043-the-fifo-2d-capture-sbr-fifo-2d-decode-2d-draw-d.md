---
id: C043
kind: claim
status: holds
created: 2026-08-12
tags: render,2d,fifo
depends: sms-recomp/runtime/devices/dev_gxfifo.cpp#decode_2d_draw
---

## Claim

The FIFO 2D capture (SBR_FIFO_2D, decode_2d_draw) decodes 34601 of 34615 orthographic draws on a 300-present Delfino run; the 14 declined are degenerate lines that rasterise nothing. The newly-enabled indexed-matrix class is not merely accepted but lands on screen with real extent: 5510/5510 fully inside the clip volume against a 76.3% baseline for the class that already worked, and 0 of 5510 collapsed to a point.

## Evidence

run-safe.sh SBR_FIFO_2D=1 SBR_J3D_CAPTURE=1 SBR_STAGE=1 SBR_QUIT_AFTER=300, the '2D gate' block from sbr_gxfifo_report_2d_gate(). Decline reasons must sum to the declined total and the report warns if they do not. The no-extent count exists because clip-space residency ALONE cannot fail: a matrix resolved from wrong bytes collapses a draw onto one point, which under an orthographic projection is a screen corner and would score as resident.

## What would falsify it

the decline count rising above 14, the indexed class's fully-inside share dropping materially below the non-indexed baseline, or any nonzero no-extent count. NOT covered and so unable to falsify this: whether the captured geometry is the RIGHT geometry, and per-vertex TexMtxIdx draws whose UVs have no texgen matrix applied.

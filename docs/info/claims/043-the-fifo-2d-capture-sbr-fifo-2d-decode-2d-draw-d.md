---
id: C043
kind: claim
status: holds
created: 2026-08-12
tags: render,2d,fifo
depends: sms-recomp/runtime/devices/gx_fifo_2d.cpp#decode_2d_draw
reconfirmed: 2026-08-22
verified_at: 2026-08-22 17:25:09
---

## Claim

The extracted FIFO 2D capture (`SBR_FIFO_2D`, `decode_2d_draw`) decodes every non-degenerate
orthographic draw in the bounded stage-1 workload. It decoded 17,204 of 17,227 candidates; all 23
declines were geometrically degenerate, and neither indexed nor direct geometry produced a draw
collapsed to one point.

## Evidence

`./run-safe.sh SBR_STAGE=1 SBR_QUIT_AFTER=40 SB_DRAW_STATS=1 SBR_FIFO_2D=1`, using the 2D-gate
report from `sbr_gxfifo_report_2d_gate()`. Decline reasons must sum to the declined total and the
report warns if they do not. The no-extent control exists because clip-space residency alone cannot
fail: a matrix resolved from wrong bytes can collapse a draw onto one point that still lies inside
an orthographic clip volume.

## What would falsify it

Any non-degenerate orthographic candidate being declined, any nonzero no-extent count, or the
decline-reason sum disagreeing with the declined total. NOT covered and therefore unable to falsify
this: whether the captured geometry is the right geometry, and per-vertex `TexMtxIdx` draws whose
UVs have no texgen matrix applied.

## Re-confirmed 2026-08-22

Post-extraction bounded run-safe control examined 17,227 orthographic candidates, decoded 17,204, attributed all 23 declines to degeneracy, and reported zero collapsed draws in both geometry classes.

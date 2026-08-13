---
id: 7
title: Z map/guide transition renders black
status: resolved
symptom: Pressing keyboard C (GameCube Z) in gameplay starts the guide/map transition, but the screen is black instead of animating.
tags: input,guide,ui,render
created: 2026-08-13
updated: 2026-08-13
---

## Root cause (recomp path)

Retail `Hx_Test5` divides the EFB into a 10x8 grid. For each 64x64 cell it calls
`GXCopyTex(..., GX_TRUE)` and immediately draws the captured texture as a deforming strip. GameCube
hardware clears only the copy source rectangle. Aurora represented every copy-clear as a WebGPU
render-pass load clear, which clears the entire attachment. Tile one therefore erased the source
for tiles two through eighty, and the transition converged to black. A first rectangular-clear fix
still used an empty rectangle as the full-clear sentinel; the eighth row is outside SMS's visible
640x448 EFB and maps to zero height, so tile eighty was accidentally promoted back to a full clear.

The fixed representation carries an explicit rectangular-clear flag. Partial copies clear only
their mapped rectangle, and a mapped zero-area rectangle is a no-op. Full-EFB copies retain the
render-pass load-clear path.


## What was tried / dead ends


## Resolution

Aurora now performs partial copy-clears with a scissored clear draw. The recomp's missing 60 fps
seam at retail `Hx_Test5` (`0x8017df74`) gives each of its 80 deforming strips a stable identity, so
the vertex path can interpolate them between simulation ticks.

The Dolphin fastboot oracle reconstructed by `tools/oracle/build_dolphin_fastboot.sh` showed the
retail tiled circular close/reopen transition. The matched recomp run (`SBR_FASTBOOT=1`, Z on input
read 600) now renders the Plaza close and Guide reopen halves instead of black. Its self-report
measured exactly 40 calls and 3,200 strips over ticks 599..638. At 60 fps, 24,657 of 26,006
screen-wipe draws used the vertex interpolation path; the Hx_Test5 seam accounts for the expected
3,120 new consecutive-tick pairs (39 later ticks x 80 strips), with the first 80 necessarily born
without a previous sample. Both the 30 fps and 60 fps bounded runs exited zero and the kernel GPU
guard reported no timeout, reset, or fault.

Because the clear correction is at the shared GX copy contract, it also fixes the same black wipe
on the Delfino load path; it is not a guide-specific exception.

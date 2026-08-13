---
id: C047
kind: claim
status: holds
created: 2026-08-13
tags: recomp,efb,guide
depends: extern/aurora/lib/gfx/common.cpp#resolve_pass, extern/aurora/lib/gfx/clear.cpp#render
---

## Claim

Aurora partial GXCopyTex clear must use an explicit rectangular-clear discriminator; Hx_Test5's off-visible eighth row maps to zero area and an empty-as-full sentinel erases the complete EFB

## Evidence

Dolphin oracle scratch/screenshots/dolphin_guide_transition.png; forced-clear control scratch/screenshots/recomp_test5_magenta.png; fixed captures scratch/screenshots/recomp_test5_early.png and recomp_test5_reopen.png; scratch/logs/recomp_test5_reopen_60.log

## What would falsify it

if Dolphin clears the full EFB for a partial clear=true texture copy, or a matched recomp Z run again renders black with these clear paths unchanged

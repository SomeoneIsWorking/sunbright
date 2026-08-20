---
id: C055
kind: claim
status: holds
created: 2026-08-21
tags: recomp,widescreen,hud
depends: sms-recomp/overrides/hud_window_layout.cpp#extend_window_centered
---

## Claim

The widescreen te_w announcement frame and fill retain one centered coordinate system

## Evidence

J2DWindow source shows the frame is emitted at local x=0 from outer width; hud_window_layout_test failed on the former symmetric x1/x2 mutation and passes when the matrix shifts -pillar and both right edges gain 2*pillar. Same-size 1280x960 4:3 control and post-fix present-1200 windowless captures show one continuous band; run-safe reported zero GPU events. See sms-recomp/overrides/hud.cpp and hud_window_layout.cpp.

## What would falsify it

A same-phase 4:3/widescreen capture shows detached end boxes, the effective frame/content centres differ in the shipping helper test, or another clip truncates the text

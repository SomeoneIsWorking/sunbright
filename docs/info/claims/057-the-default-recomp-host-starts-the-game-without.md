---
id: C057
kind: claim
status: holds
created: 2026-08-21
tags: recomp,ui,startup
depends: sms-recomp/host/main.cpp#main
reconfirmed: 2026-08-21
verified_at: 2026-08-21 10:31:52
---

## Claim

The default recomp host starts the game without showing RmlUi; settings remain available through Escape

## Evidence

A windowless run-safe invocation routed through the shipping run.sh launcher with SBR_FASTBOOT=0 and SBR_QUIT_AFTER=80 initialized RmlUi hidden, mounted the disc, entered recompiled code at 0x8000522c, reached the game's BOOT and NLOGO states without UI action, exited 0, and logged zero GPU faults; SBR_UI_SELFTEST=2 separately verified the Escape-open/close route

## What would falsify it

A default launch shows host UI before game output, requires UI action to enter the DOL, or Escape no longer opens settings

## Re-confirmed 2026-08-21

The shipping run.sh launcher, exercised through run-safe with SBR_FASTBOOT=0 and SBR_QUIT_AFTER=80, entered recompiled code at 0x8000522c and reached BOOT/NLOGO without UI action; SBR_UI_SELFTEST=2 verified Escape still opened and closed the then-current 7-option menu; both runs exited 0 with zero GPU faults

---
id: C013
kind: claim
status: holds
created: 2026-08-04
tags: eclipse,scope
---

## Claim

Super Mario Eclipse integration is DEFERRED, not rejected: its 60fps (guest logic at double rate) is mutually exclusive with our lerp60 render interpolation, and its widescreen is already owned host-side in sms-recomp/overrides/widescreen.cpp. It is compiled PPC injected via Kuribo, so it can never touch the decomp runtime, and static recomp cannot run its runtime instruction patching without an interpreter fallback that does not exist.

## Evidence

debug_journal/2026-08-04_super_mario_eclipse_integration_assessment.md

## What would falsify it

if Eclipse's 60fps is found to be render-side rather than logic-rate, or if it ships a source distribution rather than DOL patches, the assessment must be redone

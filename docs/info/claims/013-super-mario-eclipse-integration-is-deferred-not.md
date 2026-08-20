---
id: C013
kind: claim
status: falsified
created: 2026-08-04
tags: eclipse,scope
falsified_on: 2026-08-21
---

## Claim

Super Mario Eclipse integration is DEFERRED, not rejected: its 60fps (guest logic at double rate) is mutually exclusive with our lerp60 render interpolation, and its widescreen is already owned host-side in sms-recomp/overrides/widescreen.cpp. It is compiled PPC injected via Kuribo, so it can never touch the decomp runtime, and static recomp cannot run its runtime instruction patching without an interpreter fallback that does not exist.

## Evidence

debug_journal/2026-08-21_bse_native_frame_rate.md

## What would falsify it

if Eclipse's 60fps is found to be render-side rather than logic-rate, or if it ships a source distribution rather than DOL patches, the assessment must be redone

## FALSIFIED 2026-08-21

The user explicitly removed Eclipse/Kuribo from scope on 2026-08-21, and BetterSunshineEngine's public fps.cpp provides directly portable native-rate source semantics; Sunbright now ports that behavior in sms-recomp/bse instead of integrating Eclipse.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

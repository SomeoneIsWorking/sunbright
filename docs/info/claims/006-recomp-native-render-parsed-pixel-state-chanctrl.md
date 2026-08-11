---
id: C006
kind: claim
status: holds
created: 2026-07-28
tags: 
depends: sms-recomp/runtime/render/state_oracle.cpp
---

## Claim

recomp native render: parsed pixel-state (chanctrl/amb/mat all 4 channels, ras selectors, combiner words, ksel, konst) matches aurora at 100% of ~29.4k draws/frame; channel 1 black-RGB is the game's real config

## Evidence

SBR_STATE_DIFF extended pix pass, debug_journal/2026-07-23_native_texgen_and_texmap_bisect.md 2026-07-28 final section

## What would falsify it

changes to dev_gxfifo XF/BP chanctrl parsing or aurora chanctrl decode

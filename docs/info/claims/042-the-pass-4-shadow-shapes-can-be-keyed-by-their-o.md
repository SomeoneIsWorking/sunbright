---
id: C042
kind: claim
status: holds
created: 2026-08-12
tags: 
depends: sms-recomp/frame_interp/tag_shadow.cpp
---

## Claim

the pass-4 shadow shapes can be keyed by their owning actor, and doing so costs no mispairing

## Evidence

The quad is live in r24 at the pass-4 SMS_DrawShape call (0x8022f3e4; LR 0x8022f3e8 discriminates the site) and r24 is callee-saved. Measured SBR_STAGE=1 SBR_LERP60=1, 600 presents, two runs per mode. Refusals ([100,inf) object motion) by population — fp control: 174 (162 J3D + 12 volume) and 170 (162 + 8); pass4owner: 170 (162 + 8 + 0 model) and 168 (162 + 6 + 0 model); default+join: 174 and 168. 550 draws per run tagged, all 550 resolving an owner, 0 falling through, and the shadow-model population contributing ZERO refusals in every run. J3D world is 162 in all six runs, so the instrument is not drifting; shadow volume varies 6-12, which is what a 2-4 count difference is worth. The historical ordinal key on this population produced 1128 mispairs against a control of 4.

## What would falsify it

a run where the pass-4 arrival count is 0 (the LR discriminator stopped matching), or where the shadow-model population contributes refusals

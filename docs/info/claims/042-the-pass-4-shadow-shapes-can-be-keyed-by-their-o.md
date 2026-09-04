---
id: C042
kind: claim
status: holds
created: 2026-08-12
tags: interpolation,shadow,re
---

## Claim

Pass-4 shadow shapes can use their owning actor as a stable interpolation identity.

## Evidence

At the pass-4 `SMS_DrawShape` call at `0x8022f3e4`, callee-saved register r24 holds the owner and LR
`0x8022f3e8` discriminates the site. Two paired 600-presentation controls resolved all 550 tagged
draws to owners and recorded zero shadow-model mispairs; the former ordinal identity produced 1,128.

## What would falsify it

A representative run reaches no calls at the discriminated site, fails to resolve an owner, or
attributes a continuity refusal to the shadow-model population.

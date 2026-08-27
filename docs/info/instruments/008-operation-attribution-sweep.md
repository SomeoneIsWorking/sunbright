---
id: I008
kind: instrument
status: DISTRUSTED
created: 2026-07-29
tags: render, recomp, attribution
distrusted_on: 2026-08-27
---

## Instrument

`SBR_ABLATE=1` — per-operation attribution for the SDL3-GPU GX compatibility render. Renders the frame once
per ablated GX operation (texgen, texture fetch, ras selection, TEV chain, konst, alpha test,
texmap routing) and scores every variant against THE SAME aurora frame as the baseline. Reports a
ranked table; a positive delta means replacing that operation with a neutral reference moved the
frame toward aurora, i.e. this port gets that operation wrong.

## How it is validated

`control:no-op` is an ablation id the shader has no branch for, so it renders the real pipeline and
MUST reproduce the baseline. The sweep also checksums every variant against the baseline's. Trust
the table only when the control is checksum-identical AND scores +0.0, and when each real ablation
has a DISTINCT checksum (identical checksums across different ablations mean the id is not
reaching the shader).

Validated 2026-07-29: control identical, +0.0; all seven real ablations distinct.

## What it caught about itself

Three self-inflicted failures before it produced a true statement: variants accumulated across all
presents between aurora callbacks (n=4544 vs baseline n=77); the ablation id was written to
`alphaRef.z`, already the `SBR_TEV_VIZ` selector, turning every variant into a visualisation mode
(control scored -11.8); and the first validation ran on loading-screen frames where every ablation
of an empty frame is trivially identical.

## What would falsify it

The control ablation ceasing to be checksum-identical to the baseline, two different ablations
producing the same checksum on a frame with geometry, or a variant count that does not equal the
baseline frame count.

## DISTRUSTED 2026-08-27

Aurora readback is not joined to the same native frame; each round-robin variant samples a different game frame; and control:no-op failure does not suppress the ranking. Exact tagged frame pairing and fail-closed control are required before attribution is trusted.

> Every result this instrument produced is suspect until it is re-validated.

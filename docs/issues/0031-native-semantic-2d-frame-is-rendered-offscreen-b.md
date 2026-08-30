---
id: 31
title: Native semantic 2D frame is rendered offscreen but never presented
status: resolved
symptom: Both runtimes produce a GPU-rendered native J2D frame, but Aurora still owns the visible window, so users cannot inspect the native result and no game draw visibly bypasses GX.
state_items: S004,S005
tags: renderer,semantic,presentation,j2d
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The shared semantic frame client initializes the SDL GPU platform without a window, encodes only into its private texture, and submits that texture for audit readback. Runtime composition never disables Aurora presentation or attaches the already-implemented SDL GPU presenter to the application window.

## Correct fix

Add one explicit native-2D preview mode shared by both runtimes. In that mode Aurora remains active offscreen as the retained GX reference, while the semantic client exclusively claims the application window and presents its 640x480 texture after encoding. Keep the ordinary offscreen audit distinct. Refuse hidden/headless preview startup, count actual presentations versus temporarily unavailable windows, and retain every original game draw body.

The preview is deliberately incomplete: missing 3D, particles, lights, and effects render as the controlled clear. It must never be described or persisted as the complete product renderer.

## Verification required

- parsing controls distinguish disabled, offscreen audit, visible preview, and invalid inputs;
- a live SDL GPU control presents a known semantic frame and observes the platform presenter path;
- a minimized/hidden-after-start control reports window-unavailable while still completing the semantic frame submission;
- bounded recomp and decomp preview runs reach real game frames without Aurora presenting over them;
- normal output and the offscreen audit remain unchanged.

### Resolution (2026-08-30)
Implemented one shared off/audit/preview output policy. Preview disables Aurora presentation, attaches the semantic client's SDL GPU target to the live window, refuses hidden startup, counts unavailable windows, and is exposed as ./run.sh --semantic-preview with an explicit incomplete-output warning. Guarded GPU, recomp, decomp, and offscreen-audit controls all passed; C091 records the falsifier.

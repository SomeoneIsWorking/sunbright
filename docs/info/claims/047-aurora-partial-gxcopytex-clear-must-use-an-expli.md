---
id: C047
kind: claim
status: holds
created: 2026-08-13
tags: efb,oracle
depends: extern/aurora/lib/gfx/common.cpp#resolve_pass, extern/aurora/lib/gfx/clear.cpp#render
---

## Claim

Aurora partial `GXCopyTex(clear=true)` handling must use an explicit rectangular-clear discriminator;
Hx_Test5's off-visible eighth row maps to zero area, and treating an empty rectangle as a full-clear
sentinel erases the complete EFB.

## Evidence

Dolphin's Hx_Test5 transition provides the independent reference. A forced empty-as-full control
cleared the complete image, while explicit zero-area rejection preserved both the early and reopened
guide views.

## What would falsify it

Dolphin clears the full EFB for this partial copy, or an exact matched oracle capture shows the guide
view black while these clear paths are unchanged.

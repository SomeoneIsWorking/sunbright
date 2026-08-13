---
id: C049
kind: claim
status: holds
created: 2026-08-13
tags: recomp,widescreen
depends: sms-recomp/overrides/hud.cpp#ov_window_private
---

## Claim

The scrolling announcement backdrop is J2DWindow pane te_w and must widen frame/content/clip together

## Evidence

/2dclass measured te_w content rect 427 wide alongside tet1/tet2; 1280x960 tick-1202 capture after draw_private widening spans scrolling text

## What would falsify it

a retail-oracle comparison shows te_w should retain its 4:3 width, or another outer clip still truncates the backdrop

---
id: C049
kind: claim
status: holds
created: 2026-08-13
tags: recomp,widescreen
depends: sms-recomp/overrides/hud.cpp#ov_window_private
reconfirmed: 2026-08-14
verified_at: 2026-08-14 01:29:01
---

## Claim

The scrolling announcement backdrop is J2DWindow pane te_w and must widen frame/content/clip together

## Evidence

/2dclass measured te_w content rect 427 wide alongside tet1/tet2; 1280x960 tick-1202 capture after draw_private widening spans scrolling text

## What would falsify it

a retail-oracle comparison shows te_w should retain its 4:3 width, or another outer clip still truncates the backdrop

## Re-confirmed 2026-08-14

J2DWindow drawSelf source restores/applies original unkEC child clip after widened frame draw; 1280x960 tick-1202 capture measured box x61..1085, text x149..999 (88/86 px padding), nearest FLUDD HUD x1120 (34 px gap); scan matched 7486 text pixels and 137 HUD-bearing columns

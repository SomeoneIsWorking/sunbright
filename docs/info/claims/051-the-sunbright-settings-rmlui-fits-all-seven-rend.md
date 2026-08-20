---
id: C051
kind: claim
status: holds
created: 2026-08-20
tags:
depends: sms-recomp/ui/settings_menu.cpp
---

## Claim

The Sunbright settings RmlUi fits all seven renderer/framerate controls inside a 1280x960 headless viewport

## Evidence

SBR_UI_SELFTEST=2 with SDL's windowless offscreen driver reported panel=(96,96) 1088x768, Play=180x49.2, visible choices=7/7, and a clean exit on 2026-08-20

## What would falsify it

The selftest reports INVALID, any required control has zero area, or the panel exceeds the viewport

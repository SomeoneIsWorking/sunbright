---
id: C051
kind: claim
status: holds
created: 2026-08-20
tags:
depends: sms-recomp/ui/settings_menu.cpp#SettingsMenu::layout_valid
reconfirmed: 2026-08-22
verified_at: 2026-08-22 12:20:36
---

## Claim

The Sunbright settings RmlUi fits all eight renderer/framerate/effects controls inside a 1280x960 headless viewport

## Evidence

SBR_UI_SELFTEST=2 with SDL's windowless offscreen driver reported panel=(96,96) 1088x768, visible choices=8/8, and a clean exit on 2026-08-22

## What would falsify it

The selftest reports INVALID, any required control has zero area, or the panel exceeds the viewport

## Re-confirmed 2026-08-20

SBR_UI_SELFTEST=2 pushed Escape through SDL into the shipping Aurora event route, opened the in-game Dusklight window, reported window=(96,96) 1088x768 with the then-current 7/7 renderer/framerate controls visible, exercised Runtime::pause_while_open, closed through a second Escape, exited 0, and run-safe observed zero GPU faults on 2026-08-20

## Re-confirmed 2026-08-21

SBR_UI_SELFTEST=2 pushed Escape through SDL into the shipping Aurora event route, reported window=(96,96) 1088x768 with the then-current 7/7 renderer/framerate controls visible, closed through a second Escape, exited 0, and run-safe observed zero GPU faults on 2026-08-21

## Re-confirmed 2026-08-22

2026-08-22 SBR_UI_SELFTEST=2 reported settings window (96,96) 1088x768 with 8/8 choices visible, opened and closed via Escape, exited 0, and run-safe observed zero GPU faults

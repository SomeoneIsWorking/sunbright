---
id: C051
kind: claim
status: holds
created: 2026-08-20
tags:
depends: sms-recomp/ui/settings_menu.cpp#SettingsMenu::layout_valid
reconfirmed: 2026-08-21
verified_at: 2026-08-21 10:28:58
---

## Claim

The Sunbright settings RmlUi fits all seven renderer/framerate controls inside a 1280x960 headless viewport

## Evidence

SBR_UI_SELFTEST=2 with SDL's windowless offscreen driver reported panel=(96,96) 1088x768, visible choices=7/7, and a clean exit on 2026-08-21

## What would falsify it

The selftest reports INVALID, any required control has zero area, or the panel exceeds the viewport

## Re-confirmed 2026-08-20

SBR_UI_SELFTEST=2 pushed Escape through SDL into the shipping Aurora event route, opened the in-game Dusklight window, reported window=(96,96) 1088x768 with 7/7 renderer/framerate controls visible, exercised Runtime::pause_while_open, closed through a second Escape, exited 0, and run-safe observed zero GPU faults on 2026-08-20

## Re-confirmed 2026-08-21

SBR_UI_SELFTEST=2 pushed Escape through SDL into the shipping Aurora event route, reported window=(96,96) 1088x768 with 7/7 renderer/framerate controls visible, closed through a second Escape, exited 0, and run-safe observed zero GPU faults on 2026-08-21

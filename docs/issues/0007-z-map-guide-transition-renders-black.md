---
id: 7
title: Z map/guide transition renders black
status: investigating
symptom: Pressing keyboard C (GameCube Z) in gameplay starts the guide/map transition, but the screen is black instead of animating.
tags: input,guide,ui,render
created: 2026-08-13
updated: 2026-08-13
---

## Root cause

`TMarDirector::updateGameMode` treats the C-key's GameCube-Z trigger (`0x10`) as
the guide/map request.  It enters `STATE_UNK10`, begins wipe 6, and invokes
`TGuide::setup(nullptr)` / `startMoveCursor()`.  The port leaves the required
per-frame `TGuide::perform` virtual as a no-op stub, so no guide pane state is
advanced or drawn during the wipe.  `TGuide::load` also omits the retail
`guide_1.blo` pane construction that `perform` consumes.  Thread collapse is
not involved.


## What was tried / dead ends


## Resolution

### Note (2026-08-13)
Reproduced with gpuguard-run safe decomp launch: SB_PAD_SCRIPT="120:Z 124:-" while stage 1 enters the Guide path. The log fires [STUB-CALLED] TGuide::perform. Static retail US GMSE01 inspection: TMarDirector::updateGameMode sees mButton.mTrigger & 0x10 and enters STATE_UNK10; nextStateInitialize starts wipe 6 and calls TGuide::setup(nullptr) and startMoveCursor(). Retail TGuide::perform @ 0x801791d0 is 0x610 bytes and draws/advances the map screen. Ours is an explicit no-op in sms-boot/boot_stubs/ring3_stubs.cpp. This rules out collapsed threads; the required fix is porting TGuide::load + perform, because load currently does not create guide_1.blo panes and perform cannot draw them.

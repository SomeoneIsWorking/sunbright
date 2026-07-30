---
id: C012
kind: claim
status: holds
created: 2026-07-30
tags: render
---

## Claim

recomp: 2D/J2D override work ALREADY EXISTS before any new hook is written — sms-recomp/overrides/diag_2d.cpp registers a census on J2DPicture::draw/drawSelf, J2DTextBox::draw/drawSelf and J2DScreen::drawSelf, and overrides/hud.cpp ports the TGCConsole2 widescreen HUD layout. A new J2D capture must EXTEND diag_2d.cpp, not add a second override for the same address

## Evidence

sms-recomp/overrides/diag_2d.cpp:116-120 (SB_OVERRIDE registrations); overrides/hud.cpp header; a duplicate J2DPicture::draw override written 2026-07-30 was announced with diag_2d's description, proving the address was already claimed

## What would falsify it

diag_2d.cpp or hud.cpp being deleted, or the override registry gaining duplicate-address detection that makes the hazard moot

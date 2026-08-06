---
id: C033
kind: claim
status: holds
created: 2026-08-06
tags: interp60
depends: sms-recomp/host/main.cpp
---

## Claim

Interpolated 60fps requires a QUEUED present mode. aurora's vsync=false selects Mailbox, which replaces the pending swapchain image when a newer present arrives — so the in-between frame of a two-present tick is discarded by the swapchain by design, while every counter reads 60fps. Interpolated runs now select vsync=true -> FifoRelaxed.

## Evidence

runtime log 'present mode Mailbox' vs 'FifoRelaxed'; debug_journal/2026-08-06_interp60_mailbox_discards_the_inbetween.md; verified both directions

## What would falsify it

if the user still sees the in-between frame dropped under FifoRelaxed, the discard was not the (only) mechanism

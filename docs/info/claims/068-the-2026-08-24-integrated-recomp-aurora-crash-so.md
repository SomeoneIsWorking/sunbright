---
id: C068
kind: claim
status: falsified
created: 2026-08-24
tags:
depends: sms-recomp/overrides/native_frame.cpp#present_tail, sms-recomp/runtime/devices/dev_gxfifo.cpp#gxfifo_build, extern/aurora
falsified_on: 2026-08-25
---

## Claim

The 2026-08-24 integrated recomp + Aurora crash-solidity build completed representative stage 1, 13, and 24 guarded runs, including headless, visible swapchain, and interpolated-60 coverage, with process exit 0 and no increase in the validated boot-wide amdgpu timeout/reset/fault counter; the final post-integration stage-1 run was 42 to 42.

## Evidence

debug_journal/2026-08-24_recomp_aurora_crash_solidity.md records the exact run matrix, 17/17 recomp tests, Aurora negative controls, and final 42->42 counter result.

## What would falsify it

A guarded recomp + Aurora run increases the validated amdgpu counter, fails shutdown, or changes the frame/FIFO/Aurora lifecycle code without rerunning these controls.

## FALSIFIED 2026-08-25

User-observed default recomp + Aurora run reached roughly 4,202 input polls, then RADV reported a lost innocent context and Dawn aborted on vkQueueSubmit with VK_ERROR_DEVICE_LOST. The short 120-400-present controls did not cover this lifetime; their zero-delta result cannot support crash-solidity.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.

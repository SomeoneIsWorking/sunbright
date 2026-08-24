---
id: C069
kind: claim
status: holds
created: 2026-08-25
tags: recomp,gxfifo,crash
depends: sms-recomp/runtime/devices/gx_fifo_contracts.hpp#classify_display_list_span, sms-recomp/runtime/devices/dev_gxfifo.cpp#inline_display_list
---

## Claim

Recomp FIFO accepts zero-byte GX display lists as empty operations while retaining fail-closed validation for every nonempty display-list span.

## Evidence

Exact failing 0x8157daa0+0 regression control passes; end-of-MEM1 16-byte positive and 17-byte overrun negative controls pass; full Clang sms-recomp build passes; run-safe stage 1 reached 700 presents and exited 0 with validated amdgpu counter unchanged 42 to 42. Prior 1250-present clean log observed each of two stable zero lists 1150 times.

## What would falsify it

A zero-byte display-list regression test aborts or a nonempty out-of-MEM1 display-list control is accepted, or a bounded recomp+Aurora run reproduces the FIFO abort.

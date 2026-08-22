---
id: C063
kind: claim
status: holds
created: 2026-08-22
tags: decomp,endian
depends: sms-boot/assets/spc_swap.cpp, decomp/sms/src/JSystem/JAudio/JALibrary/JASystemHeap.cpp
---

## Claim

An SPC blob's endian state must be detected from its structure, not remembered by pointer: allocator reuse can place fresh big-endian data at an address previously holding a swapped blob

## Evidence

sms-boot/assets/tests/spc_swap_test.cpp address-reuse control; decomp/sms commit d3e64ea6

## What would falsify it

SPC ownership changes so buffers cannot be reloaded at reused addresses, or the structural detector fails a valid SPC fixture

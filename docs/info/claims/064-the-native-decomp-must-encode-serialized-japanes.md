---
id: C064
kind: claim
status: holds
created: 2026-08-22
tags: decomp,encoding
depends: tools/build/encode_sjis_literals.py, sms-boot/CMakeLists.txt
---

## Claim

The native decomp must encode serialized Japanese ordinary C/C++ literals as Shift-JIS bytes; Clang otherwise leaves UTF-8 bytes and exact retail name searches fail

## Evidence

tools/build/encode_sjis_literals.py selftest and full sms-boot build; debug_journal/2026-08-22_internal_work_profiling_and_decomp_rebase.md

## What would falsify it

The retail data is proven UTF-8, or Clang gains and the build enables an equivalent verified execution-charset transform

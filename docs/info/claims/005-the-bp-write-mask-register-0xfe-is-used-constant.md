---
id: C005
kind: claim
status: holds
created: 2026-07-28
tags: native-render
depends: sms-recomp/runtime/devices/dev_gxfifo.cpp
---

## Claim

The BP write-mask register 0xFE is used constantly by this game and must be modelled

## Evidence

3.5 MILLION mask writes per report, more than any other BP register; GDSetGenMode2 arms 0x07FC3F before GENMODE, GDSetCullMode arms 0xC000. aurora has always implemented merged = (cached & ~mask) | (value & mask)

## What would falsify it

if the 0xFE write count ever reads zero, the mask handling is inert and anything blamed on it is wrong

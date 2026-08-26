---
id: I032
kind: instrument
status: trusted
created: 2026-08-26
---

## Instrument

`sms-boot/runtime/tests/bathtub_tumble_test.cpp` — focused shipping-function test for
`TBathtub::tumble`.

## Validated by

The 0° and 90° cases distinguish the sine and cosine destinations and signs, the 2500-strength
case pins the `0.0001f` scale, and `unk29A = 1` is a negative no-mutation control. The test is linked
against staged `decomp/sms/src/MoveBG/MapObjCorona.cpp`, not a copied test implementation.

## Known failure modes

The synthetic quarter-turn JMath table validates indexing and arithmetic but does not validate the
runtime initialization or contents of the game's full sine table.


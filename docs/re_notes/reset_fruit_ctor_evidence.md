# The three fork-local placeholder fields, and what the retail image says

2026-09-29. After the upstream sync, a scan found 19 placeholder names this fork used
that upstream no longer has anywhere. Sixteen were half-applied upstream renames and are
fixed. The remaining three are **not** renames:
they are fields that exist only in this fork, and each needed the retail image to
decide whether it could be named at all.

The distinction matters. "used here, upstream has it nowhere" cannot by itself tell a
missed rename from a fork addition, because
a fork addition satisfies that predicate by construction. Treating all three as
missed renames would have meant renaming them to whatever upstream happened to call
the field at a *different offset* — which makes the placeholder convention lie about
the layout and imports a different meaning. So each was read against the binary.

## TResetFruit, retail ctor US 0x801E1BF4

Resolved to `__ct__11TResetFruitFPCc` in `reference/sms_gmse01_funcs.txt`. Decompiled
from the US DOL through `tools/re/DolLoadLocal.py` and `DecompDumpLocal.py`:

```c
undefined4 * FUN_801e1bf4(undefined4 *param_1) {
  FUN_801e3a20();
  *param_1 = &DAT_803d2ba4;
  param_1[8] = &DAT_803d2bc8;
  param_1[0x66] = *(undefined4 *)(unaff_r2 + -0x2428);   // +0x198
  *(undefined1 *)(param_1 + 0x69) = 0;                  // +0x1A4
  *(undefined2 *)(param_1 + 0x67) = 0xff;               // +0x19C
  *(undefined2 *)((int)param_1 + 0x19e) = 0xff;         // +0x19E
  *(undefined2 *)(param_1 + 0x68) = 0xff;               // +0x1A0
  *(undefined2 *)((int)param_1 + 0x1a2) = 0xff;         // +0x1A2
  return param_1;
}
```

Four 16-bit stores of `0xff` at +0x19C..+0x1A3, which is 255 per component of the
`GXColorS10` — retail's actual component values, not a guess at "white".

**`unk19c` -> `mTevReg0Color`, renamed.** Its only read in the whole tree is
`SMS_InitPacket_OneTevColor(getModel(), 0, GX_TEVREG0, &unk19c)`: the field's address is
handed to the routine that sets TEV register 0's colour. Combined with the ctor, that
determines what the field is. The name states exactly that much and claims nothing
about Nintendo's own identifier, which no available evidence recovers.

**`unk198` keeps its placeholder.** The ctor is the only writer and nothing in the
recovered tree reads it. What settles the *value*: the load is `r2 - 0x2428`, and
`r2` is the small-data-2 base `0x80416BA0`, so the constant is at `0x80414778`, inside
DATA7 (`0x8040EBA0..0x80417800`) at file offset `0x3ED018`. Those four bytes are
`00 00 00 00` — so the port's `unk198 = 0.0f` is **bit-faithful to retail**, not a
placeholder standing in for an unknown. A field nothing reads has no recoverable
meaning until something that reads it is recovered.

**`unk1a4` keeps its placeholder.** Written to 0 by the ctor, read once: at US
0x801E2994 it gates a second `makeObjDead()` in Ricco Harbour (`mMap == 3`). Nothing
in the recovered tree ever sets it non-zero, so whatever sets it in retail is not
known. Same shape as `unk194` beside it.

## TConsoleStr, retail ctor US 0x80172800

Resolved to `__ct__11TConsoleStrFPCc`. The decompiled body stores to +0x2B4
(`*(undefined2 *)(param_1 + 0xad) = 0`) and to +0x2B8 (`param_1[0xae] = 7`) and
**never touches +0x2B5**. So `unk2B5` is a member this port added — the same shape as
`TResetFruit::unk194`, which the port documents as a deliberate deviation because
retail leaves it uninitialised and the decrement below it is `!= 0`-guarded.

**`unk2B5` keeps its placeholder**, for a different reason: every use of it is inside
code this port never recovered. The two blocks in `ConsoleStr.cpp` that set and clear
it test the same condition (`gpMarDirector->mState != 4`) and have empty bodies under
`// TODO: uknown stuff`. A field whose only readers are unrecovered code has no
recoverable meaning. Recovering that region would settle it.

## What this leaves

One of the three is namable and is named. The other two are provably not namable from
the evidence available, and now say so at the point of declaration with the address
that would settle them — which is the useful form of a gap: the next person reads one
comment instead of re-deriving the same dead end.

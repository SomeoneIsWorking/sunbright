# TResetFruit::control — state map (US GMSE01 0x801e23b4)

The plaza fruit's per-frame behaviour. Reached every run (it is one of the nine `[STUB-CALLED]`
stubs a Delfino run prints), and unported.

**The address is not in `reference/sms_gmse01_funcs.txt`** — `control` is a weak override. It was
resolved through the vtable: `vtable_re.py initMapObj__11TResetFruitFv control__14TMapObjGeneralFv`
aligns TResetFruit's vtable against a sibling with the SAME immediate base (`TMapObjBase`) and
agrees on 87 shared slots. See instrument I025 for why the same tool must be distrusted when that
number is small.

## Shape

396 instructions, but that number is misleading. It is a `switch` on a **u16 at `this+0xfc`**,
dispatched through a 14-entry jump table at **0x803D2818** — and the table has only **7 distinct
targets**:

| state(s) | body | size | note |
|---|---|---|---|
| 0, 4, 5, 7, 8, 9, 10 | `0x801e29c4` | 8 insn | **the epilogue** — these seven states do NOTHING |
| 1 | `0x801e23f8` | ~90 insn | |
| 2, 3 | `0x801e2768` | ~60 insn | one shared body for two states |
| 6 | `0x801e26e4` | ~33 insn | |
| 11 | `0x801e2560` | ~97 insn | |
| 12 | `0x801e2858` | ~35 insn | |
| 13 | `0x801e28e4` | ~56 insn | |

So the real work is **six bodies**, the largest ~97 instructions. That is six tractable ports, not
one 396-instruction wall — which is the whole reason for writing this map down before starting.

Verified rather than assumed: `0x801e29c4` disassembles as
`lwz r0,0xfc(r1); lfd f31; lwz r31/r30/r29; mtlr; addi r1,r1,0xf8; blr` — a function epilogue with
no work before it. Seven of the fourteen states really are no-ops, not an artefact of the table.

## Before porting a body

`control` is reached, so unlike TGuide this code will actually run. But see **issue #6**: any
`load`-time model access faults because `init()` has not run yet in this port. `control` runs
per-frame, after init, so it is not subject to that — worth confirming per body rather than
assuming.

The state variable is a u16 at `+0xfc`. Whoever ports the first body should name it in
`TResetFruit`'s header from what the bodies do with it, not from a guess here.

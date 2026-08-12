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

## Case 6 — decoded (0x801e26e4, 33 insn)

Ported-ready except for one operand, noted below.

```
case 6:
    TMapObjBall::control();                  // bl control__11TMapObjBallFv on this
    if (unkF8 & 0x04000000) return;          // rlwinm. r0,r0,0,5,5 -> that single bit
    if (mStateTimer > 0) return;             // computed as a bool then tested, hence the
                                             // li 1 / li 0 / clrlwi. dance at 0x801e2704
    if (mHolder != nullptr) {                // TTakeActor::mHolder (+0x68)
        mHolder-><vtable +0xa0>(this, 8);    // <-- SEE BELOW
        mHolder->mHeldObject = nullptr;      // TTakeActor::mHeldObject (+0x6C)
        mHolder = nullptr;
    }
    mVelocity.x = mVelocity.y = mVelocity.z = 0.0f;   // +0xAC/B0/B4, constant read from
                                                      // SDA2 r2-0x2428, which IS 0.0
    mState = 12;                             // +0xFC, the switch variable
    return;
```

So the state is "the fruit is dropped": tell the holder, clear both ends of the holding
relationship, kill the velocity, advance to state 12.

### OPEN: the virtual at vtable +0xa0

Called as a THREE-argument virtual on `mHolder` — `(holder, this, 8)`. Two readings were tried
and BOTH are ruled out or unconfirmed, so it is not being guessed:

* **Not `receiveMessage`.** The obvious reading, given `(sender, message)`. But
  `receiveMessage__11TResetFruitFP9THitActorUl` sits at vtable slot 29, byte offset **0x74**,
  found by locating that listed symbol in both vtables it appears in. 0xa0 is a different slot.
* **Reading TResetFruit's own vtable at +0xa0 is not an answer.** It gives
  `bind__14TMapObjGeneralFv`, which takes no arguments and so cannot be what a 3-argument call
  site invokes. The static type at the call site is `TTakeActor*`, and `TTakeActor` is an
  ANCESTOR (THitActor -> TTakeActor -> TLiveActor -> TMapObjBase -> TResetFruit), so a derived
  class's entry at that index says nothing about which virtual the compiler was addressing.

**To settle it:** find the class that FIRST declares the virtual at index 40 by walking the
vtables up the hierarchy and finding where slot 40 stops being an inherited pointer — or count
declared virtuals from `JDrama::TNameRef` down through `TTakeActor`. Do that before porting this
line; everything else in case 6 is unambiguous.

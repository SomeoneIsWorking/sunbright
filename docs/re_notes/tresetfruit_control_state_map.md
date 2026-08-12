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

### RESOLVED: the virtual at vtable +0xa0 is `receiveMessage`

Called as `mHolder->receiveMessage(this, HIT_MESSAGE_UNK8)`. Ported.

This section previously said the identification was OPEN, and gave two reasons that both turned
out to rest on the same wrong assumption, so the mistake is kept here rather than deleted -- it
will recur in any other slot arithmetic done in this project.

**MWCC vtables carry two leading zero words and the vptr points at the OBJECT START, so every
dispatch offset already includes that +8.** Measured, not assumed: across the whole US `.text`,
the smallest `lwz r12, X(r12)` is X=8 (174 sites) and X=0 or X=4 never occur once. That is only
possible if slot 0 lives at byte 8. The note this replaces asserted "no bias, verified", which is
what produced both dead ends.

With the bias applied, `__vt__10TTakeActor` (size 0xB4, so its last function pointer is at 0xB0)
lays out as:

| byte | slot |
|---|---|
| 0xa0 | `receiveMessage` (inherited from THitActor) |
| 0xa4 | `getTakingMtx` |
| 0xa8 | `ensureTakeSituation` |
| 0xac | `moveRequest` |
| 0xb0 | `getRadiusAtY` |

Reading the offset as unbiased shifted every entry by two slots and made the call look like
`ensureTakeSituation` -- a no-argument method, which cannot be what a three-argument call site
invokes. That contradiction was the tell, and it was there in the earlier pass; it was recorded
as "unconfirmed" instead of being followed.

The layout above was recovered without any `__vt__` symbol (the US reference file carries
functions only): find a vtable word equal to a known US method of a TTakeActor subclass, walk
back to the inherited `JSGSetScaling__Q26JDrama6TActorFRC3Vec` pointer as a shared ANCHOR, and
read the four slots after it. Two independent subclasses (TBGTakeHit, TResetFruit) agree, and
the anchor is an inherited slot both carry unchanged, which is what makes them comparable --
ordinals counted per-vtable would not have been.

Corroborated semantically as well: `src/Enemy/bossgesso.cpp:269` does
`mHolder->receiveMessage(this, HIT_MESSAGE_UNK8)` in the identical held-object context.

Do NOT walk backwards from a vtable word to find its start by scanning for two zero words: a
pure virtual is a zero slot INSIDE the table and stops the walk early. That was tried here and
reported TBGTakeHit's vtable as 0x90 bytes when it is 0xB4.

## Ported 2026-08-12 — ALL FOURTEEN STATES

`decomp/sms/src/MoveBG/MapObjBall.cpp` implements the whole function. Nothing about it is a stub
any more, and the loud per-state reporter that carried the gap has been deleted along with the gap.

| state(s) | what it does |
|---|---|
| 0, 4, 5, 7, 8, 9, 10 | nothing (their table entries are the epilogue) |
| 1 | lying in the world: clear hit bit 0, run the collision list through `TMapObjBall::touchActor`, arm `MAP_OBJ_FLAG_DISAPPEARING` with `getLivingTime()`, go to 11; rebuild the matrix if the ground plane belongs to an actor |
| 2, 3 | carried / in flight: `TMapObjGeneral::control`, tick `unk194` down, track the holder's `getTakingMtx()` lifted by `unk190`, else rebuild the matrix unless at rest on static ground |
| 6 | drop check while carried -> release the holder, kill velocity, go to 12 |
| 11 | settled, plus the Gelato (map 4) sand case: push the fruit up 20/frame while `SMS_GetSandRiseUpRatio` is still RISING -> then the same release-and-drop tail as state 6 |
| 12 | vanish: restore scale, `PARTICLE_MS_ENM_DISAP_A_W`, `MSD_SE_SMOKE_EFFECT`, sleep 0xf0 frames, go to 13 |
| 13 | respawn: white tint back, `awake`, default/dead/`calcRootMatrix`/`J3DModel::calc`, wait 360, clear DISAPPEARING, go to 10 (and straight back to dead on Ricco with `unk1a4`) |
| >13 | ignored, by retail's own `cmplwi r0,0xd; bgt` |

States 6 and 11 end in the same 21 instructions; that shared tail is written once, as the inline
it evidently was, rather than copy-pasted.

Two things fell out of this that matter beyond the fruit:

* **The rest threshold in `TResetFruit::perform` was a STOPGAP and was wrong by four orders of
  magnitude.** That line hard-coded 0.01 with a comment saying the real SDA2 constant had not
  been looked up. States 2 and 3 load the same constant, so it got looked up: the word at guest
  0x804147a8 is 0x36800000 = 3.8147e-06. Fixed there too.
* **`unk194` is not initialised by retail's constructor.** `TResetFruit::kicked` stores to it and
  states 2/3 decrement it while non-zero. On the host the port zeroes it in the constructor,
  which is a deliberate deviation recorded in the header -- inheriting heap garbage would count
  down for two billion frames.

Verified on a real SB_STAGE=1 run: the fruit reaches state 1 and stays there, no crash, and
run-safe.sh reports zero amdgpu events. States 6/11/12/13 are the pickup-and-throw path and are
not exercised by a run that never touches a fruit; that is a coverage gap in the VERIFICATION,
not in the port, and is stated here rather than left for a reader to assume.

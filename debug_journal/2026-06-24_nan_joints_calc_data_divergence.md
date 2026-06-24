# 2026-06-24 — Fail-fast on NaN joints landed; root cause narrowed to calc-time TRS divergence

Per the user directive "make NaN fail fast imo", `M3UModel::perform(2)` now `OSPanic`s on any
NaN in a post-`unk8->calc()` joint matrix (translation or rotation cells), naming the joint by
index AND by joint name, dumping the matrix, the joint's persistent `mTransformInfo` (BMD-loaded
TRS) and its `mtxCalc` pointer, plus a full 0..n joint walk showing the whole skeleton state.
Opt-out: `SB_ALLOW_NAN_JOINTS=1` for runs that need to reach past the NaN.

## The current first NaN
First panicking joint (deterministic at file-select):

    joint 21/29 name='eff_sldr_L' (nan_t=1 nan_r=0)
    matrix m=[r0(0,0,-0,-nan) r1(0,0,0,-nan) r2(0,-0,0,-nan)]
    BMD-TRS scale=(1,1,1) rot=(0,0,0) translate=(17.7,0,0) mtxCalc=(nil)
    model=Mario's body (ma_mdl1.bmd), 29 joints

NaN translation, all-zero rotation — looks like a degenerate parent-concat propagation.

## What's broken — not what we expected
1. **It's NOT the sin/cos table.** `JMANewSinTable(0xC)` runs at boot (verified by a temporary
   probe that has since been removed). The table is initialized.
2. **It's NOT the BMD load.** Every joint's persistent `mTransformInfo` reads correct values
   (scale=(1,1,1), reasonable rotations, anatomically correct translates) when dumped at panic
   time via the joint walk. The BMD swapper `swap_JNT1` (native/assets/bmd_swap.cpp) uses the
   correct stride of `0x40` (the J3DJointFactory.hpp comment says `Size: 0x30` which is wrong;
   the actual sizeof(J3DJointInitData) is `0x40` with `mMin@0x28`, `mMax@0x34`).
3. **It's NOT mtxCalc anim overrides.** Every joint has `mtxCalc == nullptr` — basic/softimage
   calc only, no BCK anim driving Mario at file-select.
4. **`J3DMtxCalcSoftimage::calcTransform` is what runs** for Mario's body (29-joint model,
   filtered by `mModelData->getJointNum() == 29`). NOT Basic, NOT Maya.

## What IS broken — the data divergence (`SB_J3D_CALC=1`)
The `info` parameter `J3DMtxCalcSoftimage::calcTransform` receives is **DIFFERENT** from what
`getTransformInfo()` returns at panic time on the same joint. Concrete divergence (file-select
frame, ma_mdl1.bmd, 29 joints):

    Joint 9  jnt_leg_L2:  walk BMD scale=(1,1,1) rot=(0,0,1126)   trans=(20.80,0,0)
                          calc info  scale=(1,1, 3.99e12) rot=(0,0,1012) trans=(20.80,0,0)
    Joint 10 chn_foot_L:  walk BMD scale=(1,1,1) rot=(0,0,14151)  trans=(20.25,0,0)
                          calc info  scale=(3.99e12, 3.99e12, 3.99e12) rot=(138,138,-20986) ...
    Joint 2  jnt_waist:   walk BMD rot=(-16383,0,-16383)
                          calc info  rot=(-16384, 1194, -16384)   <-- subtle but real diff

So: at calc time, the joint's TRS data reads as garbage/different. At panic time (later in the
SAME `M3UModel::perform(2)`), reading via the same `mModelData->getJointNodePointer(k)->
getTransformInfo()` path returns the correct BMD-loaded values. Two ways this can happen:

A. **The trace is logging a DIFFERENT calc call.** `seen[]` dedups by joint index — the FIRST
   call per joint is logged. That first call may be a calc invoked during BMD load/model init
   (where TRS isn't yet finalized), not the per-frame perform(2) calc that triggered the
   panic. Walk at panic time reads the now-final TRS. (LIKELIHOOD: HIGH — the per-frame
   `info` should be the same address as walk's `getTransformInfo()`, so a value diff means a
   different point in time / different model.)
B. **Memory aliasing.** Some other code writes into the joint's mTransformInfo between calc
   and walk and corrupts it — but the WALK shows correct data, so this would require the
   write to be REVERSED before walk. Unlikely.

## Next session's targets — narrow A vs find the real corruption
1. **Drop the `seen[]` dedup** in the calcTransform traces in
   `reference/sms/src/JSystem/J3D/J3DGraphAnimator/J3DJoint.cpp`. Log every call (rate-limit
   if needed). If the FIRST calls (init time) have bad TRS and LATER per-frame calls have good
   TRS, the panic-time `info` is fine and the question is *why ARE the matrices NaN*. If the
   per-frame call also reads bad TRS — that's the real corruption to chase.
2. **Log `&joint->mTransformInfo` at walk time and compare to `&info` at calc time.** If they
   match by address but differ by content, real corruption between calls. If they differ by
   address, indexing/aliasing.
3. **Sanity-check the basic-vs-softimage choice:** is Mario's body actually loaded with the
   Softimage mtx type? Check `J3DJoint::getMtxType()` per joint and what `J3DJoint::calcIn`
   dispatches to.
4. **Compare a working `J3DMtxCalcBasic::calc` (which reads from `joint->getTransformInfo()`
   directly) vs `J3DMtxCalcSoftimage::calc` (inherited from Basic).** Both go through
   `joint->getTransformInfo()`. But the Softimage `init()` override DIFFERS: it does not
   multiply baseMtx by baseScale per-column, just MTXCopy. That's the only behavioral diff.
   Could be the source of the broken accumulation. Worth tracing.

## Diagnostics kept (committed, env-gated)
- `SB_PANIC_FAIL`: `M3UModel::perform(2)` panics on NaN joint with full context + joint walk.
  Opt-out `SB_ALLOW_NAN_JOINTS=1`.
- `SB_MARIO_DBG`: `[mario] perform / calcAnim / cap-pose / cap-entry` invocation traces.
- `SB_J3D_CALC`: `[j3dcalc-basic / -soft / -maya] mario j<N> rot=… s=… t=… info_ptr=…`
  per-joint per-variant calcTransform trace, filtered to Mario's 29-joint model.

## Repro
    cmake --build build-native --target sms-boot -j$(nproc)
    pkill -9 -x sms-boot; S=""; for f in $(seq 200 12 1690); do S="$S ${f}:START $((f+6)):-"; done
    timeout -s KILL 200 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
      SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_SEL_DBG=1 SB_J3D_CALC=1 \
      SB_PAD_SCRIPT="$S" ./build-native/sms-boot > scratch/frames/m.log 2>&1
    grep -a "\[j3dcalc\|NaN joint\|--- joint-mtx walk" scratch/frames/m.log | head -80

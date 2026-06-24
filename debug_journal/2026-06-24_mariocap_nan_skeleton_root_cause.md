# 2026-06-24 — TMarioCap "TOO BA letters": root cause is Mario's J3D skeleton calc returning NaN

Continuation of the perfect-boot-to-file-select line. The prior session correctly identified the
file-select rainbow shapes as `TMarioCap` (mariocap.bmd) but its proposed fix — "own Mario's
perform passes" — was based on incomplete evidence. This session traced the call chain end-to-end
and found that **Mario's perform passes are already driven correctly**; the real defect is
downstream in the J3D animation/skeleton evaluation.

## What works (confirmed via `SB_MARIO_DBG`)
- The perform list IS calling `TMario::perform` every frame with a varied bit mix. Observed in
  one file-select frame: 0x10000000, 0x204, 0x08000000, 0x20C, 0x3001, plus 0x2/0x20/0x40/0x8000.
- Bit 0x1 IS delivered (in 0x3001). `is_performing=0, freezeTimer=0` so the `&1` branch runs in
  full: `playerControl`, `setPositions`, `mCap->perform(1)`, `calcAnim(2,graphics)` —
  which calls `mCap->perform(2)`.
- `TMario::calcAnim` builds a **correct** `baseMtx`: translation `(950, 0, -1000)`, rotation =
  identity. Copies it into `mModel->unk8->getBaseTRMtx()`.
- Bit 0x200 IS delivered (in 0x204 and 0x20C). `entryModels` runs → `mModel->perform(0x200)` →
  `mCap->perform(0x200)`.
- `TMarioCap::perform(0x200)` enters **exactly one** cap (`unk10[0]` = HAT, with `unk4 = 0x1`,
  helmet+glasses inactive). Selection logic (`offFlag1OnAllShapes`/`onFlag1OnAllShapes`) IS
  correct — only the active cap's shapes pass `J3DJoint::entryIn`'s `shape->checkFlag(1)` filter.

## What's broken — Mario's J3D anim/skeleton produces NaN matrices
After `mModel->perform(2)` (which calls `M3UModel::perform(2)` → `updateIn() → unk8->calc() →
updateOut()`), the head joint matrix is **NaN**:

    [mario] calcAnim baseMtx.t=(950.0,0.0,-1000.0) r0=(1.000,0.000,0.000) anim=195
    [mario] cap-pose  pos=(950,100,-1000) joints(mhead=28 head=26)
                      headMtx.t=(-nan,-nan,-nan) anim=195 freeze=0

So `mCap->unkC->setBaseTRMtx(mModel->unk8->getAnmMtx(mJointIdMHead))` plants a NaN base on the
cap's J3DModel. The cap's 8 shapes then transform through NaN, projecting to deterministic-but-
garbage NDC positions — that's the 5-shape "TOO BA" rainbow at the bottom of the frame. **It's
one cap, 8 shapes**, not three caps stacked, not the title logo.

`anim=195` = `ANIM_WAIT` — Mario IS in the correct file-select idle animation; the data wiring
is just not producing valid joint matrices.

## Where the NaN comes from (next session's target)
The chain is `M3UModel::perform(2)` (M3UModel.cpp:112):
    if (param_1 & 2) {
        updateIn();          // updateInMotion: animation evaluation, sets jnt->setMtxCalc()
        unk8->calc();        // J3DModel skeleton recursion through mtx calcs
        updateOut();
    }

Candidates for the NaN seed:
- `M3UModel::updateInMotion` (M3UModel.cpp:39) — for each `M3UMtxCalcSetInfo`, it pulls a
  `J3DAnmTransform*` from `unk4->unk4[info.mAnmTransformIdx]` and calls
  `frameCtrl.update(); anmTrans->setFrame(currentFrame);`. If `currentFrame` is NaN or the anim
  data buffer is absent/un-byteswapped, the evaluation feeds NaN into the joint's mtx calc.
  Likely class: `MActorAnmData` for Mario's wait BCK isn't bound, or isn't byteswapped (cf. the
  J3D-anim-swap class from memory `anm-swap-and-watergun-region`).
- `J3DModel::calc` → `J3DMtxCalcBasic::recursiveEntry` — if any anim controller is null or
  partially set up, the per-joint matrix recursion may produce NaN.

## Action for the next session
Trace where the NaN first appears. Concrete steps:
1. Add a probe at the END of `M3UModel::updateInMotion` that dumps the frame value + anim ptr +
   the first/head joint's mtx-calc state. NaN here = anim data / frameCtrl.
2. If updateInMotion looks clean, add a probe at the END of `J3DModel::calc` dumping the head
   joint's `mNodeMatrices[mJntNo]` translation. NaN here but not before = the skeleton recursion.
3. Cross-check that Mario's BCK files (`/data/mario/...bck`) load + byteswap correctly via
   `SB_MODEL_TRACE` (or extend it to log BCK loads similarly).

The file-select cap is the FIRST surface of this NaN; expect it to also affect Mario's body
rendering (his hat/body/hands all use joints) and any other character that animates in sms-boot.
So fixing the anim/skeleton calc is a substantial, high-value milestone — not just a cap fix.

## Diagnostic kept (`SB_MARIO_DBG`, env-gated, low-rate)
Logs at three points, all keyed `[mario]`:
- `perform #N flags=0xXXXX is_performing=… freezeTimer=… pos=…` — `TMario::perform` invocation.
- `calcAnim baseMtx.t=… r0=… anim=…` — Mario's base TR matrix before `mModel->perform`.
- `cap-pose pos=… joints(mhead=…) headMtx.t=… anim=… freeze=…` — head-joint matrix the cap
  inherits (NaN here = the J3D skeleton calc is broken).
- `cap-entry unkC=unk10[N] active(helmet=… glasses=…) unk4=0x…` — which cap is entered.

In: `reference/sms/src/Player/MarioMain.cpp`, `MarioDraw.cpp`, `MarioCap.cpp`.

## Repro (unchanged; results require reaching the file-select)
    cmake --build build-native --target sms-boot -j$(nproc)
    pkill -9 -x sms-boot; S=""; for f in $(seq 200 12 1690); do S="$S ${f}:START $((f+6)):-"; done
    timeout -s KILL 200 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
      SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_SEL_DBG=1 SB_SEL_DUMP=1 SB_SEL_DUMP_N=160 \
      SB_MARIO_DBG=1 SB_PAD_SCRIPT="$S" ./build-native/sms-boot > scratch/frames/m.log 2>&1
    grep -a "\[mario\]" scratch/frames/m.log | head -8

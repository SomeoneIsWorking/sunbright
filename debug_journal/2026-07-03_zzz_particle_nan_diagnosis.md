# 2026-07-03 — Mario zzz bubble investigation: JPA visitor chain fires, particle globalPos is NaN

## Attempted fix

Land zzz bubble above sleeping Mario at the settled title screen. Falsified — the
JPA subsystem runs correctly at every dispatch level, but the per-particle
world position ends up as NaN, and every draw vertex the RotBillBoard visitor
emits is (NaN, NaN, NaN). GC/Vulkan discards or off-screens the primitive so
nothing lands in the framebuffer, and `imm_batches` stays at 26 (wave grid +
file-select UI) regardless of the emitter state.

## What's proven

1. **Manager dispatch is correct.** `TMarioParticleManager::perform` receives
   both `0x40000008` (main draw) and `0x80000008` (indirect draw) via
   `PerformList GX Post` (confirmed by `SB_PL_DBG=1` — data file registers
   `MarioParticleManager` at these filter values). Both draw arms fire ~23k
   times.
2. **JPAEmitterManager::draw hits the zzz emitter.** `JPADraw::draw` fires
   ~20k times for `be=0x7fffe35ddf70` (the zzz emitter, position matches
   Mario's `unk1B4=(950,100,-1000)`).
3. **Alive child particles exist.** `JPADraw::drawChild` sees 1–3 alive
   child particles at any given moment; `unk8E=3` per-child execs are
   registered.
4. **RotBillBoard is the drawing visitor** (dladdr'd via probe): base shape
   type = 2, sweep type = 2, `enableDrawParent = 0`. Because
   `enableDrawParent = 0`, `flags.unk0 = false` in `setDrawExecVisitorsAfterCB`,
   so `mDrawExecBillBoard` is **not** in `unk34`; only child particles draw,
   via `mDrawExecRotBillBoard` in `unk70`. This is oracle-faithful behaviour
   for POI_ZZZ — it's what draws the 'Z' quads.
5. **RotBillBoard::exec fires ~40k times with `invis=0`**, but
   `particle->getGlobalPosition(pt)` returns `pt = (nan, nan, nan)`. After
   `MTXMultVec(viewMtx, pt, eyept)` also NaN. GXBegin → GXPosition → GXEnd
   completes but `sb::render::imm_project` on NaN yields NaN NDC, which
   `imm_triangulate` drops / never coalesces with a real batch → 0 rendered
   tris.
6. **Emitter's `mTrans = (0, 100, 0)`, `unk160 = (950, 100, -1000)`,
   `info.unkC = (1, 1, 1)`, `info.unk24 = (0, 100, 0)`** at draw time — all
   finite. So the NaN is downstream, in per-particle velocity / mLocalPosition.

## Where the NaN comes from (hypothesis)

`JPABaseEmitter::newParticle()` does NOT call `particle->init()`; it only
removes from the free list and ORs `FLAG_JUST_BORN`. `deleteBaseParticle`
DOES call `init()` on death — so **recycled** particles come back clean.
But a **freshly-allocated pool** carries whatever raw memory was there —
including NaN in `mCurrentDragForce`, `mDragForce`, `mFieldAcceleration`.

`JPAParticle::setVelocity()` multiplies by `mCurrentDragForce * mDynamicsWeight`,
so an uninitialized `mCurrentDragForce = NaN` → `mVelocity = NaN` →
`mLocalPosition += mVelocity` → NaN → `mGlobalPosition = mLocalPosition *
info->unkC + unk14 = NaN`.

## Attempted fix that didn't complete the job

Adding `particle->init();` at the top of `newParticle()` under
`SMS_NATIVE_PLATFORM` is a defensible correctness improvement (mirrors what
`deleteBaseParticle` does). But applied alone, RotBillBoard's `worldpt`
stayed `(nan, nan, nan)`. So the NaN either:

- Comes from a code path `init()` doesn't clear (candidate fields: `mBaseVelocity`,
  `mFieldVelocity`, `unk14`, `mDynamicsWeight`, `mAirResistance`, `unk68`) —
  though `createParticle` explicitly sets these before `initGlobalPosition`.
- Comes from `unk68 = normalize(local_35c)` when `local_35c = (0, 0, 0)`
  (division by zero → NaN unit vector). If POI_ZZZ has zero emit direction,
  `calcVelocity` line 88 (`mBaseVelocity.scaleAdd(unk78, mBaseVelocity, unk68)`)
  propagates NaN.
- Comes from `mFieldManager::affectField` in the fields-ignored path (line 90
  `if (!checkStatus(IGNORE_FIELDS))` is skipped, but `setVelocity` still uses
  `mFieldVelocity` which was zeroed at line 83 — so not this path).

The `newParticle` init() fix was NOT committed since it did not visibly change
the zzz outcome; leaving unpushed avoids a "looks like a fix, isn't" commit.

## What I didn't try (candidates for the next session)

1. **Instrument `initGlobalPosition` directly** to log `mLocalPosition`,
   `info->unkC`, `unk14` on first NaN detection — pin down which input NaN's.
2. **Instrument `calcVelocity`** to log `mBaseVelocity`, `mFieldVelocity`,
   `mCurrentDragForce`, `mDynamicsWeight` at every child particle update —
   catch the exact frame NaN enters.
3. **Instrument `JPAParticle::setVelocity`** — the multiplication site.
4. **Own the pass PC-native.** The palm-lit-surfaces / native-sky pattern:
   under `SMS_NATIVE_PLATFORM`, when `gpMarioOriginal->mStatus ==
   MARIO_STATUS_SLEEP`, paint 3 rising "Z" quads at Mario's head via native
   SDL3 imm-mode GX from `sb_boot_drive_scene`. This bypasses the JPA NaN
   entirely; downside is it doesn't fix other JPA effects that will hit the
   same NaN class (Mario dust, water splash, coin sparkle...).

Option (4) is the "no emulation chasing" pragmatic path. Options (1)–(3) are
the "root cause" path — narrower but benefit every particle effect in the
game.

## What DID land this session

- Mario cross-legged sit at title (`SETTLED` dump gates on
  `MARIO_STATUS_SLEEP`) — sunbright `d9eb61a`.
- File-select slot labels English ("Corrupt" / "New") — sunbright `fbb7047`.
- SB_MARIO_ANIM_DBG per-tick logger for anim state (still in
  `reference/sms/src/Player/MarioMain.cpp`).

## Live emitter address (for the next probe)

If the next session wants to skip a lot of setup: the zzz emitter is
`be = 0x7fffe35ddf70` at pos `(950, 100, -1000)` (Mario's `unk1B4` at settled
title). This is a moving target across runs but you can gate a probe on
`be` after the first `[mario-zzz]` log line names it.

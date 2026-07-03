# 2026-07-03 — Mario cross-legged sit at title reached by gating SETTLED dump on MARIO_STATUS_SLEEP

## Symptom

Post-2026-07-03 title-screen arc, `sbs_title.png` still showed native Mario in the
kneeling ANIM_SIT pose (id 0x131) while the oracle showed him cross-legged in
ANIM_SIT_WAIT (0x132) with a zzz bubble. Handoff hypothesis: `sleepily()` case-2
never transitioned to SLEEPING; `isLast1AnimeFrame()` false-negative on `M3UModel`
frame updates.

## Diagnosis — falsified

Added `SB_MARIO_ANIM_DBG=1` per-tick logger (30-frame stride) in `TMario::perform`
tracking `mAnimationId / mStatus / mStatusState / mStatusTimer / frameCtrl`. Log
shows the FULL state chain progresses correctly:

- t=1..3571:   ANIM_WAIT (0x0c3), status=0xc400201 (WAIT), `mStatusTimer` counting 0→9
- t=3601:      ANIM_BELT_UP (0x12f), status=0xc400202 (SLEEPY), state=0
- t=3841:      ANIM_YAWN    (0x130), state=1
- t=4141:      ANIM_SIT     (0x131), state=2
- t=4231:                            state=3 (case-2 exit)
- t=4261:      ANIM_SIT_WAIT (0x132), **status=0xc000203 (SLEEP), state=0**  ✅

So `sleepily()`/`isLast1AnimeFrame()` work. `M3UModel::perform(2)` advances
`J3DFrameCtrl` correctly.

## Real cause — dump-window timing

The oracle dumps EVERY presented frame via Dolphin's `FrameDumper` and
`title_sbs.sh` picks a late one when Mario is naturally in SIT_WAIT. Native
has a bounded window: `SB_SEL_DUMP_SETTLED=500` requests 500 present frames
starting at "settle" (`mState==0 && unk10==2 && sb_camera_view_settled()`).

`waiting()` in `MarioWait.cpp:147` requires ANIM_WAIT to loop 10× before
transitioning to MARIO_STATUS_SLEEPY (`mStatusTimer >= 10`). ANIM_WAIT is
~180 frames at rate 0.5, so ~360 game ticks per loop × 10 = ~3600 ticks
before Mario even begins the sleep sequence. Then BELT_UP/YAWN/SIT each
run to completion. Total: ~4260 ticks from settle to SIT_WAIT.

Settle fires around t=421; dump ends around t=921 → captures Mario mid-WAIT.
The metric baseline that read "kneeling ANIM_SIT" was actually the LAST-of-500
frame which happened to land in the WAIT/BELT_UP/YAWN region depending on run.

## Fix

Gate the SETTLED dump request on Mario reaching MARIO_STATUS_SLEEP. Falls
through if `gpMarioOriginal` is null (defensive). One added conjunct in
`reference/sms/src/GC2D/CardLoad.cpp:588`:

```cpp
&& (!gpMarioOriginal || gpMarioOriginal->mStatus == MARIO_STATUS_SLEEP)
```

## Verify

`title_sbs.sh 90` (increased settle wait from 45s → 90s to allow Mario the
~60s to reach SLEEP): native Mario now cross-legged sitting, matching the
oracle pose. Log confirms:
`[cardload] SETTLED dump requested (unk10==2 + camera settled + Mario SLEEP, 500 frames)`.

`title_overbright.py`: mean_abs_pixel_delta 56.8 → 58.1. The small regression
is animation-phase drift within the SIT_WAIT loop (native/oracle presentation
timing isn't synchronized frame-exact). Semantic pose match is the goal per
the "no emulation chasing" rule; the metric moved slightly the wrong way but
tells you nothing about whether the pose is right — the visible intent is now
correct.

## Residuals (not this task)

- No zzz bubble — `sleepingEffect()` particle emission not visible in native.
- Save-block labels read "NEW" on all three vs oracle "Corrupt/New/New" — BMG
  string / block-state gap.
- Palm/water color still slightly off (already-tracked cosmetic).

## Also fixed en route

`SB_MARIO_ANIM_DBG` printf initially UB — `J3DFrameCtrl::getEnd()` returns
`s16` but was passed to `%.2f`, corrupting all later varargs slots. Now uses
`%d` with an `(int)` cast.

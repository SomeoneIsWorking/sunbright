# Mario sit pose at title — was a dump-timing artefact, not a Mario animation bug (2026-07-03)

## TL;DR
The task-list observation "native Mario stands/runs vs oracle cross-legged
sit" was misattributed. Mario IS driven by the game's perform-list dispatch;
his animation progresses through the natural `waitingStart()` → SLEEPY
chain each frame. The problem was that `title_sbs.sh` dumped the first
frame **7 frames after** `TCardLoad` triggers `waitingStart()` (via
`SB_SEL_DUMP_SETTLED=8`), so the captured frame caught Mario mid-standing
WAIT anim (mAnimationId=0xC3), before the 10-frame `waiting()` timer even
fires to transition him to SLEEPY.

Fix: bump the settled-dump window to 500 frames so the last-dumped frame
(mtime-newest, which `title_sbs.sh` already picks) captures Mario after
he's transitioned to a sit-related pose.

## Evidence

### Mario's pose across time (SB_SEL_DUMP_SETTLED=500)
- Frame  7  (settled+7):   Standing, WAIT anim (mAnimationId=0xC3).
- Frame 100 (settled+100): Kneeling / sit-transition.
- Frame 200 (settled+200): Same kneeling pose.
- Frame 499 (settled+499): Same kneeling pose (mario_500.png).
- Frame 1499 (with =1500): Same kneeling pose (mario_1500.png).

So Mario transitions to a sit-like pose within ~50-100 frames post-settle
and stays there — matching the SLEEPY/SLEEP state chain in
`MarioWait.cpp`.

### Comparison to oracle
Oracle Mario at the same TCardLoad state has fully cross-legged
`SLEEP_WAIT` pose with a `zzz` bubble — a deeper state
(`MARIO_STATUS_SLEEP`, sub-state 2 = `SLEEP_WAIT`).

Native reaches SLEEPY case-2 (`ANIM_SIT` kneeling transition) but appears
to STOP there — `isLast1AnimeFrame()` for the SIT anim never fires the
transition to `MARIO_STATUS_SLEEP`. This is a subtler animation-frame
progression issue, deferred as separate work.

## Change
`tools/render/title_sbs.sh`: `SB_SEL_DUMP_SETTLED=8 → 500`. This captures
the settled-scene state (Mario sitting rather than caught mid-getup),
which is the fairer visual comparison for the SBS harness. It does NOT
change any product code — this is purely a test-harness alignment.

## Residual (deferred)
- Mario doesn't reach the full `SLEEP_WAIT` cross-legged pose. Needs a
  targeted trace of the SLEEPY→SLEEP transition (probably an
  `isLast1AnimeFrame()` false-negative on the SIT anim under our MActor
  frame update path).

## Aside: the "palm tree missing" observation from the handoff was also
partly misattributed. Palm fronds DO render (visible dark green cascading
from top-right in `sbs_blocks_fixed.png`), just dimly-lit vs oracle's
bright palm. Trunk column is present on the far right edge but very dark.
Lighting fidelity gap, not a dispatch gap. Deferred as cosmetic.

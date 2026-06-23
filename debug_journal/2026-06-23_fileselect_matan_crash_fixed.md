# 2026-06-23 — file-select (stage 15) mState 0 reached + renders; matan OOB crash fixed

## Milestone
The real file-select (gameplay STAGE 15, the title scene → Start → file/score select) now
**reaches mState 0 and RENDERS** under sms-boot. Frame: Delfino beach backdrop + "→ OPTIONS"
2D menu element (`scratch/frames/fsel_0004.png`). Commits: sub `ff5e2f8` (matan), parent `2d7db76`.

## Root cause fixed — matan OOB atntable read (SEGV)
The title→file-select transition crashed in `matan()` (MarioUtil/MathUtil.cpp), called from the
option camera: `CPolarSubCamera::perform(1)` → `calcFinalPosAndAt_()` →
`CLBRevisionLookatByAngleX` → `CLBCrossToPolar` → `matan(252.7, 828.6)`.

`matan` is an octant-reduced atan2. `GetAtanTable(a,b)` returns `atan(b/a)` by indexing
`atntable[(b/a)*1024]`, valid ONLY for `b <= a` (index ∈ [0,1024]; table has 1025 entries). The
four branches where `param_1 < param_2` passed the args **unswapped**, so the index became
`(larger/smaller)*1024` (e.g. 3357) → OOB read. On GC that returned adjacent garbage (no crash);
on PC it SEGV'd. The camera inputs were perfectly valid (not NaN/degenerate) — purely a
reconstruction bug (the function even carried a TODO admitting the symmetries weren't worked out).

FIX: swap the args in exactly those four branches so `GetAtanTable` always gets `(larger, smaller)`
and the ratio stays ≤ 1. In-range branches (`param_1 >= param_2`) untouched → the working title
camera (mState 3) is unaffected. Added an `SMS_NATIVE_PLATFORM` fail-fast print in GetAtanTable
for any future OOB index (dumps p1/p2/idx). Verified: zero matan OOB, ctest 28/28.

## Supporting fixes (parent 2d7db76)
- **present capture drain-every-frame** (native/render/sms_boot_present.cpp): the scene/imm
  capture buffers are append-only per frame and were only reset when a dump rendered → unbounded
  growth (31304 batches / 30288 textures accumulated over ~1714 frames at the file-select). That
  both leaked host RAM and made an event-dump render every accumulated frame at once → 30k Vulkan
  texture submits wedged the GPU (drmSyncobjWait / watchdog). Now drains on every present
  (file-select frame = ~56 batches).
- **sb_boot_request_dump(n)**: event-triggered frame dump (next N presents), called by TCardLoad
  when mState hits 0 (SB_SEL_DUMP) since the present-frame number isn't known ahead of time.
- **nvk renderTevFrame**: skip+log textures with w/h > 4096 (a garbage dim would memcpy GBs OOB).
- TCardLoad `unk14→mState`/`unk38→mGamePad` rename finally committed (10e8683 had left it
  uncommitted — HEAD was inconsistent).

## State trace (SB_SEL_DBG) — title→file-select progression
`mState 10→9→3 (title wait-for-Start) →8 (camera pan, moveToLoadFromTitle) →0 (file/score select)`.
mState 3 dwells ~1200 frames because `mIntroChaseTimer` counts 300→0 slowly (camera calc runs ~1
per ~4 perform-frames); Start only registers once introChase==0. mState 0 reached ~perform-frame
1714 (~145 s headless even with SB_TURBO — iteration is slow).

## Card-read machine FULLY TRACED — next bug = null format-dialog panes (waitForChoice SEGV)
At mState 0, TCardLoad::changeScene() runs the card-read progress machine (unk1C):
`UNK30 (readOptionBlock) → UNK31 (loadOption) → UNK0 (getBookmarkInfos) → UNK2 → UNK6`.
**UNK6 = CARD_RESULT_BROKEN**: a blank/unformatted memcard reads as "broken" (CARDMount/CARDCheck
fail on the empty FS — by design; mIsMounted stays true). changeMode(BROKEN) → PROGRESS_UNK6 = the
"There is no data — format the card?" confirmation. `changeMode` calls `probe()` → `CARDProbeEx`
returns READY (card present), so at UNK6 `getLastStatus()==READY` (traced: `UNK6 rc=0`) and the
`rc==READY` branch runs `waitForChoice(UNK8, UNK4, 1)`.

**THE NEXT BUG (precisely pinned): `waitForChoice` case 0 SEGVs on a null dialog pane.** It touches
`unk468->getPane()->show()` and `((J2DPicture*)unk484[0]->getPane())->mWhite = ...`. Those panes are
built in TCardLoad setup (CardLoad.cpp:221-247) from `unk28` (load.blo) via `new TExPane(unk28,
TAG)` / `unk28->search(TAG)`: `unk468`='w_7', `unk484[0/1]`='s_6a'/'s_7a', `unk47C`='m_2a',
`unk480`='m_2b'. In the US `load.blo` one or more of these is missing → TExPane/search yields a null
getPane() → deref crash (verified: the process core-dumps the same frame it first enters UNK6, right
after `UNK6 rc=0` prints). This is the **US-disc region-tolerance / missing-pane class**
([[us-disc-vs-jp-decomp-region-tolerance]], the J2DScreen dummy-pane tolerance already applied to
~250 sites). The blank-card path is otherwise CORRECT — it reaches the format dialog exactly as the
real game does.

NEXT STEPS:
1. **Make the format dialog crash-safe (region-tolerance).** Find which of w_7/s_6a/s_7a/m_2a/m_2b
   is null in US load.blo (add a setup-time fail-fast log at CardLoad.cpp:221-247, or check
   TExPane ctor when the pane tag is missing — does it already store a dummy like J2DScreen::search?).
   Then either render the dialog (if the panes exist under different US tags) or guard waitForChoice
   so a missing pane no-ops. Drive confirm with START (checkFrameMeaning 0x20) → `format()` → re-read
   → blocks get `makeBlockNormal` (CardLoad.cpp:2654 / 1859).
2. ALTERNATIVELY skip the format dialog by providing a pre-formatted memcard image so getBookmarkInfos
   returns READY (UNK2 → UNK13) directly. (scratch/memcard_chan0.raw is auto-created BLANK by
   native/platform/card_impl.cpp; a real formatted SMS card image would bypass UNK6.)
3. Then: 3 FileLoadBlock 3D models + per-file score panes (load_score.blo, setupScoreScreen,
   loadBookmark, TCardBookmarkInfo unk40[3]); file selection → start gameplay (setNextArea).

GOTCHA: mState 0 is only reached ~perform-frame 1690 (~145 s headless even with SB_TURBO) because
`mIntroChaseTimer` (title intro camera chase) counts 300→0 at ~0.25/perform-frame; Start to leave the
title only registers once introChase==0. Keep START pulses running PAST frame 1690 to drive the
dialog (`for f in $(seq 200 12 2600)`). Each iteration is ~3 min — budget accordingly.

## Repro (watchdog ON; ~3 min/run)
    pkill -9 -x sms-boot; S=""; for f in $(seq 200 12 1500); do S="$S ${f}:START $((f+6)):-"; done
    timeout -s KILL 190 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
      SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_SEL_DBG=1 SB_SEL_DUMP=1 \
      SB_PAD_SCRIPT="$S" ./build-native/sms-boot > scratch/frames/fs.log 2>&1
    # SB_SEL_DUMP dumps boot_0001..6 when mState hits 0. SB_TEVFRAME_DBG = batch/texture scale.

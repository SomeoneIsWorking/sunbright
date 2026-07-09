# 2026-07-09 (session 2) — title GAMEPLAY↔MOVIE bounce loop, JKRHeap::find off-by-one, stopSeq seam, OSGetTime hot-path

Continuation of `2026-07-09_aurora_repoint_regressions_and_null_texmap.md` ("SIGSEGV around
scene construction" blocker). Workflow note: this session ran as orchestrator + Sonnet
subagents (investigation, builds, verification runs) — that split worked well.

## 1. SolidHeap OOM 0x1950 — root cause was `JKRHeap::find()` off-by-one (FIXED)

The repeated `SolidHeap OUT OF MEMORY requiredSize=0x1950` at title setup was already
journaled (2026-07-07) as the benign LP64 RARC side-array fallback. The *actual root
cause* goes deeper and is now fixed:

- `JKRHeap::find()` (`reference/sms/src/JSystem/JKernel/JKRHeap.cpp:149`) tested
  `mStart <= memory && memory <= mEnd` — **inclusive at `mEnd`**, but `mEnd` is
  one-past-the-end (`mEnd = data + size` in every ctor).
- `gpEmitterManager4D2`'s JPABaseField solid heap (created in `JPAEmitterManager` ctor,
  64 fields, deliberately zero-slack ≈ 0x2a00 span) is bump-allocated immediately before
  `loadParticle()`'s 0x200000 particle.arc buffer — so the buffer starts EXACTLY at that
  heap's `mEnd`.
- `findFromRoot(buffer)` therefore attributed the particle.arc buffer to the saturated
  10.5 KB field heap (children win over parents). Consequences: (a) the RARC side-array
  alloc (270 entries × 0x18 = 0x1950) OOM'd there every mount (survived via the
  documented fallback chain), and (b) `JKRMemArchive::open` stored the WRONG `mHeap`
  for later frees.
- Fix: `memory < mEnd` under `SMS_NATIVE_PLATFORM` (retail `<=` preserved off-platform —
  it's a latent retail bug GC allocation order never triggers). Verified: 0 OOM lines
  after fix (was 8/boot).

## 2. THE blocker — TMovieDirector::direct() uninitialized errc → director rebuild loop (FIXED)

The "setup runs 8×, then teardown SEGV" was NOT a setup failure:

- Title's idle-attract demo (`mMovie=12`, autodemoA.thp) triggers legitimately;
  `TApplication::proc` swaps to `APP_STATE_MOVIE`.
- `TMovieDirector::direct()` (`MovieDirector.cpp`) read an **uninitialized `void* errc`**
  after `OSJoinThread` — the native `OSJoinThread` (`sms-boot/runtime/sdk_stubs.cpp`) is a
  no-op that never writes the out-param, so garbage made `if (errc) return 5` bounce every
  movie straight back to GAMEPLAY → `delete mDirector` + fresh `TMarDirector` → full
  re-setup, in a tight loop (the fresh director's idle timer re-fires immediately).
- The IDENTICAL bug was already fixed in `TMarDirector::direct()`
  (`MarDirectorDirect.cpp:71-88`, long comment there); `TMovieDirector` never got it.
  Fix: `void* errc = nullptr;` — same pattern.
- The "8" was not a retry table — it's how many bounces happened before the teardown SEGV
  (deterministic on this box, no coded constant).

## 3. Teardown SEGV in JAIBasic::stopSeq — unported-audio seam (GUARDED)

`~TMarDirector → MSound::exitStage → MSBgm::stopTrackBGMs → JAIBasic::stopSeq` crashed
writing through `param_1->getSeqParameter()` (`JAISound::unk38`), which is null because
the JAS sequence backend (the named unported audio arc) never attached a real sequence to
the BGM handle recorded in `smBgmInTrack[]`. GC can never see this state. Added an
`SMS_NATIVE_PLATFORM` early-out in `stopSeq`
(`reference/sms/src/JSystem/JAudio/JAInterface/JAIGFrameSequence.cpp`) doing handle-side
cleanup only — explicitly marked to be deleted when the JAS mixer arc lands.

## 4. New defect found+fixed: aurora OSGetTime per-call mktime (hot path)

With the loop fixed, the game ran but crawled: live thread sampling showed the main thread
inside `OSGetTick → OSGetTime → mktime/tzset/__tzfile_read → fstatat(/etc/localtime)`.
`extern/aurora/lib/dolphin/os/OSTime.cpp` computed the UTC offset with `localtime_r` +
TWO `mktime()` per call; OSGetTick is called from every frame-pacing spin. Fixed by
computing the offset once in a static initializer.

## 5. Workflow/tooling landed this session

- **`SB_HEADLESS=1`** (USER hard rule: all automated runs headless, agents included) —
  implemented at the `window::show_window()` callsite in `extern/aurora/lib/aurora.cpp`
  (window is created hidden; we just never show it). Registered in CLAUDE.md env list.
- `.claude/settings.json`: `worktree.bgIsolation=none` (background sessions edit the
  checkout directly per the commit-to-main workflow).
- Existing instrument that paid off: `SB_JKR_BT=1` (OOM caller backtrace in
  JKRSolidHeap) — pinned the 0x1950 alloc to `loadParticle → mountFixed` in one run.

## Dead ends / corrections

- My first reading — "setup re-runs because something after genObject FAILS" — was wrong
  in mechanism: setup never fails (`TMarDirector::setup` always returns 0;
  `setupObjects`' return isn't even checked). The rebuild came from the movie bounce.
- The 8 interleaved OOM/skip lines pre-fix were an artifact of 8 bounce iterations each
  mounting particle.arc onto the misattributed heap; post-`find()`-fix the 8 consecutive
  skip lines are simply the 8 MapObjFlagManager entries in the stage-15 scene — both
  "8"s are coincidences of the same loop count, neither is a retry table.
- gdb `break file:line` doesn't resolve on this Release binary (no line info) — use the
  env-gated in-process backtrace diagnostics instead.

## 6. (2026-07-10) THP movie-skip seam + hx_wipe port resurrection — attract loop complete

- **THP stubs** (`sms-boot/runtime/sdk_stubs.cpp`): `THPPlayerGetState()` returned 0 forever
  → `TMovieDirector::direct` busy-spun in STATE_PLAYING (3.7M direct() calls / 90 s). Stubs
  now model the real states (RE'd from excluded `THPPlayer.c`: 1=prepared, 2=playing,
  3=finished-normal, 5=DVD-error): `Play` jumps straight to 3 so the game's own
  end-of-playback path (`decideNextMode`) runs. NOTE: 3 is the normal end, NOT 5 — 5 takes
  MovieDirector's error branch. Verified: title logo renders (screenshot), movie 12 completes.
- **hx_wipe**: movie 9 (Entrance.thp) then parked in FADE_IN — `Hx_MovieStartSyncEx`,
  `Hx_UpdateWipe`, `Hx_GetWipeType` were `return 0` stubs (gates: MovieDirector.cpp:295 and
  ScrnFader.cpp:228-231 — the fader can never reach FULLY_FADED_IN for ANY wipe). A complete
  verified port existed (`sms-boot/common/hx_wipe_impl.cpp` + ctest, sessions 12/14) and was
  deleted by the one-runtime consolidation (`d6d15d7`) though nothing in it depended on retired
  infra. Resurrected as `sms-boot/runtime/hx_wipe.cpp` + `runtime/tests/hx_wipe_test.cpp`
  (ctest `platform_hx_wipe_test`: DONE at 204 frames, SE gate once, THP gate once at frame
  131 — all green). Attract loop now cycles 12→9→12→9→12 cleanly.
- **Attract loop is real GC behavior**: `CardLoad.cpp:940-950` alternates movies 12/9 every
  45 s idle at the title (flag 0x3001C). Title is not supposed to hold.
- Also landed: `restlut_swap.{h,cpp}` (completes reference/sms `a47d69d4`, which shipped
  only the caller side — build was red), CMake comment fix (sdk_gap_stubs.cpp never existed).
- **Lesson (workflow)**: before re-porting a subsystem, `git log --all --full-history` for
  prior ports — the consolidation deleted at least one verified port that nothing superseded.

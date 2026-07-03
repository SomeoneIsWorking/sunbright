# Stage-1 (Delfino Plaza) first-frame — falsifications + working recipe

Follow-up to `2026-07-03_zzz_native_paint.md` and the TManhole load-path fix
(commits sms `b5f54d4` / sunbright `56ddc74`). Handoff said "TManhole crash,
then SolidHeap OOM stall." First half true. Second half — the "SolidHeap OOM
stalls the load" hypothesis — is FALSE. Root cause named below; unblocks the
plaza-sweep opener.

## Falsified: "SolidHeap OOM causes the Plaza load to hang"

Handoff-quoted signal:
```
[jkr] SolidHeap 0x7fffe3605d30 OUT OF MEMORY: requiredSize=0x1950
      (size=0x1950 align=4) mFreeSize=0x0 span mStart=... mEnd=...
```

Backtrace from `SB_JKR_BT=1`:
```
JKRSolidHeap::allocFromHead
JKRSolidHeap::alloc
operator new  (0x59a92b)
JKRMemArchive::mountFixed +0x8c
TMarDirector::loadParticle  +0x54
TMarDirector::loadResource  +0xe0
TMarDirector::setupThreadFunc +0x9
```

The OOM is a warning, not a fatal:
`JKRSolidHeap::alloc` in `reference/sms/src/JSystem/JKernel/JKRSolidHeap.cpp:67`
already routes the null-return to `sb_jkr_host_alloc` (the tagged
host-overflow path in `reference/sms/src/JSystem/JKernel/JKRHeap.cpp`). So the
particle archive mount SUCCEEDS via host memory. `SolidHeap OOM` isn't the
blocker.

## Falsified: "the load stalls after NPCBoard"

Without `SB_FRAME_DUMP`, log ends at NPCBoard because that's the last thing
`OSReport`ed — the game then enters `TApplication::gameLoop → TMarDirector::direct
→ TPerformList → J3DShape::draw` at 99.4 % CPU on the game thread. Real
gameplay, silently rendering.

`SB_PRESENT_TRACE=1` no-dump run: **4140 present_hook calls in 25 s** (~165
present/sec). Not remotely stalled — just producing no log output because it's
running gameplay.

The 2 M+ host mallocs the `SB_JKR_DBG` run counted are per-frame render
allocations. Outstanding stays flat at 45 MB — matched by frees. Not a leak.

## Real bug: `SB_FRAME_DUMP_START=0` deadlocks the cooperative scheduler

Enabling `SB_FRAME_DUMP=1` (with default `START=0`) dumps frame 0 successfully
then hangs at 0 %  CPU. gdb of the game thread shows:

```
Thread 11 (game):
  sb_sched_drain_until_idle    (native/platform/os_impl.cpp:730)
  VIWaitForRetrace              (native/platform/vi_impl.cpp:70)
  JDrama::TVideo::waitForRetrace
  ← inside TApplication::proc, first post-frame-0 VI wait
```
Worker threads are all `OSReceiveMessage`-parked
(JKRAram / JKRAramStream / JKRDecomp / TDrawSyncManager / audioproc /
JUTException). `TMarDirector::setupThreadFunc` is NOT in the thread list — it
was never created because the game thread hasn't returned from its first
`gameLoop` after frame-0 present.

The first present is expensive: SDL3 GPU init, Vulkan pipeline compile, and
the actual clear+swap. That runs on the game thread inside `present_hook`
called from `VIWaitForRetrace`. During that time the game thread doesn't
progress; `setupThreadFunc` isn't spawned yet; drain sees no runnable workers
and waits — but no worker will run because none are unblocked. Circular.

## Working recipe (verified)

```
setarch -R env SUNBRIGHT_DISC=... SB_THP_FAST=1 SB_TURBO=1 \
  SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=1 SB_SCENARIO=0 SB_OWN_GXLIST=1 \
  SB_WATCHDOG_SECS=0 SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=1000 SB_FRAME_DUMP_MAX=4 \
  ./build-native/sms-boot
```

Result in ~25 s: `boot_1000.ppm` through `boot_1003.ppm`, each with
`scene_verts=280314 scene_batches=144 imm_tris=1588 imm_batches=11
(textured=4)` — populated Plaza scene. Present rate ~130 / s during the
window.

Screenshot: `scratch/screenshots/stage1_native_first.png`.

## Follow-up (for `plaza_sbs.sh`)

- Do NOT use `SB_FRAME_DUMP_START=0` for plaza — you'll trip the drain
  deadlock. Pick a late start (≥ 500 present frames) OR mirror `title_sbs.sh`'s
  `SB_SEL_DUMP_SETTLED=N` pattern (see `tools/render/title_sbs.sh:64`) once a
  plaza-appropriate settle predicate is added.
- The real underlying fix is a scheduler bug: the drain waits when NO runnable
  worker exists AND no not-yet-created worker will ever be scheduled. Named
  fix: teach `sb_sched_drain_until_idle` to return once every existing thread
  is parked (i.e. don't wait for absent-not-yet-parked workers). Not urgent
  for the sweep — the late-start recipe is the immediate unblock.

## TManhole follow-up (from Task 1)

The manhole cover renders as a static solid cover on Plaza. The
`TMapCollisionWarp` allocation + `vtable[2]` init at `0x8018d8bc` (RE'd from
`--disasm 0x801c1b4c`) is scoped OUT — Mario cannot yet fall down manholes.
When plaza gameplay needs the warp, port that too.

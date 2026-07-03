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

- `plaza_sbs.sh` landed (`176214d`). Uses `SB_FRAME_DUMP_START=3000`
  (NOT 1000 — that's still mid-arrival-cutscene, tiny content region, native
  mean=1.2). By 3000 gameplay has started (imm_batches 11→60, textured 4→52,
  content mean 12.5, 16535 unique colors, 11.76% non-zero pixels).
- Reuses `tools/render/title_overbright.py` for the Δ — that tool just
  pixel-diffs 2 PPMs, is Plaza-agnostic.

## First Plaza Δ (native boot_3007.ppm vs oracle plaza_gx_oracle.png)

```
  channel   native            oracle            delta(N-O)        std N / O
    R         14.3               2.7             +11.6            52.7 /  22.7
    G         14.1               3.0             +11.2            48.9 /  24.3
    B          9.0              42.3             -33.2            40.2 /  68.6
  channel_mean_delta:    18.7   (MISLEADING: cancels sky-vs-ground errors)
  mean_abs_pixel_delta:  28.5   (TRUE fidelity metric — MOVE THIS)
  std-preserving(additive)=False
```
4×4 signed delta grid + region |Δ| in
`scratch/screenshots/plaza_delta.txt`. Region hot-spots:
- Row 0 (sky-top): native +57R/+67G/+29B vs oracle 0. Native writing content
  where oracle has letterbox — aspect / viewport mismatch, OR native's HUD
  bleeds into top strip.
- Rows 1–2 middle cols: -114 to -151 in BLUE. Oracle has heavy blue content
  (sky + sea), native has ~none. Same class as the closed title-screen
  sky-missing bug — a sky/water render pass is not landing on the native
  present framebuffer.
- Row 3 col 3 (bottom-right): native +65B. HUD element (shine counter?) that
  oracle doesn't show at framedump_4340, OR different HUD state.

## First-visible-defect list (ranked by |Δ| magnitude)

Priority order for the plaza sweep, most-severe first:
1. **Sky/water rendering missing on native present.** The -151B and -114B
   central-region cells dominate the delta. Oracle blue = sky dome + Delfino
   waterway. Native captures scene geometry (280k verts, 144 batches) but the
   sky/water isn't reaching the presented framebuffer.
2. **Top-strip aspect/letterbox mismatch.** Native renders (57,67,29) content
   in a region oracle blacks out.
3. **Bottom-right HUD divergence.** +65B in row-3 col-3 → HUD element differs.
4. **Frame-time misalignment.** Native frame 3007 vs oracle framedump_4340 may
   not be at the same emulated moment (different fastboot settle times). Not a
   fidelity bug — a capture-methodology one. Both engines are ~settled in
   plaza gameplay by 30s fastboot; small phase difference expected.

Attack in RE-first order per the 2026-07-03 HARD RULE: RE which actor draws
each defect (sky-dome BMD? sea `TMapObjWave`?), port PC-native under
`SMS_NATIVE_PLATFORM` via SDL3 GPU. Do NOT re-emulate GX/TEV faithfulness.

## Scheduler deadlock — FILED, not landed

Filed as separate arc. Initial hypothesis ("drain waits for absent worker")
was **wrong** on inspection: `sb_sched_drain_until_idle` (os_impl.cpp:730)
already breaks its loop when no OTHER thread is runnable, AND `park_on`
(os_impl.cpp:256) hands the baton back to `g_idle_waiter` when workers park.
Given both, the deadlock at `sb_sched_drain_until_idle → cond_wait` with all
workers parked in `OSReceiveMessage` shouldn't happen. Real cause needs
direct instrumentation of the drain loop (log each pick_next result +
runnable state per iteration) — not a 30-min fix.

Immediate unblock (late-start recipe) is committed; no urgency to chase this
now. When picked up: add `SB_SCHED_TRACE=1` to log per-iteration state in
drain, reproduce with `SB_FRAME_DUMP_START=0 SB_STAGE=1`, name the specific
stuck condition.

## TManhole follow-up (from Task 1)

The manhole cover renders as a static solid cover on Plaza. The
`TMapCollisionWarp` allocation + `vtable[2]` init at `0x8018d8bc` (RE'd from
`--disasm 0x801c1b4c`) is scoped OUT — Mario cannot yet fall down manholes.
When plaza gameplay needs the warp, port that too.

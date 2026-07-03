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

## CORRECTION (2026-07-03, after probing Defect #1)

The Δ=28.5 headline is **capture-methodology, not fidelity**. Direct probes:

**Probe A — is sky.bmd captured?** With `SB_STAGE=1 SB_PASS_VERTS=1` (no
`SB_OWN_GXLIST`) the pass breakdown shows `sky +2790 verts +5 batches` (drive_sky's
hand-driven path). Under `SB_OWN_GXLIST=1` the master perform list captures the
scene via a different path (total 280k verts) but doesn't tag sky separately.
Either way, batches EXIST.

**Probe B — does the native sky-paint mechanism apply on Plaza?** Added a
`SB_FORCE_NATIVE_SKY=1` env-gated override (`extern "C" bool sb_native_sky_active`
in `native/src/scene_drive.cpp:1180`) that flips the gate ON for any active stage.
Result: **native output BYTE-IDENTICAL** — same mean (14.3,14.1,9.0), same 11.37%
non-zero fraction, same bbox. The paint mechanism (`sb_native_sky_paint` via
`draw_seg` in `sms_boot_present.cpp:898`) is NOT the missing thing. **Probe
reverted** (no diff shipped).

**Probe C — what's actually in each image?** Bounding-box + row/column band
inspection:
- Native (`boot_3001.ppm`): content spans full `(15,13)→(619,460)` bbox but
  density is only **11.37% non-zero**. Sparse scattered content over the entire
  640×480 area. NOT a small centered content region.
- Oracle (`plaza_gx_oracle.png` = framedump_4340): content ONLY in a **401×216
  CENTERED window** `(120,150)→(521,366)`. Blue-heavy (R=0, G=0, B=94-112 in
  center rows). Corners and edges pure black. **This is a LETTERBOXED
  cutscene**, not plaza gameplay.

**Probe D — does native ever escape this state?** Tried
`SB_FRAME_DUMP_START=8000` (much later than 3000): **byte-identical output**
(mean 14.3/14.1/9.0, 11.37% nonzero, same bbox). Native has settled into a
static state and is not advancing to actual plaza gameplay.

## Revised defect list

Both engines are stuck at pre-gameplay states, but DIFFERENT ones:
1. **Oracle at framedump_4340 shows a letterboxed cutscene (probably the shine-
   sprite delivery intro), NOT plaza gameplay.** Its 401×216 central content
   with heavy blue is the sea/sky visible mid-cutscene.
2. **Native at frame 3000..8000 shows a static, low-density (11.37% nonzero)
   scene** — 280k scene_verts captured but most fragments discarded.
   Content spans the whole framebuffer with sparse coverage, not a cutscene
   letterbox. Native's boot has stabilized on some non-gameplay state and is
   spinning there.
3. The 28.5 |Δ| measures **which non-gameplay state each engine got stuck in**,
   NOT rendering fidelity. Actionable fidelity work requires BOTH engines to
   reach real plaza gameplay first.

**Next-arc gate**: before Defect #1 can be attacked on its own merits, we need
to make both engines reach POST-CUTSCENE plaza gameplay. Options: skip the
delivery cutscene (find + trigger its skip button, same as the title's Start
press), OR pick a savefile-based capture that lands directly in gameplay
(scratch/fresh_plaza.sav or similar per memory `abshot2-gx-oracle-empty`).
Once both engines are at a real gameplay frame, RE-CAPTURE Δ. Only THEN can
the sky-dome / water / HUD defect ranking be meaningful.

The sky-dome port work (title-class native paint) is NOT falsified — it's
premature to invoke without a real gameplay comparison.

## Gameplay comparison LANDED (2026-07-03, commit 9036bf2)

`plaza_sbs.sh` now loads `scratch/freeroam_plaza.sav` on the oracle side via
`/loadstate` (probe endpoint, CPU-thread State::LoadAs). Native side
unchanged — its SB_STAGE=1 fastboot already flag-skips cutscenes to
APP_STATE_GAMEPLAY on frame 0 (Application.cpp:572, sets
`getBool 0x30009/B/C/D → true`, mCurrArea=(1,0,0)).

Oracle now shows real plaza gameplay: **mean (81.4, 91.2, 96.5)**,
nonzero=99.96%, bbox = full frame. Bright sky + plaza scene.

Native's SB_STAGE=1 output unchanged (mean 14.3/14.1/9.0, 11.37% nonzero) —
that IS its current plaza gameplay render. It's not "stuck in a state" — it
IS the plaza gameplay render, just sparse and dark.

**Real baseline Δ = 88.2** (was fake 28.5 vs cutscene). All 4×4 signed-delta
cells are NEGATIVE → native uniformly darker than oracle everywhere.

## Revised (real) defect list — Plaza gameplay

Ranked by region |Δ| magnitude, most-broken first:

  Region |Δ| grid:
    82.1   68.3   69.1   82.7    <- top strip (sky area)
    88.2   86.9   78.7  100.5    <- upper scene
    97.4  102.6   91.6   88.0    <- mid scene (worst)
    95.5   99.3  100.9   80.1    <- bottom (ground/HUD)

1. **Mid-scene (rows 1–2, cols 1–2): -100 across all channels.** The plaza
   ground / buildings / palm trees are essentially not appearing on native.
   This is not a single-actor defect — it's a broad rendering failure.
2. **Top-right corner (r0c3, r1c3): -73 blue / -104 all channels.** The sky
   dome + horizon area, missing.
3. **Bottom-right (r3c3): -56R/-76G/-69B.** The least-broken region — HUD
   partially rendering here.
4. **Sky top-left (r0c0..c2): -18 to -57.** Less severe than mid-scene,
   suggests the sky-clear is partially working.

**Pattern**: native's mean (14.3, 14.1, 9.0) has near-equal R and G with less
B — grayscale-ish, no color, low brightness. Oracle's (81.4, 91.2, 96.5) has
progressively more B — sky-tinted, bright. Native is drawing DARK grays where
oracle has bright color. Consistent with:
- Lighting / material color not applied
- Or fragment shading discarding most output
- Or blend mode producing near-zero alpha everywhere

Actionable next probes (for the follow-up arc):
- Compare native's TEV/lighting pipeline to what a settled plaza would
  compute. Native's std of 40-52 across channels vs oracle's 26-32 says
  native has HIGH variance but LOW mean — flat-shaded scattered points, not
  smoothly-lit surfaces.
- Bisect: set `SB_SKIP_KEY=<batch_hash>` on individual scene batches to see
  which ones actually contribute pixels vs which are silently discarded.
- Check the native pass-verts distribution at gameplay (SB_PASS_VERTS=1):
  the sky:2790 / scene-perform:126321 / chr:4044 breakdown from the earlier
  probe was captured pre-cutscene-end; re-capture at settled gameplay to see
  what draws.

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

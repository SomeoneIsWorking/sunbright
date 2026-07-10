# 2026-07-10 (continuation) — Black title backdrop ROOT CAUSE FOUND + FIXED: JRenderer.cpp was excluded from the native build, so every J3D material's BP TEV-order register was silently dropped

## Task

Phase-4 perspective-bound Sky Xlu/MapOpa draws are PROVEN to emit
(`2026-07-10_phase4_does_emit_uncapped_frame_dump.md`, stream idx ~198-225) but the
backdrop still renders pure black. Determine why, with no speculative fixes.

## Step 1 — trustworthy magenta re-test (SB_FORCE_COLOR)

Prior "zero magenta pixels" result (`2026-07-10_title_backdrop_black_verdict.md`) is
superseded — this session's clean re-test, isolating buffers by `SB_SKIP_MARK` (which
filters by draw-identity marker, not by phase, so BOTH phase-1 ortho and phase-4
perspective draws of a given buffer pass through):

- `SB_FORCE_COLOR=1` + `SB_SKIP_MARK=` everything except `MapOpa` (present retrace 2000,
  paced): `scratch/frames_title/mapopa_forcecolor_v2.png` — **0 magenta pixels anywhere**
  (raw pixel value at every sampled point is (0,0,0), confirmed via direct pixel read, not
  the image-preview renderer which visually mis-rendered both this and the all-black case
  as white — ground truth is the raw BGRA bytes, not the viewer).
- `SB_FORCE_COLOR=1` + `SB_SKIP_MARK=` everything except `Sky Xlu`: `scratch/frames_title/skyxlu_forcecolor_v2.png`
  — **100% of the 1280x960 framebuffer is exactly (255,0,255)**. Sky Xlu alone covers
  every pixel of the viewport.
- `SB_FORCE_COLOR=1`, no skip list at all (every draw, whole scene):
  `scratch/frames_title/allforce_v2.png` — **100% magenta**, sampled every-other-pixel,
  zero non-magenta samples anywhere including the logo silhouette region.

**Verdict: branch 2a.** The geometry rasterizes across the ENTIRE screen — this is not a
coverage/clip/raster-state problem. Real (non-forced) rendering shows black, so the
defect is downstream: color/TEV output resolves to 0 despite full coverage.

(Aside, not this session's target but worth recording: MapOpa's own geometry produces
zero coverage in isolation at this exact frame/window — separate from the black-backdrop
question since Sky Xlu alone already accounts for 100% coverage. Not investigated further
here.)

## Step 2a — TEV/state dump at the exact retrace-2000 frame

`SB_DRAW_DUMP=1 SB_DRAW_DUMP_AFTER=2000 SB_DRAW_DUMP_FRAME=2000 SB_TEV_DUMP=1` (paced),
cross-checked against `[draw-dump-frame]`'s exact-retrace-match stream indices (198-206 =
Sky Xlu phase-4, 207-225 = MapOpa phase-4, matching the phase4 journal's table exactly).

Every sampled Sky Xlu AND MapOpa draw — phase-1 ortho ghost pass and phase-4 perspective
pass alike — shows **`texMap=255` on every TEV stage** (`GX_TEXMAP_NULL`), including the
cloud-quad draws that per `2026-07-07`'s Sky.bmd shape inventory are known to carry real
16x16/64x64/128x128/256x256 textures. `tex0=58x68` printed alongside is stale garbage
(the printf reads `textures[0]` unconditionally; with `texMapId=255` that slot is never
the one actually referenced). MapOpa's stage-0 color pass is `a=ZERO b=TEXC c=RASC
d=ZERO op=ADD` → `out = RASC * TEXC`; with `TEXC` forced to 0 (aurora's documented,
HW-faithful behavior for a NULL texmap reference — the already-flagged CLAUDE.md open
item "aurora emits 0 per GC HW" on NULL texMap), **output is 0 regardless of RASC/lighting**.
This is the first, and sufficient, divergent field: real GC materials must be binding a
valid, non-NULL texMap for these stages (confirmed indirectly: the shapes are known-textured
per the inventory, and per Step 3 below the game genuinely tries to pass a real texMap
value — it just never reaches the BP register).

## Step 3 — root cause, file:line

`JRNISetTevOrder` (`reference/sms/src/JSystem/JRenderer.cpp:551`) is the function
`J3DMaterial::entry()` (`reference/sms/src/JSystem/J3D/J3DGraphBase/J3DMaterial.cpp:318-462`)
calls to write the GX TREF/BP register (texMap, texCoord, tex-enable bit, channel) for
every TEV stage of every material, every draw. **`sms-boot/CMakeLists.txt` (pre-fix,
lines 56-57) excluded `JRenderer.cpp` from the native build entirely**, under a comment
block reasoning "pure PPC-hardware TUs, no meaningful native impl" that applied correctly
to `JUTException.cpp`/`JUTDirectPrint.cpp` but was wrongly extended to `JRenderer.cpp` —
which is portable C++ holding the real J3DGD*/JRNI* BP-register-writing bodies, not
hardware access.

With `JRenderer.cpp` excluded, every call to its 21 externally-visible functions —
`JRNISetTevOrder`, `J3DGDSetChanCtrl`, `J3DGDSetTexCoordGen`, `J3DGDSetFog`,
`JRNSetIndTexOrder`, etc — linked instead against **empty no-op stub bodies** in
`sms-boot/runtime/sdk_stubs.cpp:350-372` (`void JRNISetTevOrder(...) {}` and 19 siblings).
The BP TREF register is simply never written. Aurora's `gx.hpp:107` defaults every
`TevStage::texMapId` to `GX_TEXMAP_NULL` at struct construction and nothing ever
overwrites it — so every material's every TEV stage stays permanently NULL-textured,
which is exactly the `texMap=255` universally observed in Step 2a.

**This regression was self-inflicted and already half-fixed once**: commit `4ed2ac15`
("Build JRenderer.cpp natively — the J3DGD*/JRN* writers were no-op stubs", 2026-07-08)
diagnosed this exact class of bug and patched `JRenderer.cpp`'s body for native/LP64
compilation (extern decls for `GXTexImage*Ids`/`GXTexTlutIds`, pointer-cast fixes) — but
**never removed the `sms-boot/CMakeLists.txt` exclusion filter**, so the fixed source was
still never compiled in and the stub bodies kept silently winning. The commit message's
own diagnosis ("channel NULL -> RASC=0 -> black geometry (title sky/map)") is this exact
bug, just never actually wired up.

## Fix (root cause, not a bandaid)

1. `sms-boot/CMakeLists.txt`: removed the `JRenderer\.cpp$` exclusion regex; documented why
   in-place (it's portable C++, not PPC-hardware access).
2. `sms-boot/runtime/sdk_stubs.cpp`: removed the 20 now-duplicate no-op stub definitions
   for the J3DGD*/JRN* functions (kept the unrelated `JPABaseField`/`_GXFogAdjTable`
   forward decls other stubs in the file still need) — `JRenderer.cpp`'s real bodies now
   provide these symbols; leaving both would be a duplicate-definition link error.
3. Reconfigured (`cmake -B build`) + rebuilt `sms-boot` clean.

## Verification

- `SB_TEVORDER_DBG=1` (existing diagnostic, previously never fired — confirmed the
  `[tevorder]` format string was **absent from the compiled binary entirely** pre-fix,
  i.e. the function had truly never been called/linked-in as the real body): post-fix,
  fires immediately with real values, e.g. `[tevorder] n=1 stage=0 texCoord=0 texMap=0
  colorChan=4` — genuine, non-NULL texmap indices now reach the BP writer.
- `SB_DUMP_FRAME=scratch/frames_title/fixed_2000.raw SB_DUMP_FRAME_AFTER=2000` (paced,
  same present used throughout this investigation): **the black backdrop is gone.**
  `scratch/frames_title/fixed_2000.png` shows a full blue sky, white clouds, and sun-glare
  rays behind the "Super Mario Sunshine" logo card — matching the real title screen's
  known appearance. Pre-fix pixel at (5,900) was `(0,0,0)`; post-fix it's `(183,218,244)`
  (sky blue).
- `ctest` in `build/`: 27/27 buildable tests pass (4 additional entries reported "NOT_BUILT"
  are aurora's own gtest binaries, gated behind a separate CMake option not part of the
  `sms-boot` target — pre-existing, unrelated to this change). `platform_mtx_lookat_test`
  (the MTX sign-convention regression guard from the same investigation arc) still passes.
- No oracle pixel-diff percentage taken this session — the cached Dolphin oracle capture
  in `scratch/oracle/` (`title_gx_oracle.png`) is the file-SELECT screen, not this exact
  title-logo-card frame; a same-frame oracle diff needs a fresh pinned Dolphin capture,
  named here as follow-up rather than fabricated.

## Residual/follow-up (not fixed this session, named per no-bandaids rule)

- MapOpa's own geometry produced zero force-color coverage in this session's Step-1
  isolation test at this exact frame — separate from the backdrop question (Sky Xlu alone
  already explains 100% coverage) but worth checking after this fix lands, now that MapOpa's
  materials also get real TEV/channel state for the first time.
- The previously-recorded raster-state divergences
  (`2026-07-10_world_pass_raster_state_comparison.md`: MapOpa `z_func`/`blend_enable`,
  dome `color_update`) were captured against the OLD (broken) TEV state and should be
  re-diffed against retail now that materials carry real channel/texture config —
  `J3DGDSetChanCtrl` (also previously stubbed, now real) directly affects `matSrc`/`ambSrc`/
  lighting-enable, which feeds exactly the raster-state comparison's channel columns.
- CLAUDE.md's "NULL-texMap TEV callsite question... aurora `26d5a7b` emits 0 per GC HW"
  open item is now answered for the title-Sky/MapOpa case: the NULL was never a retail
  fact nor an aurora bug — it was this build-exclusion silently discarding the game's real
  BP writes. Aurora's TEXC=0-on-NULL behavior is correct and should stay; other NULL-texMap
  sites (if any remain elsewhere in the game) should be re-examined now that this class of
  bug is confirmed to exist, rather than assumed retail-faithful.

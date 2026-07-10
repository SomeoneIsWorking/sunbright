# 2026-07-10 — Full stub-body audit: silent no-ops → loud FAIL-FAST (JRenderer lesson)

## Motivation

`sms-boot/runtime/sdk_stubs.cpp` carried 20 silent no-op bodies for JRenderer.cpp's
exports because that file was wrongly excluded from the native build (fixed today,
`eefa112`). `JRNISetTevOrder` as a no-op left every TEV stage on GX_TEXMAP_NULL,
rendering the whole 3D scene black for days of investigation before the excluded-file
bug was found. **A stub that produces valid-but-wrong state silently is the worst
failure shape** — nothing crashes, nothing warns, the output just looks like a
different, unrelated bug. This entry is the full sweep for more of the same shape
across every stub body in the native runtime, per CLAUDE.md FAIL FAST.

## Method

1. Inventoried every function body in `sms-boot/runtime/sdk_stubs.cpp` and all
   `sms-boot/boot_stubs/*.cpp` (+ the `.S` weak-dtor scaffold).
2. Classified each as:
   - **(a) INTENTIONAL SEAM** — the no-op IS the correct native semantic (single-thread
     doctrine, audio-silence gap, synchronous-render doctrine), or the CMake exclusion
     comment already documents why.
   - **(b) SILENT LANDMINE** — a caller assumes the operation happened (state mutation,
     render, collision response, AI, a data producer) and gets a fabricated answer
     instead. Converted to a one-time `[STUB-CALLED] <name> — unported, output will be
     wrong` `OSReport` (see `sms-boot/boot_stubs/stub_trace.h`, macro `SB_STUB_HIT`) —
     loud but non-fatal, since these sit on gameplay classes (Enemy/MoveBG/etc.) not yet
     reached by the title/file-select boot target. Where a wrong return value would
     corrupt caller-visible state rather than just "this actor does nothing" (the
     existing `TMapObjTree::initMapObj` precedent), `OSPanic` is used directly instead —
     judged per function.
   - **(c) DEAD** — unreferenced. None found in this sweep (all stub bodies have a live
     caller through the decomp's vtables/nerve tables).
3. Audited every CMake `list(FILTER ... EXCLUDE)` on `reference/sms` sources for the
   same failure shape (a later fix landing but the exclusion staying).
4. Rebuilt, ran ctest, ran the title boot + save-select pad-script repro, collected every
   `[STUB-CALLED]` line that fired.

## Classification counts

| File | Bodies | (a) Intentional | (b) Landmine → loud | (c) Dead |
|---|---|---|---|---|
| `sdk_stubs.cpp` | ~85 | ~78 | 7 (GXReadPixMetric, GXPeekARGB, 5 `sb_*` signature-mismatch fns) | 0 |
| `boot_stubs/classes2_stubs.cpp` | 66 | ~13 (ctors/dtors) | 53 | 0 |
| `boot_stubs/enemy_stubs.cpp` | ~30 | ~15 (ctors/data) | 15 | 0 |
| `boot_stubs/movebg_stubs.cpp` | ~110 | ~68 (ctors) | 42 | 0 |
| `boot_stubs/ring3_stubs.cpp` | ~150 | ~5 (already-real bodies: TGuide::load) | 145 | 0 |
| `boot_stubs/ui_map_stubs.cpp` | ~25 | ~14 (ctors/data) | 11 | 0 |
| `boot_stubs/unresolved_stubs.cpp` | ~140 | ~70 (`theNerve()` accessors, DSP/PPC intrinsics, `SpcTrace`, JAS probe) | 70 (`TNerve*::execute` + 7 matrix/effect helpers) | 0 |
| `unresolved_stubs_asm.S` | 21 (weak dtors) | 21 | 0 | 0 |

**Total: 329 landmines converted to loud-once stubs** (327 via mechanical instrumentation
+ 2 hand-edited: `GXReadPixMetric`/`GXPeekARGB` in `sdk_stubs.cpp`), plus 5 `sb_*` bug
fixes (below) and 7 matrix/effect free-function landmines.

## The one real bug this audit found: 5 signature-mismatched `sb_*` stubs

`sdk_stubs.cpp` defined `sb_boot_drive_scene`, `sb_boot_request_dump`,
`sb_camera_view_settled`, `sb_gx_get_color_alpha_update`, `sb_gx_get_projection` with
signatures that had drifted from the `extern "C"` declarations at their actual callsites
(`CardLoad.cpp`, `MarDirectorDirect.cpp`, `J3DDrawBuffer.cpp`, `J3DModel.cpp`) — worst
case, `sb_gx_get_color_alpha_update` was declared `void(int*, int*)` at the callsite but
DEFINED as a zero-arg `int()`. `extern "C"` linkage resolves by name only across
translation units, so this linked cleanly and ran with the caller's real arguments
silently discarded — undefined behavior, and exactly the JRenderer failure shape (a
linkable stub whose signature lies about what it does). All 5 sit behind retired-Path-B
or env-gated `SB_*_DBG` diagnostics, never the render hot path, so this was latent rather
than actively wrong today — but it's the same bug class. Fixed to the real callsite
signatures; kept as no-ops (documented per-function in `sdk_stubs.cpp`) since none of the
5 do anything render-critical.

## CMake exclusion audit (`sms-boot/CMakeLists.txt`)

| Exclusion | Verdict |
|---|---|
| `dolphin\|PowerPC_EABI_Support\|TRK_MINNOW_DOLPHIN` | Still justified — GC SDK/PPC CRT, Aurora supplies the equivalents. |
| `OdemuExi2\|dspproc\|dsptask\|osdsp` | Still justified — PPC-hardware/DSP register access. |
| `GC2D/hx_wiper.c` | Still justified — real port lives in `runtime/hx_wipe.cpp` instead. |
| `THPPlayer/` | Still justified — movie-skip seam in `sdk_stubs.cpp`. |
| `JASVload.cpp` | Still justified — replaced by `sms_boot_audio.cpp`'s BARC loader. |
| `JUTException.cpp` / `JUTDirectPrint.cpp` | Still justified — verified by reading both sources: genuine PPC crash-handler/console code (`OSContext` register dump, direct-framebuffer console writes via `JUTExternalFB`), no meaningful PC translation. NOT the JRenderer shape. |
| `GDLight.c` (sms-gd target) | Still justified — duplicates aurora::gd's own `GDLight.cpp`. |
| **`JRenderer.cpp`** | **Was the stale exclusion (fixed today, `eefa112`, before this audit started).** Confirmed already removed from the exclusion list. |
| **`JPAField.cpp`** | Also already fixed (per the CMakeLists comment) — confirmed not present in the exclusion list; comment explains the `JPABaseField` ctor `JSUPtrLink` init bug this caused. |

No newly-stale exclusions found this round — the two real ones (JRenderer, JPAField)
were already fixed earlier today.

## Verification

- **Build**: `cmake --build build --target sms-boot -j$(nproc)` — clean.
- **ctest**: 27/27 buildable tests pass (4 `_NOT_BUILT` entries are stale pre-existing
  test-registration gaps, not new failures — matches the pre-audit baseline exactly).
- **Title boot** (`SB_HEADLESS=1 SB_STAGE=15 SB_TURBO=1`, frame dump at present-frame
  600): 99.6% non-black pixels, zero `[STUB-CALLED]` lines, zero `OSPanic`.
- **Save-select repro** (`SB_PAD_SCRIPT="250:START 282:- 500:START 532:-"`, dump at
  frame 900): 100% non-black, zero `[STUB-CALLED]`, zero `OSPanic`.
- **Gameplay fastboot** (default `./run.sh`, no `SB_STAGE`): fires
  `TMapObjBase::loadBeforeInit`, `TMapObjBase::getHitObjNumMax` (now loud, previously
  silent), then hits the **pre-existing** `TMapObjTree::initMapObj` `OSPanic` — unchanged
  behavior, confirms the new loud stubs don't block or alter boot progress versus before
  this audit.

## Fired-stub inventory (the honest unported-surface worklist)

On the title and file-select paths: **none** — no Enemy/MoveBG gameplay class is
instantiated before Delfino gameplay starts, so none of the 329 newly-loud landmines
fire there. This is the expected shape (task target is boot-order fidelity, not
gameplay), and it's now provable rather than assumed.

On the gameplay fastboot path (informational only, not this session's target): the
`TMapObjBase` landmines above, immediately gated by the existing `TMapObjTree::initMapObj`
panic (a pre-existing, correctly-named worklist item — MapObjTree needs a decomp port
before boot can progress past it). The remaining 327 landmines are worklist entries that
will surface their own `[STUB-CALLED]` lines the first time boot reaches each class —
that log output, going forward, IS the porting worklist.

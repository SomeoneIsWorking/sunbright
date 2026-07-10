# 2026-07-10 — Title stage-15 black 3D backdrop: verdict (case c-raster)

## Confirmed stable-window frame

Paced/turbo boot to `SB_STAGE=15`, dumped with `SB_DUMP_FRAME` at
`VIGetRetraceCount()`-equivalent present ≈2000 (see below for why the
window isn't a fixed wall-clock offset). `scratch/frames_title/stable1.png`
shows the full "Super Mario Sunshine" logo + partial "PRES[S START]" text +
"©2002 NINTENDO", i.e. squarely inside the held title-logo card. **Everything
outside the logo/text is pure black (0,0,0).** A sweep from present 600
through 3200 (`SB_DUMP_FRAME_EVERY=200`) shows the same thing at every
sampled point except the fade transitions (~1200-1600, ~2600-3000, all-white
motion-blur) — the 3D backdrop is black at every held frame of the title
card, not just a mistimed one.

Present ~3200 in this sweep is **fully black including the logo** — first
sign of the fade into whatever comes next (the attract-mode handoff). Shortly
after, the process SIGSEGVs inside `MSound::exitStage → JAIBasic::stopSeq →
JAIBasic::releaseControllerHandle` (main-thread stack trace, not a signal
artifact — SIGKILL can't produce a destructor unwind). This is a **separate,
real regression** blocking anything past the title card (attract movie, later
demo cycling) — not investigated further per scope (diagnostic-only), but
it means `SB_STAGE=15` runs currently cannot survive past ~present 3200-3500
without crashing. Named here so it isn't re-discovered as "mysterious hang."

## Root cause: NOT TEV/color — every 3D draw carries the wrong projection + viewport

Added `SB_NDC_PROBE_AFTER=<retraceCount>` and `SB_DRAW_DUMP_AFTER=<retraceCount>`
(aurora commit 8a05460) to window the existing NDC-probe/draw-dump instruments
to an exact present instead of a whole-run print budget / draw-index guess —
both were unusable at this frame before (see commit message). With the window
aimed at present ~2000:

- **Every single 3D draw this frame — `DrawBuf Sky Xlu`, `DrawBuf MapOpa`,
  `DrawBuf MapXlu`, `DrawBuf Mirror Opa` alike — shares one identical bound
  state**: `proj=ORTHOGRAPHIC`, projection diagonal
  `[0.0045, -0.0031, -0.5, -0.5]`, and viewport/scissor **`(0,0 640x448)`**.
  The render surface is 1280x960 (native 512x384 × 2.5 scale, per boot log
  `Using framebuffer size 1280x960 scale 2.5`) — 640x448 matches *neither*
  the native resolution nor the scaled target. One global (wrong) camera
  state is bound for the whole 3D pass, not a per-buffer issue.
- That orthographic scale (diag ≈0.0045/0.0031) maps a **local coordinate
  magnitude of ~±222 to the ±1 NDC range**. Actual per-vertex camera-space
  positions (`mv`) for Sky/Map/Mirror content run into the **thousands**
  (e.g. MapOpa `mv=(892, 2285, 2389)`, Sky Xlu `mv=(-68463, 228578,
  -142089)`). Every coordinate this far outside the calibrated range lands
  at `ndcX`/`ndcY` in the 2-300+ range — nowhere near the `[-1,1]` clip
  square.
- Sampled over the stable window: **Sky Xlu 117/117, MapOpa 205/205, Mirror
  Opa 72/72 draws fully outside NDC XY bounds (`inXY=0`)**. Only `MapXlu`
  had any partial containment (2 of 8 draws, `inXY=1` — a handful of
  vertices land in-frame by coincidence of translation). `wneg` (behind
  camera, `clip.w<=0`) was 0 throughout — this is not a behind-camera
  problem, it's a wrong-scale/wrong-type projection problem.

This reconciles the earlier "contradiction" (a prior, unwindowed NDC sample
had reported both fully-behind-camera and fully-visible MapOpa draws): that
sample was not actually the confirmed stable window (see commit 8a05460's
motivation) — it mixed frames from elsewhere in the boot. At the verified
present, MapOpa (and everything else 3D) is uniformly out of bounds.

## Rasterization-reach test (case-c bisection)

`SB_FORCE_COLOR=1` (bypass TEV, every fragment → magenta) combined with
`SB_SKIP_MARK="Sky Xlu,MapXlu,Mirror,LensFlare,TLightDrawBuffer"` (strip every
3D draw-buffer except MapOpa) at the same present:
`scratch/frames_title/mapopa_forcecolor.png` — **zero magenta pixels outside
the logo silhouette** (the logo itself isn't marker-filtered here and turns
magenta too, as expected/documented for `SB_FORCE_COLOR`'s existing global
scope). MapOpa contributes **no pixels at all**, forced-color or not.

**Verdict: case c-raster.** The geometry never reaches rasterization — it's
discarded by the wrong-scale orthographic projection before the fragment
stage runs at all. This is not a TEV/blend/color defect; forcing the
fragment output to a fixed color changes nothing because the primitives
don't cover any framebuffer pixels in the first place.

## Narrowed next arc

Find WHO binds this orthographic projection + 640x448 viewport for the
Sky/Map/Mirror draw buffers at title, and why the real perspective camera
projection (the one that should govern the panning island backdrop) never
gets bound before them. Candidates to RE next: whatever GX call sequence
title's `TSky`/`TMap`/mirror draw-buffer `perform()` chain issues right
before invoking `J3DDrawBuffer::draw` — either a `GXSetProjection` call is
being skipped/short-circuited for this pass, or the wrong cached
`TCamera`/`J3DSys` projection object is current when the buffers flush. The
shared exact-same projection+viewport across FOUR unrelated draw-buffer
names (Sky Xlu, MapOpa, MapXlu, Mirror Opa) says this is a **global
current-projection state bug**, not a per-object one — look at whatever sets
`g_gxState.proj`/viewport once per frame for stage 15's 3D pass, not at any
one buffer's material.

## Tooling landed

- `SB_NDC_PROBE_AFTER=<retraceCount>` (aurora `lib/gx/command_processor.cpp`):
  gates the existing per-vertex NDC transform probe to fire only once
  `VIGetRetraceCount()` clears the threshold, instead of burning its
  400-print whole-run budget on unrelated earlier frames.
- `SB_DRAW_DUMP_AFTER=<retraceCount>`: same present-based gate for
  `SB_DRAW_DUMP`'s per-draw identity/state dump, replacing the
  draws-per-frame-heuristic draw-index window (unreliable once a single
  marker's packet count is in the hundreds).
- Both read `VIGetRetraceCount()` via a weak `extern "C"` declaration +
  `sb_gx_vi_retrace_count()` helper (game-side symbol,
  `sms-boot/runtime/sdk_stubs.cpp`, advanced once per `sb_frame_present`) so
  aurora's standalone unit tests still link without the game.
- Practical note: game log lines containing raw Shift-JIS bytes (perform-list
  NameRef names) break plain `grep`'s line matching (silent, no error) —
  **use `grep -a`** on every sms-boot log, per the harness's own hard rule.

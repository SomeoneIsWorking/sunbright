# interp60 session handoff (2026-06-13) — read this first to continue

State of the 60fps interpolation work, the two open frontiers, and exactly where to pick up.
Everything below is committed + pushed (parent `main` + the Dolphin fork branch `sunbright`).

---

## ⛔ USER DIRECTIVE — NO SKIPS, EVER (top priority)

**There must be NO skips and NO skip reasons under ANY circumstances.** Every gameplay
frame gets a real present AND an in-between present — a perfectly clean `R,B,R,B,R,B…`
stream at 60fps, no exceptions, in every scene/state/load condition. This is non‑negotiable:

- The `skips(rate / nodir / full)` counters in `/interp60` must all be **0** — always. Today
  `skip_full=9` (from boot: `!g_gfx_valid` during startup). That is still a skip → must be eliminated.
- The present **cadence doublings** must be **0%**. Today it is **~13%** even with no capture
  overhead (`/interp60` → `CADENCE (lifetime, no readback)`), i.e. ~1 in 8 presents is a
  doubling (`RR` or `BB`) = a 60Hz hitch. Long `RRRRR` runs appear under load. **All of this
  must go to zero.** "Mostly smooth" is not acceptable — the requirement is *never* skip.

Do NOT treat any skip as a tolerable edge case, a transition exception, or a perf fallback.
If a condition currently forces a skip (1‑field frame, not‑fresh, gfx‑not‑valid, VI field
timing), the job is to make interpolation work in that condition too — own it, don't skip it.

---

## What this session accomplished (done, committed)

### 1. Motion-interp reached FULL coverage — fixed "looks 30fps / camera not 60fps"
`runtime/overrides/interp_capture.cpp` — the render-only LoadIndexedXF hook interpolates each
object's pos-matrix between frame N‑1 and N (read-only; writes only XF/GPU, never guest RAM).
Three bugs fixed, in order:
1. **Cross-object mispairing** (draw-order pairing) → vertex explosion. Fixed: identity pairing
   from the model registry (each `J3DModel`/`SDLModel`'s own `mDrawMtxBuf`).
2. **Fresh-model garbage N‑1** → explosion. Fixed: `g_prev_registry` freshness gate (only
   interpolate models present BOTH frames; no N‑1 = nothing to interpolate).
3. **THE BIG ONE — coverage was ~9%** (`hits=100/misses~1300`). Root cause (found via the
   env-gated miss classifier `SUNBRIGHT_DBG_XF` → `REG[0]/REG[1]/UNKNOWN`): the GX stream loads
   `mDrawMtxBuf[0][view]` for ~90% of objects and `[1][view]` for the rest — the `[0]`/`[1]`
   double-buffer **phase is mixed** (variable `swapDrawMtx` count/frame from multi-pass/
   multi-view). Fixed: map **both directions** `a↔b` per model+view; the two buffers are always
   `{this frame, last frame}`, so whichever the stream loads is current and the map resolves to
   the other = prev. Also map all views (stop at first non-heap slot = exact view count).
   **Result: `hits=1395, misses=0`** — the whole scene + camera interpolate (camera bakes into
   every `drawMtx = view × node`). Decomp truth: `J3DModel.hpp` `mDrawMtxBuf[2]` @+0x60 (`Mtx**`,
   per-view), `mCurrentViewNo` @+0x7C; `getDrawMtxPtr` returns `[1][view]`; `swapDrawMtx` swaps
   `[0][v]↔[1][v]`; buffers are `new(0x20) Mtx[]` (0x20-aligned).

### 2. `/verify` — visual midpoint verification tool (new, durable)
- **Fork** (`externals/dolphin`): `Present.cpp` `ViSwap` does an optional synchronous XFB readback
  per unique present (`Presenter::SbCaptureXFB`, gated by `sb_capture_frames`), tagged by
  `xfb_addr` (real vs in-between's alt `^0x400000`), delivered to runtime hook
  `sb_slot_frame_captured` (`Common/SunbrightHooks.h`). Also cheap always-on cadence counters
  `g_sb_cadence_alt` / `g_sb_cadence_dbl` (no readback).
- **Runtime**: `runtime/overrides/interp_verify.cpp` — downsamples to a 64×36 luma ring AND dumps
  every armed frame full-res to `scratch/verify/s<unixtime>/fNNN_<real|btwn>_<addr>.ppm`. `/verify?n=K`
  arms (small K blocks+reports; large K arms and returns — poll `/verify` for `armed remaining=0`).
  Verdict metric: **balance** (`lo≈hi`), not magnitude (a half-step is ~¼ MSE for translation but
  ~½ for rotation). `DUP` = a side ≈0 (no interp).
- **Analysis**: `tools/interp/verify_shots.py` (one triplet → pan shots + diffs),
  `tools/interp/verify_walk.py` (a long capture → `walk.mp4` + `walk_diff.mp4` + `walk_mse.png` +
  doubling/DUP summary).
- **Verified**: in-between is a genuine intermediate frame, `DUP=0`, balanced.

---

## OPEN FRONTIER 1 — eliminate ALL skips + cadence doublings (see directive above)

**Symptom:** even with an in-between inserted every gameplay frame (`redraws` ~30/s for the
30fps plaza), the present stream has ~13% doublings (`RR`/`BB`), worse under load. The
`skip_*` counters stay 0 during the walk (only `skip_full=9` from boot), so the doublings are a
**present-level** problem, not the `room`-condition insertion skip.

**Leading hypothesis:** the real + in-between presents (both issued back-to-back inside one
`endRendering` in `interp_redraw.cpp`) don't map 1:1 onto the two VI fields — the async VI
present/field timing sometimes shows one buffer twice or drops one. (Prior note: "present cadence
jitter stddev ~4.5ms — VI field timing vs 2-copies/frame not perfectly 1:1.") Also eliminate the
boot `skip_full=9` (`!g_gfx_valid`): interpolate from the first gameplay frame too.

**Where to work:**
- `runtime/overrides/interp_redraw.cpp` — `ov_interp_endRendering` (`DISPLAY_END_RENDER`): the
  `room` condition (must never skip), the two `func_802f80d0` presents + `setNextXFB(alt)`, the
  `wait` (`display+0x4C`) field-count handling. The 1:1 mapping of (real, in-between) → (field 0,
  field 1) must be made exact.
- `externals/dolphin/.../Present.cpp` `ViSwap` — VI field/present timing, `is_duplicate`
  detection, the cadence counters (the measurement seam).

**Measure:** `/interp60` → `CADENCE (lifetime, no readback)` (true rate; the `/verify` readback
PERTURBS it — Heisenbug, 35% vs ~13%). `tools/interp/verify_walk.py` prints the full R/B run
pattern. Reproduce: boot fastboot+interp60, `curl '/pad?do=right&ms=14000'`, sample `/interp60`.

---

## OPEN FRONTIER 2 — effects are wrong under interpolation (co-equal priority, the user flagged this explicitly)

This is the second mandatory issue, not a nice-to-have. The render-only replay must produce a
correct frame for BOTH the real present and the in-between present, **including every effect** —
screen-space/EFB-feedback (water refraction, dash shimmer, underwater filter), cast shadows /
marukage (the round ground+water shadow), and any other on/off-per-field effect. The user's
standing directive from the start of this work: *"it's not just shadows — apply this to ALL
on-off effects."* The concrete instance found is the screenspace lag below; the FIX must cover
the whole class, not just water.

**Symptom (user, frames 594–597 of a walk):** the in-between frames look correct, but the REAL
frames show the screenspace effect (water refraction / EFB-feedback dynamic texture) "following
behind" — lagged ~half a step.

**Root cause (confirmed):** the in-between REPLAY re-runs the captured GX stream including its EFB
copies, which write to the FIXED guest screenspace texture address (`TEfbCtrlTex` `mImagePtr` @+0x2C).
The in-between's copy (screen at N‑½) **overwrites** the texture that the NEXT real frame's water
feedback samples → the real frame's reflection shows N‑½ instead of N (lagged). The in-between
itself is self-consistent (copy + sample both N‑½), so it looks right.

**The fix policy already EXISTS but is wired to dead code.** `runtime/overrides/efb_native.cpp`
redirects the in-between's EFB-copy dest AND its consumer texture read to `alt = orig ^ 0x400000`
(so the in-between gets its own screenspace texture and never clobbers the real frame's). But it
hooks the **guest `GXCopyTex` function** (`ov_efb_native_copytex`) and is armed by
`sb_efb_native_begin_inbetween()` in the **dead mutating path** (`interp_redraw.cpp` line ~369,
unreachable — the live replay branch returns earlier). Under the render-only replay there is no
guest function call (raw GX bytes through the OpcodeDecoder), so the override never fires.

**FIX:** add a **fork OpcodeDecoder / BP EFB-copy seam** (exactly like `sb_slot_xf_indexed` for
`LoadIndexedXF`). During the in-between replay (`g_interp60_in_redraw == true`): redirect the
EFB-copy **destination** and the EFB-copy-texture **sample source** from a tracked screenspace
address → `alt`. Reuse `efb_native.cpp`'s policy (tracked dests, `^0x400000`, `copy_to_ram=false`
⇒ no guest-RAM clobber). Implement in `externals/dolphin/.../BPStructs.cpp` (EFB copy execute →
`g_texture_cache->CopyRenderTargetToTexture`) + the texture-load/sample path; expose via a new
`SunbrightHooks.h` slot; runtime sets the redirect active during replay.
RE notes already exist: `docs/re_notes/efb_native_60fps.md`, `efb_dynamic_texture_chain.md`,
`interp_screenspace_strategy.md`.

---

## Key files
- `runtime/overrides/interp_capture.cpp` — motion-interp map (identity + phase-agnostic both-dir
  pairing; `SUNBRIGHT_DBG_XF` classifier diagnostics).
- `runtime/overrides/interp_redraw.cpp` — in-between insertion, the two presents, replay,
  `/interp60` report, cadence readout, `room`/`skip` logic. **(Frontier 1 lives here.)**
- `runtime/overrides/interp_verify.cpp` — `/verify` capture + analysis (PPM dump).
- `runtime/overrides/efb_native.cpp` — EFB-copy redirect POLICY (needs rewiring to the replay).
  **(Frontier 2 reuses this.)**
- `runtime/probe_server.cpp` — `/verify` endpoint.
- `externals/dolphin/Source/Core/VideoCommon/Present.{cpp,h}` — capture hook, cadence counters.
- `externals/dolphin/Source/Core/Common/SunbrightHooks.h` — fork hook slots.
- `tools/interp/verify_shots.py`, `tools/interp/verify_walk.py` — analysis.
- `runtime/interp60.h` — `g_i60` shared state (probe controls/observations).

## How to reproduce / measure
```
SUNBRIGHT_HEADLESS=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_INTERP60=1 SUNBRIGHT_PROBE=1 ./build/sunbright   # boot to plaza
curl '127.0.0.1:17654/pad?do=right&ms=14000'        # walk right (cright = rotate camera)
curl '127.0.0.1:17654/interp60' | grep -E 'skips|CADENCE'   # true skips + doubling rate (no readback)
curl '127.0.0.1:17654/verify?n=600'                 # arm a 600-frame capture, then poll /verify
python3 tools/interp/verify_walk.py                 # -> scratch/verify/s<ts>/{walk.mp4,walk_diff.mp4,walk_mse.png}
```
Always headless; `/recompile` not needed for any of this (all runtime/fork).

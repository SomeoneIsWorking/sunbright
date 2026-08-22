# 2026-07-16 — Performance: renderer 33× faster (J3D texture cache missing every draw)

> **Measurement supersession (2026-08-22):** The elapsed microseconds, speedup ratio, and FPS
> ceiling below are historical observations, not admissible optimization evidence. Their root-cause
> evidence survives independently: the cache missed every draw, conversion/upload counts collapsed
> after stable content identities were introduced, and render-output controls remained correct.
> Future optimization choices use deterministic subsystem work and bounded no-loss sampling.

User: "Performance is not ideal." Title/file-select ran ~17 fps in turbo.

## Profiling chain (build-the-instrument-first)
Added `SB_PROFILE=N` (sms-boot frame_seam, per-phase frame wall-clock) →
- game logic ≈ 0.9 ms, events/begin negligible, **aurora_end_frame ≈ 56 ms** (98% of frame).

Added `SB_PROFILE_GFX=N` (aurora end_frame: drain/finish/submit split) →
- **gx::fifo::drain ≈ 62 ms**, finish ≈ 15 µs, submit ≈ 50 µs. Bottleneck = the GX→wgpu
  translation on the main thread, NOT the GPU. Pipelines cached (createdPipelines constant).

Per-draw A/B (SB_ONLY_DRAW to drop the GPU build) → parse+vertex-push only = 810 µs, so the
61 ms is the per-draw GPU-command build (365 draws, ~165 µs/draw). Sub-timing each build call
localized it to **`resolve_sampled_textures` = 53.7 ms/frame** (~90% of the whole frame).

## Root cause
`resolve_static_texture`'s static-texture cache (`s_textureObjectCaches`) is keyed by
`GXTexObj_::texObjId`, and it SKIPS the cache when `texObjId == 0` (and `store_cached_texture`
early-returns). SMS binds textures through the GD path `sb_gd_load_texobj_aurora`
(J3DTevs.cpp) → `GX_AURORA_LOAD_TEXOBJ`, which **wrote `texObjId = 0` ("uncached")**. So every
J3D texture re-ran `new_static_texture_2d` (full GC→RGBA8 convert + GPU upload) on EVERY draw —
~797 k conversions over a short run (~1 per draw). (The stock `GXInitTexObj` path also churned a
fresh monotonic id per call, same effect — fixed there too via `content_tex_obj_id`.)

## Fix
Write a STABLE content-hash `texObjId` = FNV-1a over the texture identity (data ptr + width +
height + format + mipCount), non-zero. Re-binds of the same texture now hit the cache; after
warmup, conversions ≈ 0. `texDataVersion` written = 1 (constant): J3D static textures don't
change content in place (animation swaps the bound img pointer → different hash); EFB-copy /
dynamic textures never use this cache (they go through `copyTextures`, `copyRef != nullptr`).

## Result (verified)
- `drain`: **62,000 µs → 1,880 µs (33×)**; `resolve_sampled_textures`: 53,420 µs → 23 µs.
- Whole frame ~57 ms → ~2 ms in turbo (~17 fps → 300+ fps ceiling; unblocks steady 60 fps).
- Title + file-select render **pixel-correct** (no stale/garbage textures):
  scratch/pndump/perf_title.png, perf_fsel.png.

Commits: reference/sms cb420f98 (emitter), aurora 5e061f4 (profiling tooling + GXInitTexObj id).

## Tooling kept
- `SB_PROFILE=N` — per-phase frame timing (game/endframe/events/begin).
- `SB_PROFILE_GFX=N` — drain/finish/submit + per-draw-build breakdown
  (arrayUpload/shaderinfo/bindgroups/pipeline_ref/build_uniform/push_cmd/resolve_tex).
Both gated, ~free when unset. Use them before any future perf work — they localize to the exact
GX build phase.

## Next perf candidates (if pursued)
Post-fix, the per-draw-build residual is bindgroups ≈ 3.4 ms/frame and shaderinfo ≈ 0.3 ms —
`build_bind_groups` rebuilds the descriptor + xxh3-hashes + mutex-locks per draw even on a cache
hit; a per-draw bind-group memo (skip when texture set unchanged) would shave it. Not urgent at
2 ms/frame.

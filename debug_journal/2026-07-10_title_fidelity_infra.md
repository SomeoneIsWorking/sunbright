# 2026-07-10 — Title-fidelity infrastructure: SPIRV crash post-mortem + headless WSI deadlock

Arc context: USER gate — no airport/gameplay work until the title is oracle-identical.
The first valid diff (debug_journal/2026-07-10_title_fidelity.md) needs a reliable
cold-cache boot for the [dbhead] backdrop census; two infra defects stood in the way.

## 1. "Tint invalid SPIRV" crash — ROOT CAUSE FOUND & FIXED (2026-07-11, supersedes the ccache theory)

**The 2026-07-10 "closed as ccache staleness" verdict below was WRONG.** On 2026-07-11 the
crash reproduced 5-6/6 on a verified `--clean-first CCACHE_DISABLE=1` build (the earlier
agent's dismissed claim was CORRECT — it was verified erroneous to dismiss it). It is NOT
ccache; it is a genuine Tint miscompile.

Root cause: aurora's WGSL vertex-fetch helpers used **dynamic-offset `extractBits`**
(`extractBits(word, sub*8u, 16u)` in `load_u16`; `extractBits(word, shift+Nu, 8u)` in
`raw_fetch_u8_2`). Tint lowers a *runtime-offset* extractBits with a clamped path that
emits `OpExtInst(GLSL.std.450 UMin ...)`, and its SPIRV writer produces an invalid operand
(the GLSL.std.450 `OpExtInstImport` id used where a type is required → `Operand '%N' cannot
be a type` / `requires a type`). Nondeterministic SPIRV line = which pipeline cold-compiles
first varies; warm dawn_cache masked it (hence "not reproducible" when a cache existed).
Constant-offset extractBits compile fine (Tint knows they're in-range, no clamp, no ExtInst).

Fix (aurora `lib/gx/shader.cpp`): replace the 3 dynamic-offset extractBits with equivalent
shift+mask (`(word >> (sub*8u)) & 0xffffu`, `(word >> (shift+Nu)) & 0xffu`). Guarded ranges
(`sub<=2`) make offset+count<=32 so they are exact equivalents. Verified: 0 SPIRV crashes,
title renders to J2D draws, full-length runs. bswap16 if-return (7d96ec4) fixed a *different*
instance of the same Tint-chokes-on-vertex-fetch-helpers class; this was the remaining one.

## 2. Real cold-boot defect: hidden-window WSI swapchain deadlock (fix in flight)

With SB_HEADLESS (hidden X11 window), cold boots deadlock: render worker inside
dawn vulkan SwapChain::GetCurrentTextureInternal → wsi_drm_wait_for_explicit_sync_release
→ drmSyncobjTimelineWait (never signaled — the compositor never displays the hidden
surface, so buffer release can stall); main thread waits in wait_for_gpu_progress ←
begin_frame. Warm-cache boots usually squeak past (timing). Proper fix (in flight):
under SB_HEADLESS aurora never touches the surface/swapchain — offscreen render target
only, present is a no-op, SB_DUMP_FRAME reads back from the offscreen source.

## New instrument

`SB_DUMP_WGSL=<dir>` (aurora lib/gx/shader.cpp): dumps every composed WGSL pipeline
source to <dir>/pipeline_<hash>.wgsl before compilation. Permanent, env-gated.

## Verification discipline note

Cold-cache testing must move BOTH `<home>/.local/share/Sunbright/pipeline_cache.db` AND
`dawn_cache.db` (+-shm/-wal) aside — dawn_cache alone can mask a cold Tint recompile.

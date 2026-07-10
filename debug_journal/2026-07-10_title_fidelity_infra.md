# 2026-07-10 — Title-fidelity infrastructure: SPIRV crash post-mortem + headless WSI deadlock

Arc context: USER gate — no airport/gameplay work until the title is oracle-identical.
The first valid diff (debug_journal/2026-07-10_title_fidelity.md) needs a reliable
cold-cache boot for the [dbhead] backdrop census; two infra defects stood in the way.

## 1. "Tint invalid SPIRV" crash — closed as NOT REPRODUCIBLE on a clean rebuild

Historic logs showed `Produced invalid SPIRV ... %bswap16 = OpFunction` aborts on cold
pipeline compiles. Investigation: the fix for exactly this (aurora `7d96ec4`, bswap16
if-return instead of select(), 2026-07-05) IS in source and PREDATES the lineage swap;
no Dawn/Tint version change in the bump range. After a forced recompile of shader.cpp,
74 pipelines cold-compiled clean twice. Verdict: almost certainly the known
[[ccache-shadow-staleness]] landmine serving a stale shader.cpp object.
**Contradiction on record**: an earlier agent claimed a `--clean-first` +
`CCACHE_DISABLE=1` rebuild still crashed — that claim did not survive re-testing and is
presumed erroneous (agent reports must be verified against artifacts). If the SPIRV
abort EVER reappears on a verified-clean build, reopen; until then, closed.

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

# 2026-07-14 — Seagull invisibility: narrowed to "zero fragments"; fixes + falsifications

Replay of `title_settled.dff`: the bottom-left/left seagulls (post-merge draws #133-#135)
render in Dolphin's playback but emit NOTHING through aurora; the O-ring bird (#137-138)
renders but pale. Symptom re-validated on the current build (`scratch/shots/seagull_ab.png`).

## Landed fix (real, independent of the remaining mystery)

`fifo_player` synthesized every `GX_AURORA_LOAD_TEXOBJ` with **mipCount=0** (comment
claimed "mode1 max_lod arrives via BP" — but `GXTexObj_::mip_count()` gates on the
has_mips flag that ONLY this byte sets, so every replayed texture sampled base-only).
Now tracks per-texmap TexMode1 (BP 0x84-0x87/0xA4-0xA7, seeded from the .dff BP
snapshot) and derives mipCount = max_lod integer part + 1 (clamped to chain depth).
Fixes minification aliasing for ALL replayed mipmapped textures (same class as the
cloud-moire fix on the native GD path). NOTE: this did NOT fix the invisible birds —
their bind's mode1 is genuinely 0 at that stream position (Dolphin's snapshot agrees).

## Falsified for the invisible pair (do not re-chase)

- Texture content: [tex-id] FNV hash identical for both pairs (ver 285 vs 287 split is
  an unrelated-region invalidation; the dff has NO atlas-sized memupdate — 64x64 RGB5A3
  atlas at GC 0xAD47C0 comes from the initial texture-memory snapshot).
- Mip selection: invisible pair binds base-only in Dolphin too (snapshot mode1=0).
- Alpha cutout: new `SB_NO_ACMP=1` (drop alpha-compare from all pipelines) changes only
  ~1400 cloud-edge pixels; no bird appears anywhere.
- Depth: SB_NO_DEPTH=1 byte-identical (earlier).
- Culling/winding: bird body draws are GX_CULL_NONE; all posmtx dets positive.
- Geometry (CPU state): per-vertex NDC probe — all verts on-screen, tiny (~3×7 px
  spread), inZ ok, w>0; UVs sane 0..1 (new t0 dump in ndc-probe).
- Uniform staleness via draw merging: merging is gated on !stateDirty and
  copy_xf_data (LOAD_INDX target) sets stateDirty — mid-stream matrix loads DO break
  merges.

## Session 2 additions (same day): depth ELIMINATED for real; every state layer now falsified

- The earlier "SB_NO_DEPTH changed 0 pixels" was measured post `-alpha off` — comparing
  the RAW dumps shows 25 alpha-only pixel diffs at BOTH bird locations. That looked like
  "birds rasterize but are depth-killed, clouds (aU=0) later overwrite color". FALSIFIED
  in turn by per-draw instruments:
  - `SB_NO_ZWRITE_DRAWS=0:9999999` (suppress ALL depth writes): byte-identical frame ⇒
    NOTHING in this frame is ever killed by another draw's written depth.
  - `SB_NO_ZTEST_DRAWS=132:144` (force compare Always for the bird draws): byte-identical
    ⇒ the birds are NOT depth-killed at all — they emit ZERO fragments. (Pipelines
    compile synchronously by default, so these aren't async-skip false negatives.)
  - The 25 alpha diffs under global SB_NO_DEPTH remain unexplained — do not treat them
    as bird evidence.
- `SB_NO_ARRCACHE=1` (re-upload every indexed array per draw): byte-identical ⇒ stale
  array-upload caching is NOT the cause.
- WGSL fetch helpers audited (load_u8/u16/u32_raw): all shift+mask, no dynamic-offset
  extractBits — the Tint-miscompile class does not apply.
- `SB_ONLY_DRAW` isolation windows are CONFOUNDED around EFB-copy clears (skipping the
  draws around a copy changes clear behavior); its "enabler draw #76/#77" readings are
  artifacts, not dependencies.
- Bird trifan triangle areas computed from probe NDC: up to 6.2 px² (≈25 px² at 2×
  internal) — not degenerate; coverage cannot explain zero fragments.

## Remaining suspects (next session)

The draws produce ZERO fragments with every fixed-function kill path disabled. The
divergence must be in the GPU-side vertex path producing out-of-frustum/degenerate
positions from the SAME inputs the CPU probe transforms correctly. Bird vertex layout
is distinctive: per-vertex PNMTXIDX (direct u8) + POS/NRM/CLR0/TEX0 all INDEX16 —
odd 9-ish byte stride, per-vertex matrix indexing into the pnMtx uniform.
Next instruments (pick one):
1. `SB_DUMP_WGSL=<dir>` the bird pipeline + dump its uniform block (proj + postex_mtx)
   at push time, then HAND-SIMULATE vertex 0 of draw #133 through the WGSL fetch code
   (offsets, stride, in_pnmtxidx /3, mat3x4 row-vector order) vs the CPU probe.
2. RenderDoc capture of the replay frame (Dawn/Vulkan) — inspect the bird draw's VS
   inputs/outputs directly.
Watch for: config.vtxStride vs the stream parser's vtxSize disagreeing for this
layout; mat3x4 postex_mtx row/column convention for the `vec4f(pos,1) * mtx` product;
uniform offset misalignment shifting postex_mtx for draws whose ubuf layout includes
optional blocks (loadsTevReg bitset, lighting).

## Instruments added (aurora 6f20fc6, 6b003bd)

- `SB_NO_ACMP=1` — global alpha-compare drop (pipeline-level bisect).
- `[tex-id]` — per-windowed-draw texture identity: GC addr, host ptr, dims/fmt,
  version, mip count, content hash, per-mip RGB5A3/IA8 alpha stats.
- ndc-probe now prints per-vertex TEX0 UVs.
- `SB_FIFO_TEXDBG=2` — uncapped bind log incl. tracked mode1.

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

## Remaining suspects (next session)

The draws produce ZERO fragments (with cutout disabled, blend off, colorUpdate on —
any fragment would be visible). Either:
1. The GPU-side vertex path (storage-buffer attribute fetch + pnMtx uniform indexing)
   transforms these vertices differently than the CPU-state probe (e.g. PNMTXIDX
   handling for this vertex layout, or a stale/partial pos-array cachedRange upload
   for the bird pool), or
2. Thin-trifan rasterization: the fan's triangles are slivers that miss all sample
   centers in wgpu while GC/Dolphin's fill rules catch them (the O-ring bird's
   paleness could be partial coverage). Note aurora renders at 2x internal — MORE
   samples than Dolphin 1x — which argues against pure coverage.
Probe idea: [vtx-gpu] instrument — read back the exact bytes the vertex-fetch shader
sees for one windowed draw (storage-buffer slice at the draw's vaRanges + uniform
pnMtx block) and cross-check against the CPU probe; or force PNMTX0 (identity-skin)
for the windowed draw to see if it appears.

## Instruments added (aurora 6f20fc6, 6b003bd)

- `SB_NO_ACMP=1` — global alpha-compare drop (pipeline-level bisect).
- `[tex-id]` — per-windowed-draw texture identity: GC addr, host ptr, dims/fmt,
  version, mip count, content hash, per-mip RGB5A3/IA8 alpha stats.
- ndc-probe now prints per-vertex TEX0 UVs.
- `SB_FIFO_TEXDBG=2` — uncapped bind log incl. tracked mode1.

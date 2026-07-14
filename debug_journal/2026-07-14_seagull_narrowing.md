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

## WGSL source audit (done, session 2): consistent — needs a GPU capture next

The bird pipeline is `scratch/wgsl/pipeline_530c51bd37db0ffa.wgsl` (SB_DUMP_WGSL over
the replay; identified by the unique stride-9 layout). Verified line-by-line against
the CPU probe:
- vertex fetch: pnmtxidx u8 @+0 (shift+mask, `/3u`), pos INDEX16 @+1 (S16 frac 8),
  nrm @+3 (frac 14), clr0 @+5 (rgba8 array), tex0 @+7 (S16 frac 8) — byte-identical
  to the probe's walker offsets.
- transform: `vec4f(in_pos,1) * postex_mtx[idx]` with mat3x4 columns = GC rows =
  exactly the probe's row dot products; proj likewise; reversed-z `out.pos.z = -z`.
- uniform struct offsets (vtx_start..array_start[12]=80B, proj@80, postex_mtx@144,
  20×48B, nrm_mtx 10×48B) match shader_info.cpp's append order/alignment.
So the SOURCE agrees with the CPU math that puts the birds on-screen, yet zero
fragments come out.

## Session 3 additions: uniforms verified, driver exonerated — it's the draw execution

- **lavapipe reproduces it**: `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`
  replay → birds still missing on software Vulkan. Not a native-driver bug; the
  submitted commands themselves are wrong somewhere.
- **`SB_UNIF_DUMP=<lo>[:<hi>]`** (new, build_uniform): dumps vtxStart/curPn/viewports/
  vaRange offsets/proj/all 10 pn matrices for windowed draws. Draw #133's dump shows
  the bird bone matrices (pn0-pn8 at ≈(-3224,-2729,-7932)) and a sane projection.
  HAND-SIMULATED vertex 0 through the WGSL math with these exact values:
  mv=(-3206.5,…) matches [ndc-probe]; x_ndc=-0.827; wgpu z=0.00123 in [0,1]. The
  uniform data is PROVABLY correct at push time.
- Remaining surface, by elimination: the indexed-draw EXECUTION for this draw —
  index-buffer generation (trifan triangulation), draw-call parameters
  (base vertex/first index/count), or the vbuf staging slice at vtxStart. Everything
  upstream (state, uniforms, arrays, shader source) is verified.
- RenderDoc trigger IMPLEMENTED (`SB_RDOC=<present#>` + `SB_RDOC_PATH`, aurora ad45944:
  dlopen + Start/EndFrameCapture at end_frame boundaries, headless-safe) but BLOCKED:
  under `renderdoccmd capture`, StartFrameCapture crashes the process (Dawn interplay;
  SB_NO_GPU_PROF=1 — new switch disabling timestamp queries — does not fix it). Plain
  dlopen mode returns success but writes no .rdc (no Vulkan layer injected).
- Draw-execution layer also audited: prepare_idx_buffer fan triangulation standard,
  instanceCount=1, bind_pipeline ASSERTs on miss (no silent skip; ASSERT active in
  Release), render() sets index buffer from idxRange and DrawIndexed(indexCount, 1).
- RECOMMENDED NEXT (in our control, no RenderDoc needed): a VS-output debug — for
  SB_NDC_DRAW-windowed draws, generate the pipeline with an extra storage-buffer
  binding and have the vertex shader write out.pos per vertex; read back after drain.
  Compares what the GPU ACTUALLY computed against the CPU probe with zero external
  tooling. Alternatively debug the RenderDoc crash (try --opt-api-validation,
  windowed non-headless run, or capture only a trimmed replay).

## Instruments added (aurora 6f20fc6, 6b003bd)

- `SB_NO_ACMP=1` — global alpha-compare drop (pipeline-level bisect).
- `[tex-id]` — per-windowed-draw texture identity: GC addr, host ptr, dims/fmt,
  version, mip count, content hash, per-mip RGB5A3/IA8 alpha stats.
- ndc-probe now prints per-vertex TEX0 UVs.
- `SB_FIFO_TEXDBG=2` — uncapped bind log incl. tracked mode1.

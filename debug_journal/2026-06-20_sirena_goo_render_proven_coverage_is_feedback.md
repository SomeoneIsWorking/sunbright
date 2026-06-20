# 2026-06-20 — Sirena goo: RENDER PATH PROVEN, coverage is a VRAM-only feedback texture

## The decisive experiment (verified)
Built a gated inspection tee on `TPollutionCounterLayer::countTexDegree` (0x8019b3a0,
`runtime/overrides/pollution_inspect.cpp`, `SUNBRIGHT_DBG_POLL` / `SUNBRIGHT_POLL_FORCE`). It reads
gpPollution's counter-layer state straight from guest RAM (`this`=gpr[3]; gpPollution = this-0x70).

`SUNBRIGHT_POLL_FORCE=1` fills ngx's EFB side buffer (`sb_ngx_efb_store_copy`) with FULL coverage
(0xFFFFFFFF) for each layer's `unk54` EA each frame, then invalidates. **Result: the Sirena goo
renders correctly** (green/yellow/cyan goo over the whole beach + the HUD counters/gauge), visually
equivalent to the GX baseline (`scratch/screenshots/sirena_gx.gx.png` vs `sirena_force.ngx.png`).

## What this PROVES (corrects the handoff/prior RE)
1. **ngx already captures and renders the goo plane correctly.** The goo is the `TPollutionLayer`
   J3D plane drawn in the MAIN scene (the sandy beach floor IS that plane in ngx). It samples
   `unk54` (the coverage I8 texture) as a MASK. Coverage 0 ⇒ plane masked out ⇒ you see the floor
   under it (orange/sandy). Coverage full ⇒ goo shows. The missing thing was never the plane's
   geometry/capture — it was the coverage texture content.
2. **The coverage (`unk54`, per-layer I8 512×512 at EA 80c72780 for Sirena) is VRAM-ONLY.** `unk54`
   RAM reads **all zeros in BOTH ngx present AND the Dolphin-GX baseline** (`/r?a=80c72780`). Dolphin
   keeps the GX_CTF_R8 EFB-copy in its texture cache (VRAM) and does NOT write it back to guest RAM,
   so the goo plane samples the VRAM copy. ngx (which decodes textures from guest RAM) sees zeros.
   The side buffer (`g_efb_side`, served by `texture_for` for that EA at matching 512×512) is the
   correct ngx home for it — proven by the force test.
3. **The coverage is a feedback render-to-texture.** `countTexDegree` each frame: `drawBlack` (clear
   red→0), `drawTex` (re-draw previous coverage = persistence, with a decay/threshold TEV from
   `initGXforPollutionLayer`), then stamps; a per-layer GX_CTF_R8 copy grabs EFB red → `unk54`.
4. **In the fastboot Manta-Storm state the task queues are EMPTY** (joint=0, tex ntasks=0,
   revival=0, model=0) in both ngx and baseline at t=45s — yet the baseline shows full goo. So the
   static goo is the feedback PERSISTING a seed established at episode load; it is not re-stamped
   every frame in this idle state. The Manta-Storm layer is `type=4` (loads ms_thunder particle).

## Why the capture path is OUT (and the per-epoch infra can't drive it as-is)
`countTexDegree`'s draws are immediate-mode `GXBegin(GX_QUADS)`/`GXPosition2u16`/`GXTexCoord2u16`
(+ `getShapeDraw()->draw()`). `GXPosition2u16` writes the vertex straight to the WGPIPE gather pipe
(0xCC008000) — capturing it = decoding the GX FIFO byte stream, which the user explicitly rejected.
So `shapes_in_epoch=0` for the graffito copy is expected and unfixable by "tag the epoch": there are
no J3DShape::draw calls AND the immediate quads aren't function calls. The per-epoch offscreen infra
(landed 9352480) is correct machinery but has nothing to render here.

## The remaining faithful work (native port, the genuine frontier)
Produce the coverage natively in the side buffer (EA `unk54`), per layer, by REPRODUCING
`countTexDegree`'s feedback render from the object model — NOT by capturing GX. Open sub-question
that gates a clean port: **the seed.** Where does the initial full-beach coverage come from with
empty task queues? Candidates to pin next: `TPollutionLayer::action` (0x8019a2e4, calls
`stamp` at +0x398 — per-frame re-pollute?), the `/scene/map/pollution/<name>.bmp` depth map
(`unk80`, loaded by `initTexImage`; baked into `unk54` only for `mMap==9` — check Sirena's mMap),
and `TPollutionManager::stamp` (0x8019de84, callers incl. TBossManta::moveObject). Decide via a
"force coverage ONCE at frame 0, does it persist?" experiment (feedback lossless ⇒ seed+run-feedback;
lossy ⇒ re-stamp every frame).

## GROUND TRUTH obtained (decisive tooling: SUNBRIGHT_DUMP_TEX)
`SUNBRIGHT_DUMP_TEX=1` (main_sdl.cpp) sets `GFX_HACK_SKIP_EFB_COPY_TO_RAM=false` so Dolphin writes
EFB-copies back to RAM (+ `GFX_DUMP_TEXTURES`). Run the **baseline** (NGX_PRESENT=0) with it and the
real coverage lands in `unk54` RAM — readable via `/r?a=80c72780` and dumped de-tiled to PGM by the
inspect override (`dump_coverage`/`dump_depthmap`, gated on SUNBRIGHT_DBG_POLL).

Findings (`scratch/bin/pollution_{cov,depth}_0.png`):
- Sirena Manta-Storm is **mMap=6** (NOT 9) → `initTexImage` zeros unk54, so the goo is NOT depth-seeded.
- Real coverage: **nonzero=47875 texels, mean=244** (saturated ~0xff where present), a specific
  connected goo blob — NOT the full depth-polluted region (201005 texels). So coverage ≠ f(depth).
- unk54 (I8 512×512) uses the SAME 8×4 GC tiling as the depth map (`TPollutionPos::index`).
- **No per-frame stamps** (joint/tex/revival/model = 0 every frame, from the first countTexDegree
  call). The feedback TEV (`initGXforPollutionLayer`, type=4) is **bistable**: coverage ≥ ~50 stays,
  < ~50 decays to 0 (verified: edges read 8/7/2, interior 0xff). So the goo blob is a ONE-TIME SEED
  established at episode load, then sustained forever by feedback with zero per-frame input.

## Remaining unknown that gates the native port: THE SEED
Where the one-time goo blob is written at episode load (queues empty ⇒ not via countTexDegree's task
queues at steady state). The decomp STUBS the candidates (`TPollutionLayer::action` 0x8019a2e4,
`::stamp` 0x801a1440, init), so this needs binary RE (disasm). Once the seed is known, the native
port = seed the side buffer once + run the (simple, derived) feedback each frame in the override.
NOTE: `SUNBRIGHT_DUMP_TEX` does NOT help under present (Dolphin doesn't render the graffito pass
under ngx present — gx_super discards it), so it's a baseline-only ground-truth/oracle tool.

## Don't re-chase
- "unk54 RAM holds the coverage" — NO, zeros in both modes; it's Dolphin VRAM-only.
- "Capture the goo draws into the graffito epoch" — they're WGPIPE immediate-mode = FIFO decode (out).
- Full-coverage force is a DIAGNOSTIC (SUNBRIGHT_POLL_FORCE), off by default, NOT the fix (ignores
  cleaning + the real pollution-map shape; only coincidentally correct at Manta-Storm t=0).

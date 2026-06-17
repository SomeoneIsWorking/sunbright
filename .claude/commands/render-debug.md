# /render-debug — live native-renderer (ngx) fidelity debugging

Drive the RUNNING game and compare the native renderer (ngx) against the Dolphin-GX oracle on
the **same game state**, then isolate defects with live toggles (no rebuild to *use* them).
Full rationale + proof of validity: `docs/render_ab_harness.md`. Skinning case study:
`debug_journal/2026-06-17_skinned_mario_shred.md`.

## ⚠️ Sandboxed-Bash rules (these silently break everything if ignored)
The game binary + the probe HTTP both need GPU/network, which the Claude Bash sandbox BLOCKS
SILENTLY (curl returns empty, launch exits 1 with no output). So:
- Run every game launch / curl with the Bash sandbox **disabled** (`dangerouslyDisableSandbox: true`).
- The game must run in the **foreground** of the call — launching with `&` aborts the call. Put the
  *capturer* (poll → freeze → abshot2 → kill) in a background subshell; `tools/gpshot` does this.
- **Never** `pkill -f "build/sunbright"` (matches the driving shell → kills your own call). Use
  `pkill -x sunbright`.
- A new foreground Bash call kills a prior `run_in_background` game task → do launch+wait+capture in
  ONE call (that's `tools/gpshot`).
- Always `pkill -9 -x sunbright` first; a stale instance squats port 17654 with the OLD binary.

## One-shot A/B (start here)
```
tools/gpshot                 # FASTBOOT Delfino gameplay → zero-drift GX-vs-ngx A/B + region delta
tools/gpshot --fs            # AUTOSTART → file-select (skinned Mario / J2D HUD)
tools/gpshot --fs '/ngxskip?ti=10'   # apply probe GET(s) before the capture (isolate a material)
```
Output: `scratch/screenshots/ab2.{gx,ngx}.png` (oracle vs native, SAME present) + per-region mean
pixel delta. The abshot2 line prints `ngx_frame=N` — if two captures show the same N, the snapshot
is stale; distrust it. Read the two PNGs and compare. Crop a region to inspect (e.g. Mario):
`magick scratch/screenshots/ab2.ngx.ppm -crop 200x148+30+300 -scale 300% out.png`.

abshot2's GX side is an UNTAINTED oracle (all ngx capture hooks super-call the real fns) and it's
single-core (can't drift). It tests *ngx vs Dolphin-GX of the same recomp output*. It does NOT tell
you if the recomp itself is right — if the GX side ALSO shows the defect, it's a recomp bug
(use `SUNBRIGHT_DISABLE_RECOMP`/`SUNBRIGHT_DIFF`).

## Live isolation toggles (probe GETs on the running game, no rebuild)
- `/ngxmtxsrc?m=0|1|2` — skinned pos-matrix source: 0=per-packet object-model, 1=g_posmtx, 2=single
  modelview. `m=2` collapsing a model to a coherent blob proves positions are fine and the defect is
  the per-vertex matrix. `/ngxshape` reports cumulative `pkt_applied`/`fallback`.
- `/ngxonly?ti=N` / `/ngxskip?ti=N` — render only / skip one material (tev_index).
- `/ngxnoblend?on=0` — force opaque. `/ngxfreeze?on=1` — latch the snapshot for repeated probes.
- `/pixbatch?x=NDC&y=NDC` — which batch/material wins a pixel + its raster rgba + depth order.
  `x=-901 y=<ti>` = full combiner + TEV swap-table dump for material ti.
- `/ngxshape` — big state dump: PNMTXIDX shape/vert/maxnelem counts, lighting/illum, sky, fog, TEV.

## Live env-gated dumps (need ONE rebuild to add, then driveable)
- `SUNBRIGHT_DBG_PKT=1` → stderr `[pkt]`/`[pktv]`/`[pktt]`: per-packet J3DShapeMtxMulti unkC tables,
  vert matidx ranges, and resolved per-slot matrix translations — for skinned-matrix debugging.

## Manual launch recipe (when gpshot isn't enough)
Run as the foreground of a sandbox-disabled call; capturer in a bg subshell that polls
`/ngxshape` for `frame_swaps>=480` (file-select) or `>=60` (gameplay), then curls, then
`pkill -9 -x sunbright`. Boot env: `SUNBRIGHT_HEADLESS=1 SUNBRIGHT_PROBE=1 SUNBRIGHT_NGX_SHAPE=1
SUNBRIGHT_NGX_PRESENT=1` + `SUNBRIGHT_AUTOSTART=1` (file-select) or `SUNBRIGHT_FASTBOOT=1` (Delfino).

## Discipline
A renderer fix MUST move the ab_diff number (or a unit test), never "looks better". Verify on the
SAME present. Don't trust xfmem/CPU GP state as an oracle (async-lagged) — only rendered pixels.

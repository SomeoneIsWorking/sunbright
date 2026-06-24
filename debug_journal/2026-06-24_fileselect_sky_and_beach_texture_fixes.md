# 2026-06-24 — File-select: sky dome + beach/map texture fixes (oracle-verified)

Continuation of the scene-content work after the camera was proven correct. Both fixes were
diagnosed by VALUES (SB_BATCH_DBG) and verified against the GX pixel oracle, never eyeballed.

## Fix 1 — sky deep-blue dome bleed (commit b0ee4ed)
SB_BATCH_DBG at the settled frame showed b0 (TSky GXDrawSphere backdrop) pinned at z=0.99990
and b4 (sky.bmd gradient dome) at z[0.99982,0.99992], both full-screen. The backdrop's pin
(`if z>=0.9999 → 0.9999` in `sb_boot_capture_sphere`) pulled it NEARER than the gradient's far
rim (0.99992 > 0.99990), so the gradient lost LEQUAL at the horizon band and the deep-blue
sphere bled through as a "dome." The backdrop (radius 100000) is genuinely the farthest object;
the pin under-stated its depth. FIX: pin to 0.99997 (behind the gradient rim, still inside far).
Verified: horizon (0,18,238)→(169..201,204..216,255) light-blue gradient = oracle truth.
Note: the sphere projects NATURALLY to ~0.99993 here, so after the fix the clamp rarely engages
(only true z≈1.0 verts) — the old clamp was the active harm. Memory `fileselect-sky-dome-zpin`.

## Fix 2 — flat-white beach + all map surfaces (commit 01dd0fe)
SB_BATCH_DBG (augmented to print `ntex=` + UV span) showed the beach (key f19161bf) had
texcoords (UV tiled to 31×) but ntex=0 — no sampler. Added fail-fast logging to the previously
SILENT out-of-range/null texNo `continue` paths in `sb_resolve_textures` → `[texres] OOB
texNo=48..58 >= num=4`. The map references a 59-entry SHARED texture table (map.bmd uses
setMaterialTable) but the capture resolved against `modelData->getTexture()` — the embedded
TEX1 with only 4 entries. A `[textbl]` one-shot probe confirmed: beach material `packet=59 /
modelData=4 / sys=59`. FIX: resolve against the per-packet `J3DMatPacket::mTexture` (the
authoritative table, set at DL build, J3DModelData.cpp:558). Verified: beach (255,255,255)→
(239,204,185) tan, OOB count 40→0, and the WHOLE map re-textured (b5/b8/b14 ntex 0→4).
Memory `fileselect-perpacket-texture-table`.

## NEW LEAD (side-effect of Fix 2, NOT a regression)
A previously-white sea/shoreline surface (SB_BATCH_DBG b30, key eb5c8e74, ntex=2, UV tiled
20..51×, bm=1/4/2, a grazing near-horizon plane) now renders with harsh DIAGONAL MOIRE stripes
over the shoreline. Cause: a heavily-tiled texture sampled with VK_FILTER_NEAREST and NO mipmaps
(nvk.cpp tevSampler). It was flat-white (hidden) before; binding it correctly exposes the
renderer's missing-mipmap gap. Proper fix = mipmaps/anisotropic filtering in nvk (a renderer
feature) — NOT skipping the surface (that would be a bandaid). Lower priority than #3/#4.

## Remaining file-select divergences (unchanged, deeper RE)
- #3 file blocks: render as bare blue bars; missing A/B/C letter cubes, Corrupt/New labels, and
  the "Select data" banner. Blocks = 3D TFileLoadBlock ("ロードブロックＡ/Ｂ/Ｃ", CardLoad.cpp:496).
  Banner/labels = J2D screens (load.blo=unk28, title_1.blo=unk34) + BMG text (region-specific).
  J2D text DOES render (OPTIONS shows), so the banner pane is likely a region-tolerant dummy or
  its BMG string isn't resolved. Memory `us-disc-vs-jp-decomp-region-tolerance`.
- #4 Mario absent: scene_verts 3183 (Mario draws → ~4400). Skeletal/perform-pass issue, see
  memories `mariocap-nan-skeleton-root-cause`, `fileselect-letters-are-mariocap`.

## Tooling added this session (all committed, env-gated)
- SB_BATCH_DBG: now prints per-batch `ntex=` + first-tex dims + UV span (white-untextured triage).
- SB_J3D_DBG: `[texres] OOB/NULL` fail-fast on out-of-range/null texNo; `[textbl]` one-shot per
  material comparing packet/modelData/sys table sizes.

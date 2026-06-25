# 2026-06-25 — File-select banner/labels FAINT → fixed (GXEnd-less glyph-quad flush dropped the last vertex)

## TL;DR
The file-select text ("Select data." banner, "NEW" slot labels, "OPTIONS") rendered **faint /
half-smeared** instead of crisp opaque white. ROOT CAUSE: the native GX immediate-mode capture
(`native/platform/gx_imm_impl.cpp`) auto-finalized a GXEnd-less primitive on the **nverts-th
`GXPosition`** — i.e. BEFORE that final vertex's `GXColor`/`GXTexCoord` arrived. On GC a vertex
streams pos → colour → texcoord, so the last (top-left) corner of every glyph quad lost its colour
AND its texcoord, defaulting to `uv=(0,0)`. The quad's second triangle (BL,TR,**TL**) then sampled
the wrong texels → every glyph was half-correct / half-garbage = faint. FIX: defer the GXEnd-less
flush to the **next GXBegin / GXEnd / present-take**, after all attributes of the last vertex are in.

## How it was found
The prior chain (commit 6934430) added a GC-faithful auto-terminate so glyph quads — which omit the
HW-noop `GXEnd` (`JUTResFont::drawChar_scale`) — wouldn't be dropped. That made the text APPEAR but
FAINT. Tracing `drawChar_scale` (reference/sms JUTResFont.cpp:335) shows the per-vertex order is
`GXPosition3f32` → `GXColor1u32(mColorN)` → `GXTexCoord2u16(u,v)`, and the 4 corner colours/UVs are
BL=mColor1, BR=mColor2, TR=mColor4, TL=mColor3. The auto-terminate fired inside `sb_gx_imm_pos` the
instant `g_prim_verts.size() >= nverts` (the 4th position), so:
- the 4th vertex (TL) kept the *running* colour from vertex 3 (harmless for white text), and
- the 4th vertex's `GXTexCoord` landed AFTER `finalize_prim()` (`g_in_begin` already false) → ignored
  → TL kept the default `uv=(0,0)`.
The decoder (`runtime/render/tex_decode.cpp`) and the MODULATE/blend path were already correct (I4/I8/
IA-format alpha tracks intensity), so it was purely the dropped last-vertex attributes.

## Fix (native/platform/gx_imm_impl.cpp)
- `sb_gx_imm_begin`: if a previous prim is still open (`g_in_begin`), `finalize_prim()` FIRST, then
  start the new prim. This flushes the previous GXEnd-less glyph with all attributes intact.
- `sb_gx_imm_pos`: removed the nverts-th-position auto-finalize entirely.
- `sb_gx_imm_take` / `sb_gx_imm_take_batches`: `finalize_prim()` if a prim is still open, so the LAST
  glyph of the frame (no following GXBegin) is flushed at present.
A primitive thus completes after `nverts` whole VERTICES (pos+colour+texcoord), not nverts positions —
the actual GC semantics. Panels that DO call `GXEnd` are unchanged (GXEnd still finalizes; the new
begin/take finalize is a no-op when nothing is open).

## Verification
- Settled file-select (`SB_SEL_DUMP_SETTLED=6`, recipe in handoff): `scratch/frames/glyphfix2_full.png`
  / `glyphfix2_banner.png` — "Select data." is now CRISP opaque white, "NEW NEW NEW" + "OPTIONS"
  crisp, matching `scratch/oracle/fileselect_gx_oracle.png`. (was `glyphfix_0006.png` = faint.)
- Regression test `native/render/tests/gx_imm_glyph_test.cpp` (ctest `render_gx_imm_glyph_test`):
  drives two consecutive GXEnd-less glyph quads in exact drawChar order, asserts all four corners
  keep their own colour AND texcoord (the TL corner would be `uv=(0,0)` under the old code). PASS.
- Full render suite: 11/11 pass. Pre-existing unrelated failures (NOT from this change, confirmed by
  stash): `sms-platform_test` (link: `JKRExpHeap::createRoot`), `platform_anm_swap_test`.

## Remaining file-select divergences (NOT this task)
- Banner window FILL reads purple-ish vs the oracle's blue (J2DWindow fill colour/gradient — separate).
- Slot labels show "NEW" not "Corrupt/New/New" — correct for our BLANK card (oracle has save data).
- Black spiky shapes between slots, and Mario still a small white blob front-centre (separate items).

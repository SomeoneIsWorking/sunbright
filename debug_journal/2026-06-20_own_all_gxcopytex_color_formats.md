# 2026-06-20 — ngx owns ALL colour GXCopyTex formats (not just RGB565/RGB5A3)

## What & why (user directive: expand engine ownership)
ngx previously served EFB→texture copies (GXCopyTex) for only fmt 4 (RGB565) and fmt 5 (RGB5A3);
every other format ran Dolphin's original `GXCopyTex`, which under ngx present copies the EMPTY
Dolphin EFB → a BLACK texture (+ the async EFB→RAM stomp). So any scene using a colour copy in a
different format (e.g. RGBA8 fmt 6) would sample black.

Now ngx owns **all standard colour copy formats** (GXTexFmt 0..0xF: I4/I8/IA4/IA8/RGB565/RGB5A3/
RGBA8/CI4/CI8/CI14/CMPR). The side-buffer path is format-agnostic — `copytex_writeback` reads the
ngx scene colour (`sb_ngx_efb_copy_region`, ARGB) and stores it via `sb_ngx_efb_store_copy`;
`texture_for` reads that side buffer first and uploads it as RGBA8, so the consumer samples the
real scene regardless of the guest copy format. The guest-RAM GC-tiled write stays 16-bit-only
(fmt 4/5) — other colour formats are served purely from the side buffer.

`ov_gxcopytex` now SKIPS Dolphin's original for every owned colour format (was 4/5 only), so ngx
owns those copies end to end (no empty-EFB stomp, no wasted GP work).

## The caveat (respected): CTF/Z copy formats stay on Dolphin
Copy-only formats ≥ 0x10 are NOT the rendered colour scene and/or are read back by GAME LOGIC:
- **GX_CTF_R8 (0x28 = 40) = the "graffito check"** pass — the game reads this R8 coverage texture
  as DATA to compute graffiti-cleaning progress. ngx does NOT render the graffiti-coverage pass
  (that's the parked pollution system), so it cannot supply correct content; serving it the main
  scene would corrupt the detection. It stays on Dolphin.
- Z-buffer copies likewise.

Classifier: `ngx_efb::is_ngx_owned_copy_format(fmt) = fmt >= 0 && fmt < 0x10` (ngx_efb_copy.h),
unit-tested in render_test `efb_copy` (colour fmts owned; 0x10/0x11/0x28/0x30 + -1 not owned).

## Verification
- render_test 17/17 PASS (incl. the format-split assertions).
- No regression on the fastboot-reachable scenes: plaza, Sirena beach, Pianta render correctly.
  Those scenes use ONLY fmt 4/5 (served+skipped, unchanged) + fmt 40 (correctly routed to Dolphin,
  `[efb-wb] skip: fmt=40 (CTF/Z)`), so their behaviour is identical pre/post.
- The change is therefore strictly-improving for any non-4/5 COLOUR copy (was black → now the
  scene) and behaviourally inert where it can't help. No fastboot-scenario-0 entrance exercises a
  non-4/5 colour copy, so there is no NEW visible before/after in the cheaply-reachable scenes; the
  win is capability + dropping Dolphin's GXCopyTex for all colour formats.

## NOT done / next
- Dolphin's `GXCopyTex` is NOT fully droppable yet: the CTF/Z formats (graffito-check, Z) still use
  it. Fully owning them needs ngx to render the graffiti-coverage pass (the parked pollution system)
  and a Z-copy path — out of scope here.
- Differently-VIEWED colour passes (the Sirena hotel MIRROR, RGBA8 reflections) would now be served
  the MAIN scene (wrong) instead of black; the proper fix is still per-epoch side-buffer content
  (handoff task #1), gated on reaching the hotel lobby. Not triggered in reachable scenes.

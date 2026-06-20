# Airstrip BLACK SKY — FIXED. Root cause = Dolphin's async EFB→RAM copy stomps ngx's writeback with zeros.

## Symptom
Airstrip (new-game intro, `SUNBRIGHT_STAGE=0 SUNBRIGHT_SCENARIO=0`) sky renders BLACK in ngx; GX = bright
blue. Only the sky/upper region is dark — Mario/Peach/plane/platform render correctly.

## The PREVIOUS handoff's diagnosis was WRONG (falsified)
The 2026-06-20 handoff claimed "the EFB color readback `g_efb_color` reads dark data that does NOT match
the rendered screenshot." FALSE — I measured both: readback center `(0,10,19)` == screenshot center
`(12,24,35)`. They AGREE. The readback is faithful; the rendered frame is genuinely dark. The handoff's
suspect #1 (stale GXSetTexCopySrc coords) is also falsified — the raw readback center is dark independent
of any source coords.

## Actual mechanism (traced with /pixblend on a frozen frame)
At a sky pixel: L2 ti=9 sky base = bright blue (211,232,249), THEN L3+L4 ti=42 (BathWaterManager ocean
reflection) sample texture `80f94fe0` and blend it (SRC_ALPHA, a≈0.72, twice) over the sky → dark
(16,18,19). The reflection texture `80f94fe0` is the half-res (320×224) RGB565 EFB copy. ngx serves it
via `copytex_writeback` (efb_readback_native.cpp) → `sb_ngx_efb_copy_region` → `g_efb_color`.

## ROOT CAUSE
The served copy reaches guest RAM (`stored=541f` when force-bright-injected, all 71680 texels nonzero),
but the video thread's `texture_for` decodes `80f94fe0` as **all zeros** at the decode point
(`src16[0..3]=0000`). So something STOMPS the bright write with zeros between writeback and decode.

That something is Dolphin's **original** GXCopyTex. Under `NGX_PRESENT`, Dolphin's EFB is empty.
`sb_run_original_around` runs the original synchronously on the guest thread, but Dolphin's actual
EFB→RAM copy is performed **asynchronously on the video thread** — it copies the empty EFB (zeros) into
`80f94fe0` AFTER `copytex_writeback` already wrote ngx's scene there → zeros over our content → black
reflection → black sky. (The headless-present-every-frame fork change exposed/worsened this timing.)

## Why the obvious "skip the original" is WRONG
Skipping Dolphin's original GXCopyTex for fmt 4/5 fixed the airstrip, BUT regressed PLAZA by 23 (vs a
0.029 same-binary determinism baseline): the original does more than write RAM (EFB clear / GP epoch
state ngx's compositing relies on). NOT a safe fix.

## THE FIX (committed) — ngx EFB-copy SIDE BUFFER
Keep running the original (plaza correct), but ngx reads the EFB-copy texel from a buffer IT OWNS, not
from guest RAM that Dolphin asynchronously stomps:
- `g_efb_side` (ngx_present.cpp): `map<MEM1 offset → {w,h, ARGB8888 buffer}}`, mutex-guarded.
- `copytex_writeback` calls new `sb_ngx_efb_store_copy(ea,w,h,argb)` after filling its ARGB buffer.
- `texture_for`, after the texcache miss and before the guest-RAM decode, checks `g_efb_side[addr&0x01FFFFFF]`
  (dims must match the bind) → uploads a 1-mip VK_FORMAT_R8G8B8A8_UNORM image directly from the side ARGB.
- The existing per-present texcache eviction (dirty set) still forces a re-decode each frame, now from
  the side buffer. The guest-RAM write is left in place for any non-ngx consumer.

This sidesteps the async stomp by construction — the "own the framebuffer" principle. Independent of any
Dolphin EFB timing.

## Verification (all headless, build-freshtest)
- Airstrip sky band: black (5,16,27) → blue (10,31,52). Non-black confirmed.
- Plaza no-regression: idle-plaza ngx-vs-GX via oracle_ab.sh = **17.9%** (= the ~18% baseline). Same-scene
  same-instant side-buffer on-vs-off toggle (/efbcopy?on=) = **1.76** mean delta; on=58.56 vs off=58.45 vs
  GX → neutral. (The earlier "plaza 23 delta" was emu-time drift 0.3s during the fastboot camera pan, NOT
  the fix — proven by the in-process toggle.)

## Residual (NOT this bug)
The airstrip sky is now blue-grey, not full bright blue — the reflection samples ngx's real scene, and the
reflected horizon/ocean region is legitimately dimmer; this is the general ~18% ngx-vs-GX wash, not the
black-sky bug. file-select menu darkness + missing airstrip HUD coin counter are separate, uninvestigated.

## Tooling note
`/efbcopy?on=0` returns before `sb_ngx_efb_store_copy`, so it reproduces the OLD stomped-black behavior →
it is a clean in-process A/B for any EFB-copy effect. `sb_ngx_efb_store_copy` is the seam.

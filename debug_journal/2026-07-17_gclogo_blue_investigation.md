# 2026-07-17 — GC-logo "blue" defect: localized, constant verified FAITHFUL, root cause still OPEN

Investigated the user-reported "Nintendo logo shows BLUE" defect (codemap GC-logo row).
Captured headlessly (per codemap rule — no hand-debug). NO code changed: the obvious
"fix" is a bandaid that diverges from faithful retail (see below), so it was NOT applied.

## What renders, and where the color comes from

- Boot draws the Nintendo logo via `TGCLogoDir` (`reference/sms/src/System/GCLogoDir.cpp`).
  `setup()` creates `unk20 = new TNintendo2D(unk34)` where `unk34` = texture
  `/nintendo/timg/nintendo_376x104.bti`; `unk30` = `title_dolby_mark.bti` (Dolby logo).
- `TNintendo2D::perform` (GCLogoDir.cpp:26-59) draws a quad: `GXSetTevOp(GX_MODULATE)`
  (tex × ras), ras = vertex color emitted by `GXColor1u32(unk24)`.
- `unk24` is a `JUtility::TColor`, defaulting in the ctor (GCLogoDir.hpp:21) to
  `TColor(0, 70, 255, 255)` = **blue**. `direct_nlogo()` NEVER writes unk24; it is set to
  white ONLY at direct():151 for the state-0→1 (Dolby) transition. So during the Nintendo
  logo phase unk24 stays blue.

## Captured evidence (scratch/pndump, scratch/oracle/logo_probe)

- MY build (`SB_NO_FASTBOOT=1`, `SB_DUMP_FRAME_EVERY`): renders the classic **"Nintendo®"
  rounded-pill wordmark in solid BLUE** (lit-pixel mean R0 G61 B222 ≈ (0,70,255)) at
  present ~10-40, then a gray element ~55-85, then the title (~325+). scratch/pndump/mcard.png.
- ORACLE (dolphin_fork boot from power-on, framedump): first visible content (e15-e30) is
  **"Nintendo Presents / SUPER MARIO SUNSHINE" text in WHITE/GRAY** (R=G=B, fading in), then
  a big **RED brush "M"** grows over it (f40-f120). e0-e14 are fully black. NO blue pill
  appears in the captured range. scratch/oracle/logo_probe/{e30,f90,f120}.png.

## Verified FAITHFUL (why the naive fix is a bandaid)

- `unk24=(0,70,255)` is retail's REAL ctor default: byte pattern `00 46 FF FF` appears
  EXACTLY ONCE in scratch/bin/sms.dol (`strings`/byte-count) — it is the TNintendo2D ctor
  immediate, not a decomp guess. Angle-bracket-style placeholder ruled out.
- No endianness bug in the color path: `TColor::toUInt32()` is byte-explicit
  `(r<<24)|(g<<16)|(b<<8)|a` = 0x0046FFFF (matches GC), and the FIFO write is consistent.
  So my build FAITHFULLY renders the blue that unk24 specifies — retail's unk24 is the same
  blue, so on faithful replay retail would render blue too.
- Therefore changing unk24→white would make the port render differently from the retail
  constant = a hand-tuned bandaid. NOT applied.

## The OPEN puzzle (next-iteration entry point)

Retail's unk24 is blue (DOL) and the decomp faithfully keeps it blue, YET the oracle's
first visible boot content is WHITE "Nintendo Presents SUPER MARIO SUNSHINE" text (+ red M),
not a blue pill. Reconcile before any fix. Hypotheses, in priority:
1. **Image identity**: is `nintendo_376x104.bti` the "Nintendo®" pill (what my build draws)
   or the "Nintendo Presents…" text (what the oracle draws)? Sharpening: TNintendo2D's rect
   `unk14=(133,170,509,274)` (376×104 center band) is EXACTLY where BOTH my blue pill AND
   the oracle's white "Nintendo Presents" text sit — same texture+rect, different image. So
   either my build decodes the WRONG image from the .bti (texture-decode/asset bug), or my
   present-25 pill is a DIFFERENT boot phase than the oracle's e15 text (the oracle's pill
   phase, if any, was black in e0-e14 / not rendered by the fork). NEED a BTI decoder + arc
   extractor (not yet in tools/) — build one (workflow-first) and view the .bti to settle it.
2. **Director sequence**: which director draws the RED-M "presents" card? GCLogoDir draws
   only the pill + Dolby. The red-M card may be a THP movie (Entrance.thp = movie 9) or a
   title intro. My build may show the pill where retail shows the red-M card because that
   card's director is unported/skipped. Trace the APP_STATE / director order at boot.
3. **Object visibility**: does retail actually DRAW the blue TNintendo2D pill on screen, or
   is it culled/offscreen/overdrawn while a separate white-text object shows? Disasm
   TNintendo2D::perform (0x802963f0) + the state machine for draw gating.

Do NOT change unk24 until 1-3 are resolved. The user's "blue is a defect" is ground truth
(believe it) — but the root cause is upstream of the (faithful) blue constant.

## Tooling gap surfaced (workflow-first for next time)
- No BTI/TIMG decoder or nintendo.arc extractor in tools/. Build a `tools/decode_bti.py`
  (BTI header → RGBA PNG) + arc extraction so logo/2D texture identity is checkable without
  guessing. This blocks resolving hypothesis #1.

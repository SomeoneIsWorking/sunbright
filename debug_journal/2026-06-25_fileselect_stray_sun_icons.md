# File-select stray sun icons — crb2/crc2 FourCC stride typo (2026-06-25)

## Symptom
Two white sun-spiral sprites flanked the file-select NEW slot labels (between A–B and
B–C), each a 52×52 RGB565 quad with a faint dark box. The GX oracle
(`scratch/oracle/fileselect_gx_oracle.png`) shows NONE there (palm/sky).

## Root cause (NOT what the handoff guessed)
The prior handoff's PRIMARY LEAD — "fileblock appear-sparkle JPA particle emitted but never
expired" — is **REFUTED**:
- Traced `TEmitterViewObj::perform` (SB_EMIT_DBG): `gpEmitterManager4D2` is ALWAYS empty
  (nemit=0). The perform-list dispatch delivers BOTH calc (0x2) and draw (0x8) correctly.
- SelectDir's own two emitter managers are not-yet-ported (not built).
- `TFileLoadBlock::pushed()` (the only `emit(0x6E)`) is never called at idle.
So the suns are NOT JPA particles at all.

They are J2DPictures from `load.blo`: the per-slot corrupt/sun markers `cra2`/`crb2`/`crc2`
(`load_sun_1.bti`). `TCardLoad::initLoadPane` (CardLoad.cpp ~line 207) hid them with
`unk28->search('cra2' + i)->hide()`. `'cra2' + i` = cra2/cra3/cra4 — but the slots stride the
**3rd FourCC byte** (a/b/c), i.e. `+ i * 0x100` (= cra2/crb2/crc2), exactly like the
`cra1`/`crb1`/`crc1` lookup two lines up (`'cra1' + i * 0x100`). cra3/cra4 don't exist
(`search` → "region-tolerant dummy"), so only cra2 got hidden; crb2/crc2 stayed at their
`.blo`-default VISIBLE state → two stray suns. Decomp transcription typo (the real GameCube
hides all three; oracle confirms none render).

## Proof (the decisive tool)
`SB_LOAD_PANE_DBG=1` full pane-tree dump at settled (`scratch/frames/fs_panes2.log`):
```
'cra2' vis=0   'crb2' vis=1   'crc2' vis=1     (all 52×52, bounds (-20,-29,32,23))
'cra1' vis=0   'crb1' vis=0   'crc1' vis=0
[j2d] search MISSING pane tag 'cra3' / 'cra4' -> region-tolerant dummy
```
crb2+crc2 visible = the two suns. cra1/crb1/crc1 (the sun-texture markers) are all correctly
hidden by `selectBookmark` — confirmed via SB_SUN_DBG (all slots New: unk0=0 unk18=0,
cra1vis=0).

## Fix
`reference/sms` 6378da9 (parent 667f9eb): `search('cra2' + i * 0x100)->hide()`.
Verified: `scratch/frames/suns_fixed.png` — both suns gone, matches the oracle.

## Reusable
- `SB_LOAD_PANE_DBG=1` dumps the WHOLE load.blo pane tree with vis/alpha/bounds + TBX strings
  at the settled select. FourCC kinds print byte-REVERSED (PIC1='1CIP', WIN1='1NIW',
  TBX1='1XBT'). This is the way to find a stray-visible 2D pane.
- A "stray sun/sparkle" 2D sprite at file-select is a load.blo PIC pane, not a JPA particle —
  all the file-select emitter managers are empty/unported.

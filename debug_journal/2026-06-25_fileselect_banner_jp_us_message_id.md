# 2026-06-25 — File-select banner "Select data" — JP-decomp vs US-disc loadmessage id divergence

## TL;DR
The file-select top banner rendered "Formatting file..." instead of "Select data." The panes
were **fully visible and correctly walked** the whole time — the bug was the **message id**. The
decomp (doldecomp/sms, targets **GMSJ01/GMSP01**) hardcodes loadmessage.bmg id **0x1A** for the
select banner; that is the JP/PAL index of "Select data." But we run against the **US (GMSE01)**
disc, whose loadmessage.bmg orders its SMS-specific strings differently — "Select data." is at id
**0x1D** there (US 0x1A = "Formatting file..."). Fix: a documented JP→US loadmessage id remap in
`TCardLoad::setMessage` (`sb_loadmsg_jp_to_us`), currently mapping the verified select-banner entry
0x1A→0x1D. `reference/sms/src/GC2D/CardLoad.cpp`.

## How this was found (tooling-first, the handoff's NEXT task)
Added `SB_LOAD_PANE_DBG` — a one-shot recursive pane-tree dump of the load.blo screen (`unk28`)
fired once at the SETTLED select sub-state (`mState==0 && unk1C==PROGRESS_UNK13 && unk10==2`),
mirroring SelectMenu.cpp's SB_SEL_DBG dump. It prints every pane's FourCC tag / visibility / alpha /
bounds, and for J2DTextBox ('TBX1') panes the `getStringPtr()` string. Plus a full-table dump
iterating `SMSGetMessageData(unkA0, id)` for id 0..39.

Key output (settled file-select):
- The pane tree is **fully populated and visible**: `.w_0` (top banner window) → `m_0b` → `m_0a`,
  all `vis=1`, `m_0a` alpha=255, bounds=(-3,-3,362,45) inside `.w_0` bounds=(97,70,503,134) =
  on-screen near the top. Slot labels `m_1a[0..2]` vis=1 = "NEW" (hardcoded literal, correct for a
  blank card). So **NOT** a visibility / camera / pane-walk problem.
- `m_0a` string = **"Formatting file..."** ← the bug.
- Full US loadmessage.bmg table (all 35 strings resolve cleanly — the BMG parse is CORRECT):
  - id 0x1a → "Formatting file..."
  - id 0x1b → "New"
  - id 0x1c → "Saving file..."
  - **id 0x1d → "Select data."**  ← what the banner should show
  - id 0x1e → "Where do you want to copy this data to?"
  - id 0x00..0x19 = the standard memory-card system messages.

## Root cause (named, not a deduction)
- The decomp is **JP/PAL** (`reference/sms` = doldecomp/sms; only `config/GMSJ01` + `config/GMSP01`).
  The disc is **US** (`scratch/disc/sms.iso` header = `GMSE01`).
- SMS resolves messages by POSITIONAL index (`SMSGetMessageData` walks INF1 to `entry[id]`), so a
  region that orders its bmg strings differently shifts the id of each string.
- The **system block 0x00..0x19 is region-identical** (verified: literal id 12 → "Erasing file..."
  matches the US bmg exactly; the cMessageID per-state dialog ids in that range are all coherent).
  Only the **SMS-specific block 0x1A+ is reordered** JP↔US.
- The JP literal 0x1A ("Select data." in JP) lands on US "Formatting file...". This is the
  `us-disc-vs-jp-decomp-region-tolerance` class, now pinned with data.

## Fix
`sb_loadmsg_jp_to_us(int jp_id)` (CardLoad.cpp, `SMS_NATIVE_PLATFORM`-only) maps JP loadmessage ids
to US, applied in the `setMessage` funnel so every call site is consistent (JP 0x1A means "Select
data" at both the banner and `cMessageID[0x13]`). Currently maps the VERIFIED entry: 0x1A→0x1D.

### NOT fixed (documented TODO — unverifiable from the blank-card select oracle)
The rest of the 0x1A+ block diverges too, but the strings only appear during card OPERATIONS:
- Erase-mode banner: JP id **0x1B** (selectBookmark case 0, `PROGRESS_UNK1C` = erase-selection
  after `makeBlockRock`). US value unknown — needs driving the erase flow on a card WITH data.
- `cMessageID` copy/erase/format/save DIALOG strings using JP 0x1B/0x1C/0x1E/0x1F (shown in the
  hidden `.w_7`/`w_10` confirmation windows during those operations).
- Hardcoded JP corrupt label "こわれています" (selectBookmark case 1, line ~1990) → US "Corrupt"
  (only shows for a corrupt file; our blank card shows "NEW" for all 3, which matches a blank-card
  state — the oracle's "Corrupt" is a save-data difference, not a render bug).
These need a card-with-data + per-operation oracle before they can be mapped and verified. They do
NOT appear on the normal select screen, so the banner fix is the complete fix for the visible
divergence.

## Diagnostics added (committed, env-gated)
- `SB_LOAD_PANE_DBG=1` — one-shot load.blo pane-tree dump + full loadmessage.bmg table at the
  settled select state (unk10==2). The way to SEE pane strings/visibility without guessing.

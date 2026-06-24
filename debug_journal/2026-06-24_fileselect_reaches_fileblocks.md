# 2026-06-24 — file-select (stage 15) reaches the UNK13 file-block screen + RENDERS, no crash

## Milestone
The real file-select (gameplay STAGE 15) now runs the **full card path** end-to-end and
**renders the file-block select screen** with NO crash:

  boot → GC logo → title → camera pan → blank-card detected → **format dialog** → `format()`
  writes a valid FS → re-read returns READY → **PROGRESS_UNK13** (file-block select) →
  3 TFileLoadBlocks (purple bars) + shine/sun icons + "→ OPTIONS" all render.

Frame: `scratch/frames/boot_0044.png` (dumped via the new UNK13 trigger). Commits: sub
`572f16f`, parent `a0ac593` (folds the gitlink bump + the watchdog crash-handler).

## Three crashes fixed along the path (all region-tolerance / init-skip class)
1. **format-dialog `waitForChoice` (prior session, now committed):** `setMessage` null-buffer
   guard + `unkAC` sparkle-emitter null guard let the "There is no data — format the card?"
   dialog (PROGRESS_UNK6→UNK8) run. Driving LEFT→A through both confirmations calls
   `TCardManager::format()` → native `CARDFormat` writes a valid FS to `scratch/memcard_chan0.raw`.
2. **`TCardLoad::perform`/`changeScene` null `unk284`:** the option-wall collision object
   `TNameRefGen::search<TMapObjOptionWall>("オプション用壁")` is **absent in the US (GMSE01)
   file-select scene** → `unk284` stayed null → `unk284->offCollision()` deref'd `null+0x68`
   (offCollision = `unk68->remove()`). Guarded all 4 call sites (560/682/2679/2696); the
   option-wall collision toggle is cosmetic for this screen.
3. **`TCardManager::buildHeader_` null `mBanner`/`mIcons`:** loaded from `/card/*.bti` in the
   nlogoAfter boot-init (`Application.cpp:414`), which the `SB_STAGE=15` direct jump **skips**,
   so they stayed null → `memcpy` from null. Write a blank banner/icon (cosmetic GC-BIOS file-
   manager metadata) when the source is null, loudly logged. Proper fix noted in-code: run the
   `.bti` resource load before this.

## Key realizations
- **`J2DScreen::search` already returns a non-null region-tolerant dummy pane** under
  `SMS_NATIVE_PLATFORM`, and `TExPane` ctor uses it — so the journal-claimed "null pane in
  waitForChoice" was NOT the crash. The real crashes were the three above. Static-read the
  guards before assuming the prior diagnosis.
- **The blank card is INTENTIONAL** (`card_impl.cpp`: all-0xFF card → `CARD_RESULT_BROKEN` to
  exercise the game's real format flow, "identical to inserting a new physical card"). Once
  formatted, `scratch/memcard_chan0.raw` PERSISTS as formatted → subsequent runs mount READY
  and go straight to UNK13 (no dialog). To re-test the format dialog, delete that file.
- **Dialog input mapping** (`MarioGamePad::updateMeaning`, PAD_FLAG_0x1 menu mode set at
  `changeScene` PROGRESS_UNK0): checkFrameMeaning 0x8=LEFT, 0x10=RIGHT, 0x20=A, 0x40=B. The
  cursor defaults to option 1 ("no"); LEFT→option 0 ("format"), A confirms. **GOTCHA:** case-2's
  checks are an else-if chain with 0x8 (LEFT) before 0x20 (A) — pressing LEFT+A together eats
  the A. Release LEFT before pressing A.

## Repro (card already formatted → straight to UNK13; ~3 min)
    pkill -9 -x sms-boot; S=""; for f in $(seq 200 12 1700); do S="$S ${f}:START $((f+6)):-"; done
    timeout -s KILL 260 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
      SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_SEL_DBG=1 SB_SEL_DUMP=1 \
      SB_PAD_SCRIPT="$S" ./build-native/sms-boot > scratch/frames/fs.log 2>&1
    # SB_SEL_DUMP now dumps a 40-frame window when unk1C reaches UNK13 (boot_0001..0044).
    # To re-test the format dialog: rm scratch/memcard_chan0.raw first, and drive LEFT/A
    # through both UNK6+UNK8 confirmations (see "Dialog input mapping" gotcha above).

## RESIDUAL / NEXT
- **Scattered title-logo letters** (chunky T/B/A/O/O around the blocks) linger in the file-
  block frame — the title "SUPER MARIO SUNSHINE" logo isn't fully hidden/flown-off when the
  file-select takes over. Fidelity gap, NOT a crash. Likely the title-logo panes/3D letters
  aren't hidden on the title→select transition.
- **`[hx_wipe] UNIMPLEMENTED wipe type 10`** fires at the format-dialog→file-select transition
  (a warning, no crash) — port `table1[10]`'s wipe callback from the DOL when fidelity needs it.
- **File selection → gameplay:** UNK13 → `selectBookmark` → file chosen → `setupScoreScreen`
  (score panel: shine count digits via `unkC8`, per-stage shine icons `unk584`) → `setNextArea`
  → start gameplay. Drive a file pick (A on a block) and own the next crash/render gaps.
- **Proper banner/icon load:** run the `/card/*.bti` resource load (Application.cpp:414 path)
  under the stage jump so save files get a real GC-BIOS banner (cosmetic, low priority).

## Navigation probe (later this session) — scene IS navigable, selection needs telemetry
Added diag (sub `a8dfad1`): `TFileLoadBlock::pushed`/`touchPlayer` traces (SB_SEL_DBG) +
`SB_SEL_DUMP_N` to set the UNK13 dump-window length. Drove headless movement+jump after UNK13
(`SB_PAD_SCRIPT` UP+A sweep) and dumped 200 frames:
- **Mario IS present & the scene responds:** `scene_verts` jumps 1866 (static) → 4803 during
  movement (≈3000 verts = Mario's model); the camera pans as input drives it.
- File selection = **Mario head-butts a floating block** (`touchPlayer`→`marioHeadAttack()`→
  `pushed()`→`gpCardLoad->setSelected(unk138)`), exactly like hitting a ? block. `setSelected`
  (CardLoad.cpp:2168) sets `unkB0` → `selectBookmark` case 2 advances (unkB0 != -1).
- **My blind UP went the WRONG way** — toward the **OPTIONS wooden signpost** (visible in 3D at
  `boot_0190.png`), the opposite side from the file blocks. `touchPlayer` never fired.
- **Conclusion:** driving file-selection needs POSITION TELEMETRY (Mario world coords + the 3
  `unk278` block coords `unk144`), not blind input. Build a probe that prints Mario's pos and the
  block positions, then script movement toward a block + jump. THAT is the next porting unit
  (file-selection → `selectFunction` → `setNextArea` → gameplay), and it wants a fresh context.

## Open fidelity items seen in the frames (separate from the card path, NOT crashes)
- **Title-logo letters linger** (chunky outlined "S.M.S." letters scattered around the blocks,
  e.g. `boot_0100.png` "T O O B A"): the title→file-select transition doesn't fly the 3D logo
  letters off / hide the title-logo object. RE the title-logo flyout.
- **Striped rendering artifact** bottom-left in `boot_0190.png` (beach sand under the moved
  camera) — possible z-fight / bad normal / texgen; investigate with the ngx tooling.

# 2026-06-23 — File-select port, milestone 4 (stage 1): setup save-data population

Continues `2026-06-23_fileselect_textured_2d.md` (milestone 3: textured 2D renders, but the
menu was STATIC at .blo defaults). This stage ports the foundation of `TSelectMenu::setup`
(DOL @0x8017449c) — the per-file pane-visibility decisions — fixing the headline residual:
**"EPISODE 1" rendered TWICE.** Boot-order port (`port-in-boot-order-not-delfino`). No recompiler.

## Verified result
`scratch/frames/stage1_s4.png` (SB_STAGE=4): a SINGLE centred "EPISODE 1" window (was doubled),
and the banner now reads **GELATO BEACH** (correct for stage 4) instead of always BIANCO.
ctest -E platform_test = 28/28. Submodule 9586fa9 / parent 8c539f8.

## Root cause of the doubled EPISODE
The scenario_select_1.blo has TWO file-window groups, `.s_0` (anchor 300,299) and `.0_0`
(444,299), each with its own s_1a/s_1b/s_2a/s_2b window halves + an `sttx`/`0ttx` textbox =
"EPISODE 1". Both are VISIBLE at the .blo default. The DOL `setup`'s very first action hides
`0_0` (`*(0_0 + 0xC) = 0`), leaving the single centred `s_0`. Milestone 3 never ran setup, so
both drew → doubling. Fix = port that hide (+ the rest of setup's resting visibility).

## What landed (faithful, against the real J2D APIs)
- **hide `0_0`** → single EPISODE window.
- **Stage banner**: hide default `bi_0`, show the banner group + its halves (`mm_a`/`mm_b`…)
  for THIS stage, from the DOL per-stage 3-char-prefix table @0x80388308 (banner tag =
  `(prefix<<8)|0x30`). Stage→prefix: 2 bi_, 3 rc_, 4 mm_, 5 pi_, 6 sr_, 8 mo_, 9 mr_.
  Banner now tracks the stage (previously always BIANCO via the .blo default).
- **episode state[8] + numSlots + cursor** from save flags via `SMS_getShineStage` +
  `SMS_isGetShine` (existing StageUtil): default OPEN(2), each collected shine → CLEARED(3)
  and extends numSlots = idx+2, rest → LOCKED(0); clamp [1,8], cursor = numSlots-1. Added
  `u8 mEpisodeState[8]` to TSelectMenu. (Fresh save at stage 4 → numSlots=1, states=2,0…0.)
- **hide `sc_s`** (the 100-coin mark; revealed later by the score-mark table — TODO).
- Reusable `SB_SEL_DBG` recursive pane-tree dump in setup (tag/kind/vis/alpha/bounds).

## Key findings (reusable)
- **J2DScreen::search resolves the JP-decomp tags** (`"s_0"`=0x00735f30, `"0_0"`=0x00305f30,
  `"msk1"`, `"s_2a"`, `"i_o0"`, …) against the loaded US .blo panes — PROVEN by live probe
  (`mScreen->search(tag)` returns the real pane, vis matches the tree). So the whole setup CAN
  be ported against `search()`; the region-tolerant dummy is only hit for genuinely-absent tags.
  (Caveat: reading `p->mUserInfoTag` raw in the dump showed a bogus '.' prefix — host LP64
  layout ≠ DOL offsets; trust the ACCESSORS / search(), not raw `*(p+0x10)`.)
- **⚠ SB_STAGE=4 is GELATO BEACH (mm_), not Bianco.** `local_288[stage]` (banner) and the
  gradient `setStageColor` both index by the same stage; Bianco = menu-stage **2** (bi_).
  Milestone 3's "BIANCO HILLS @ stage 4" was the .blo default banner, not the stage's banner.
  To verify a specific course, pick the matching menu-stage. (The SB_STAGE→menu-stage mapping
  in the fastboot wiring may want a look, but it's separate from this port.)
- **SDA bases (from __init_registers @0x8000536c): _SDA2_BASE_ (r2) = 0x80416ba0,
  _SDA_BASE_ (r13) = 0x804141c0.** So `r2-0x47f0`=0x804123b0 etc. Flag-manager global =
  r13-0x6060 = 0x8040e160 (= `TFlagManager::getInstance()`). The setup "float constants" are
  mostly the compiler's int-division idiom: r2-0x47ec=0.1 (÷10), r2-0x47e8=0.01 (÷100),
  r2-0x47f0=1.0 (frameScale numerator + insert arg). Port the integer semantics, not the floats.
- **DOL data extraction**: `sunbright-recomp <disc> --dump-dol --output scratch/sms.dol`, then
  `scratch/dolread.py {hex|u32|u8} <va> [n]` (parses the DOL section table, VA→file-offset).

## NEXT (stage 2, same unit)
- Score/coin digit display: load coin_number_0..9.bti via `JKRFileLoader::getGlbResource` +
  `JUTTexture::storeTIMG`, then `J2DPicture::changeTexture(sc_1/sc_2/sc_3, coin[digit]->getTexInfo(), 0)`
  with integer digit decomposition; count = `getFlag(shineStage + 0x20005)` clamped 0..999.
  ⚠ VERIFY the getGlbResource path BE-swaps the standalone ResTIMG (milestone 3 only wired the
  swap into JUTResReference::getResource for 'TIMG'); an unswapped ResTIMG → the 2048×2048 SEGV.
- sc_s score-mark + r_i/r_s2 rank visibility need `local_2e0[stage]` (special-shine id arrays,
  ptrs @0x80388360 → e.g. stage 4 = 0x80412338, ~12 bytes each) — extract with dolread.py.
- BMG stage/scenario-name strings (sttx/0ttx setFont + the scenarioname.bmg lookup) —
  DAT_803c0cc8 (stage→name-array) + DAT_803c0d18 (bmg id table).
- The window-open animation + input navigation (perform's calc, 10-state machine @0x80172c90)
  + the i_o*/i_e* slot-indicator row (revealed by the animation; uses mEpisodeState).

## Verify loop (watchdog ON — do NOT disable it)
    cmake --build build-native --target sms-boot -j$(nproc); ctest --test-dir build-native -E platform_test
    pkill -9 -x sms-boot; (timeout 100 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso \
      SB_THP_FAST=1 SB_HOST_ALLOC_CAP_MB=3072 SB_FILESELECT=1 SB_STAGE=4 SB_SEL_DBG=1 \
      SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=400 SB_FRAME_DUMP_MAX=2 ./build-native/sms-boot \
      > scratch/frames/fs.log 2>&1 &); sleep 88; pkill -9 -x sms-boot
    # boot_0401.ppm → PIL. SB_SEL_DBG: pane-tree dump + setup summary line.

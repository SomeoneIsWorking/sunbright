# 2026-06-23 — File-select port (boot order), milestone 2: menu windows render

Continues `2026-06-23_fileselect_port_grad.md` (milestone 1: gradient). USER directive
(memory `port-in-boot-order-not-delfino`): port sms-boot in BOOT ORDER. Next unit was
TSelectMenu (the file-slot windows).

## What landed
`TSelectMenu` (the `scenario_select_1.blo` J2DScreen) is now reconstructed and wired in.
The select screen draws the file-slot windows over the animated gradient.
`scratch/frames/menu_preview.png` is the verified output (windows render at the correct
.blo layout — top scenario banner, middle shine row, bottom coin/file row).

Files (reference/sms submodule):
- `include/GC2D/SelectMenu.hpp` — NEW. `TSelectMenu : JDrama::TViewObj`. PC-native object
  (host new / host sizeof, never read by guest code → host layout, NOT the DOL's 0x170
  offsets — same as TSelectGrad). Members named by meaning with the DOL offset in a comment.
- `src/GC2D/SelectMenu.cpp` — added TSelectMenu ctor (@0x801753d0), setup (@0x8017449c),
  perform (@0x80172c90, TViewObj vtable slot 8).
- `src/GC2D/SelectDir.cpp` — rsetup now builds Group 2D + TSelectMenu + Screen 2D (TOrthoProj)
  alongside the gradient sub-scene; direct's first-frame block calls `mSelectMenu->setup`.
- `src/System/Application.cpp` — SB_FILESELECT path now honours `SB_STAGE` (the stage id
  passed to TSelectDir picks the select variant; stages 0/1/10 suppress the windows, >=2
  build them — so the menu needs SB_STAGE>=2 to be visible).

## Control-flow / RE (Ghidra + recomp disasm)
- TSelectMenu vtable (DOL 0x803c0e58) slot 8 (+0x20) = perform @0x80172c90. (Found by
  matching TSelectGrad's vtable, whose known perform 0x80175560 sits at the same slot.)
- perform: `flags & 0x1` = calc (a 10-state window-open + input state machine, cases 0-9 —
  NOT YET PORTED, needs the per-file panes); `flags & 0x8` = draw, in state [0,10):
  `ReInitializeGX(); SMS_DrawInit(); J2DOrthoGraph graph(gfx->getViewport()); graph.setup2D()
  ×2; mScreen->draw(0,0,&graph);` (FUN_802ecfcc=J2DOrthoGraph ctor, 802eb6bc=setup2D,
  802cfda8=J2DScreen::draw). The menu builds its OWN J2DOrthoGraph from the graphics viewport,
  so the Screen 2D's TOrthoProj only fixes that viewport; near/far ±100 keeps pane z in range.
- setup (@0x8017449c, 3892 B): early-bail `stage==10 || stage<2` → mDisabled=1, no screen.
  Else `new J2DSetScreen("scenario_select_1.blo", archive)`, then ~3.5 KB of per-file
  save-data population (sc_number/coin_number/sc_mark J2DPictures, i_o* windows, visibility
  from TFlagManager). PORTED: store stage/shineMgr/dir, mFrameScale=1.0/SMSGetAnmFrameRate(),
  load the screen. DEFERRED (TODO): the save-data population + the open-animation panes.
- direct's first-frame (DOL @0x80175ec4): setup(menu) → FUN_8017443c (=TSelectShineManager::
  initData) → startOpenWindow (@0x80172990). Only setup is ported here; the shine-mgr init +
  startOpenWindow + the calc animation are deferred (the menu renders static at .blo defaults).
- rsetup args to setup: `setup(this->mStage(0x40), this->mArchive(0x2c),
  this->mSelectShineMgr(0x28), this)`. menu->unk100(0x100) = TSelectDir gamepad (0x18) — the
  handoff's "unk18=FrmGXSet/display" was wrong; 0x18 is mGamePad (perform case 6 reads pad
  buttons from it). mSelectShineMgr is null for now (shine mgr not built) — setup only stores it.

## RESIDUAL (the next unit): textured 2D — windows render solid WHITE
The windows are at the right place/size but draw as solid white boxes. ROOT CAUSE: the
sms-boot immediate-mode 2D path (`native/platform/gx_imm_impl.cpp`) captures **position +
colour only** — NO texcoords, NO GXLoadTexObj/TEV. So every textured J2D pane (J2DWindow
9-slice borders, J2DPicture digits, J2DTextBox glyphs) draws untextured → white. The gradient
(vertex-colour only) renders fine; the textured panes do not.
NEXT: add textured 2D to gx_imm — capture GXTexCoord + the bound GXTexObj/TEV order, decode
the J2D textures (sb_tex_decode), upload, and sample in the nvk 2D shader. (cf. the OLD ngx
renderer's identical "white windows = texture block-padding UV leak" fix, memory
`fileselect-fixed`/the CLAUDE.md note — but that was runtime/render; this is the sms-boot
native gx_imm/nvk path, a fresh implementation.) After textures: the open animation + input
navigation (perform calc), TSelectShineManager + particles, file-pick → gameplay.

## Verify loop
    cmake --build build-native --target sms-boot -j$(nproc)
    ctest --test-dir build-native -E platform_test     # 28/28
    pkill -9 -x sms-boot; (timeout 120 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso \
      SB_THP_FAST=1 SB_WATCHDOG_SECS=0 SB_HOST_ALLOC_CAP_MB=3072 SB_FILESELECT=1 SB_STAGE=4 \
      SB_SEL_DBG=1 SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=400 SB_FRAME_DUMP_MAX=3 \
      ./build-native/sms-boot > scratch/frames/fs.log 2>&1 &)
    # boot_0402.ppm → PIL preview. SB_SEL_DBG traces TSelectGrad/TSelectMenu::perform.

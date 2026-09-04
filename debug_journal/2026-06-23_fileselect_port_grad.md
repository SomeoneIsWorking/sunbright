# 2026-06-23 — File-select port (boot order), milestone 1: gradient renders

USER directive (memory `port-in-boot-order-not-delfino`): port sms-boot in BOOT ORDER
(logo→title→file-select→gameplay), not via plaza fastboot. Next unit = file-select.

## What landed (committed: submodule ed9517e, parent 2ff3c34)
`SB_FILESELECT=1` reached file-select but rendered BLACK because `TSelectDir::setup/direct`
were empty stubs (`native/boot_stubs/ui_map_stubs.cpp`) — the GC2D/SelectDir.cpp,
SelectMenu.cpp TUs are un-decompiled (file-select only ever ran under Dolphin's JIT in the
hybrid build). Reconstructed faithfully from the DOL:
- `reference/sms/src/GC2D/SelectDir.cpp` — TSelectDir setup/setupThreadFunc/rsetup/direct/changeOrder.
- `reference/sms/src/GC2D/SelectMenu.cpp` + `include/GC2D/SelectGrad.hpp` — TSelectGrad (the
  animated background gradient).
- removed the TSelectDir stub from ui_map_stubs.cpp.
Result: file-select renders the full-screen animated diagonal gradient (mean 0 → 127, max 255,
ctest 28/28). `scratch/frames/grad_preview.png` is the verified output.

## Control-flow map (verified via Ghidra + recomp disasm)
- `TApplication::proc` APP_STATE_TITLE: `new TSelectDir; selectDir->setup(display, pad, stage)`
  then `gameLoop()` calls `mDirector->direct()` each frame. (Application.cpp:646)
- `setup` @0x80177400: store display(0x1c)/pad(0x18)/stage(0x40); `OSCreateThread(gSetupThread,
  setupThreadFunc)`, `OSResumeThread`.
- `setupThreadFunc` @0x801773e0: just `rsetup(this)`.
- `rsetup` @0x801761b0 (4.4 KB): mount `/data/select.arc`; build root "View Objs" + TDStageGroup
  (unk10/unk14); TSelectMenu(0x170)@0x20, TSelectShineManager(0x120)@0x28, TSelectGrad(0x24)@0x24;
  groups "Group 3D/2D/Grad/2D Particle"; 2× JPAResourceManager + emitters@0x30/0x34; TDStageDisp;
  5× TScreen (Screen_Grad/2D/3D + dup 2D/Grad) each with TOrthoProj/TLookAtCamera; wire via
  assignCamera/assignViewObj. Member layout in SelectDir.hpp.
- `direct` @0x80175ec4: while !mSetupDone, wait `OSIsThreadTerminated(gSetupThread)`+`OSJoinThread`,
  then first-frame TSelectMenu::setup(0x8017449c)+startOpenWindow(0x80172990)+fader startWipe;
  per-frame `FUN_802f7d28` = `JDrama::TDirector::direct()` (testPerform(3) on unk10 = calc,
  testPerform(8) on unk14 = draw). NO perform-list AND-masking (unlike TMarDirector) — the draw
  Just Works once the scene is built.

## KEY GOTCHA — ortho near/far z-clip (the black-after-wiring root cause)
The grad TOrthoProj must have `mNear=-100, mFar=100` (rsetup writes these AFTER the ctor:
`local_58[0xa]=-100, [0xb]=100`). The TSelectGrad quad sits at **z=-100** (corners
(0,16,-100)…(600,464,-100), ortho 0/16/600/464). `JDrama::TOrthoProj`'s ctor defaults to
`TCamera(-1,1)` → z=-100 is outside [-1,1] → Vulkan z-clips the quad → black. Fix: set
`gradCam->mNear=-100; gradCam->mFar=100;` (public members). General lesson: any reconstructed
ortho whose content has non-trivial z needs the DOL's real near/far, not the ctor default.

The gradient draws via GX immediate (GXBegin/GXPosition3f32/GXColor3u8), captured by the native
gx_imm path (`native/platform/gx_imm_impl.cpp`) and composited by `sms_boot_present.cpp`. The
screen's TOrthoProj::perform(0x10) sets GXSetProjection(ORTHO) before the group draws.

## Reusable Ghidra-decomp tooling (THIS is the force multiplier — reuse it every boot-order unit)
The community decomp is missing whole TUs (all of Select*). Ghidra headless gives readable C.
Note (2026-07-04): the old flat-BinaryLoader path below is superseded — install the Cuyler36
GameCube-Loader extension (matches installed Ghidra version) and `analyzeHeadless <proj> <name>
-import scratch/bin/sms.dol -loader-autoloadMaps false` imports the DOL natively (real section
addresses, `Gekko_Broadway` sleigh). See sunbright decomp-port SKILL.md for details.

Legacy (Ghidra 11.x, no extension):
1. `python3 scratch/dol2flat.py` → `scratch/bin/sms_flat.bin` (flat image of sms.dol, base 0x80003100).
2. Import+analyze: `analyzeHeadless scratch/ghidra_proj sms -import scratch/bin/sms_flat.bin
   -processor "PowerPC:BE:32:default" -loader BinaryLoader -loader-baseAddr 0x80003100` (the project
   is CACHED in scratch/ghidra_proj — skip re-import).
   GOTCHAS: NO `Gekko_Broadway` lang in Ghidra 11.0.3 (use `:default`); `_JAVA_OPTIONS` /
   `-Djava.io.tmpdir` flags break the launcher (don't pass them).
3. `scratch/CreateAndDecomp.py` (postScript): reads `scratch/all_funcs.txt` (= funcs.txt addrs),
   CREATES functions at every known boundary (Ghidra misses vtable-only fns like `direct`), then
   decompiles `DECOMP_TARGETS` → `scratch/decomp/<addr>.c`. `_SDA2_BASE_=0x80416ba0` (read SDA2
   float constants at `base+off` from sms_flat.bin).
Anchors come from `reference/sms_gmse01_funcs.txt` (partial map), direct disassembly, cross-references,
and callee graphs. Cross-reference Ghidra C against sister native code (MenuDir.cpp/MovieDirector.cpp) = Rosetta.

## NEXT (the loop continues — tooling→RE→own)
- TSelectMenu (file windows): ct 0x801753d0, setup 0x8017449c (3892 B), startOpenWindow 0x80172990,
  get{Prev,Next}Index 0x80172bdc/0x80172c34. Loads J2DScreen file-slot windows from select.arc.
  rsetup creates it (0x170 B) into Group 2D + Screen 2D. Reconstruct → file slots render.
- TSelectShineManager: ct 0x80178eb4, initData 0x8017894c, start{Decrease,Increase,Close}.
- The two JPAEmitterManager particle sets + Group 3D/2D-Particle.
- Then changeOrder's screen unkC draw-order dance (2D vs grad) becomes relevant (currently grad
  screen unmasked = always drawn; with the 2D menu present, replicate rsetup's end masks).
- direct's menu/input transition + file-pick → APP_STATE_GAMEPLAY.

# Z guide transition is an unported UI path

## Symptom

In decomp + Aurora gameplay, pressing keyboard C (the Aurora binding for GameCube Z)
begins the guide/map transition but shows a black screen instead of the retail map-screen
animation.

## Reproduction and control correction

The first bounded run injected Z at retrace 120:

```sh
gpuguard run --timeout 150 -- ./run-safe.sh SB_RUNNER=run.sh SB_STAGE=1 \
  SB_SCENARIO=0 SB_PAD_SCRIPT='120:Z 124:-' SB_QUIT_AFTER=360
```

That timing was before fastboot entered gameplay, so it did **not** reproduce the user's
transition. The stub report establishes that the scene's perform list reached `TGuide`, but
cannot be attributed to the injected Z event. A later trace at retrace 600 proves the guide screen
was constructed and the input event fired, but the run was interrupted immediately afterward; it
does not establish a rendered outcome. The user's manual observation remains the symptom evidence.
This correction matters because an input script firing is not proof that gameplay accepted it.

## Cause

`TMarDirector::updateGameMode` tests `mButton.mTrigger & 0x10` and transitions to
`STATE_UNK10`.  `nextStateInitialize(STATE_UNK10)` starts wipe 6, calls
`TGuide::setup(nullptr)`, then calls `startMoveCursor()`.

The relevant retail function is `TGuide::perform` at US GMSE01 `0x801791d0`, size
`0x610`. It advances the guide state and draws the screen. The port defined it as an acknowledged
no-op in `sms-boot/boot_stubs/ring3_stubs.cpp`. The in-progress native port now constructs
retail's `guide_1.blo` screen, resolves the pane ownership tables, records animation bounds, and
implements the state 9 -> 10 fade-in/draw spine. The remaining input/marker branches and
pixel-oracle verification are not yet complete. Neither a frame-seam change nor a threading change
can replace this missing UI behavior: the port unit is `TGuide::load` plus `TGuide::perform`.

The Ghidra decompile used for the conclusion is `build/decomp/801791d0.c`, generated
from the existing `scratch/ghidra_proj/SMS` project.  The result must be verified
against a scripted Dolphin guide transition after the class is ported.

## 2026-08-13 measured update

Fastboot skipped `TCardLoad`'s `TMario::waitingStart`, leaving Mario in
`MARIO_STATUS_DISAPPEAR` and the director in entrance state 2. Carrying that skipped semantic to
`TMarDirector::setMario` admits normal state 4. The next crash was `TConsoleStr::processGo`: it
fell off a non-void function, confused the retail emitter slots with byte fields, and omitted the
DOL's three-glyph Bézier animation. The port now follows US `0x80170d18`, including the 0.5 center
interpolation used to move each `0x1fd` emitter.

Wipe type 10 (`Hx_Test2`, US `0x8017ef1c`) has four control timers: 11, 11, 10, and 12 updates.
The unit test proves exact completion after 44 non-DONE updates. A gameplay-window probe at frame
750 proves Z is accepted (`trigger=0x10`) and guide state advances `9 -> 10 -> 0`, then draws.

The remaining absent visual transition is the known render-only gap in wipe types 5/6:
`test5_callback` advances its exact 20-frame timer but does not emit retail `Hx_Test5`'s sinusoidal
framebuffer grid. Do not replace it with a generic fade; port the GX grid from US `0x8017df74` and
compare intermediate frames to Dolphin.

## Guest-path correction and resolution

The user clarified that the requested defect was in the guest-runtime path, not the native-reference
path. That path executes retail `Hx_Test5`; its black transition had a different cause. Ghidra confirms that
US `0x8017df74` loops over a 10x8 grid, calling helper `0x80182a20` to copy and clear each 64x64 EFB
cell before drawing one 18-vertex strip. Dolphin's `BPStructs.cpp` confirms that the clear applies
to the copy source rectangle. Aurora instead used a render-pass load clear for every clear=true
copy, which erased the whole EFB after every tile.

A scissored clear draw implements partial clears. The first version exposed a second contract bug:
the eighth row starts at y=448 and maps to zero visible height, while the clear draw used an empty
rectangle as its full-clear sentinel. The last tile consequently erased the whole frame. The final
representation has an explicit `rectEnabled` discriminator, and an enabled zero-area clear returns
without drawing.

The historical Dolphin-backed runtime at `9283f44^` still contains fastboot. The reproducible
builder is now `tools/oracle/build_dolphin_fastboot.sh`; it uses the live `extern/dolphin_fork` and
keeps generated source/build output under gitignored `scratch/oracle/`. Its Z transition supplied
the pixel oracle. Matched recomp captures now show both the tiled Plaza close and the tiled Guide
reopen. A forced-magenta clear was the failing control: before the zero-area correction tile eighty
made the entire frame magenta, proving the ambiguous sentinel fired; afterward normal content
survives.

The missing interpolation seam was the retail function itself. The override at `0x8017df74` tags
each GXBegin under `SB_POP_WIPE`. A bounded 60 fps run measured 40 calls, 3,200 strips over ticks
599..638, and the expected 3,120 new consecutive-tick pairs. The bounded 30 fps and 60 fps runs both
exited zero with no kernel-reported amdgpu timeout, reset, or fault. The generic partial-copy clear
also covers the Delfino load wipes; no stage- or Guide-specific exception was added.

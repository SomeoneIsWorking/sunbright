# Z guide transition is an unported UI path

## Symptom

In decomp + Aurora gameplay, pressing keyboard C (the Aurora binding for GameCube Z)
begins the guide/map transition but shows a black screen instead of the retail map-screen
animation.

## Reproduction and control

The bounded, headless decomp run below injected exactly one Z press and kept the GPU
guard active:

```sh
gpuguard run --timeout 150 -- ./run-safe.sh SB_RUNNER=run.sh SB_STAGE=1 \
  SB_SCENARIO=0 SB_PAD_SCRIPT='120:Z 124:-' SB_QUIT_AFTER=360
```

Its log both fired the script events and reported
`[STUB-CALLED] TGuide::perform -- unported, output will be wrong`.  The runner's
kernel check reported no amdgpu timeout, reset, or fault.

## Cause

`TMarDirector::updateGameMode` tests `mButton.mTrigger & 0x10` and transitions to
`STATE_UNK10`.  `nextStateInitialize(STATE_UNK10)` starts wipe 6, calls
`TGuide::setup(nullptr)`, then calls `startMoveCursor()`.

The relevant retail function is `TGuide::perform` at US GMSE01 `0x801791d0`, size
`0x610`.  It advances the guide state and draws the screen.  The port currently
defines it as an acknowledged no-op in `sms-boot/boot_stubs/ring3_stubs.cpp`.
Moreover, the partial native `TGuide::load` does not construct retail's `guide_1.blo`
screen and panes, which `perform` uses.  Therefore neither a frame-seam change nor a
threading change can restore the missing animation: the required port unit is
`TGuide::load` plus `TGuide::perform`.

The Ghidra decompile used for the conclusion is `build/decomp/801791d0.c`, generated
from the existing `scratch/ghidra_proj/SMS` project.  The result must be verified
against a scripted Dolphin guide transition after the class is ported.

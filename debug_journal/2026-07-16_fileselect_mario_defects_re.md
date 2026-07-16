# 2026-07-16 — File-select Mario defects: state-pin harness + Ghidra-confirmed root causes

The multi-session "Mario paleness" chase fixated on the overalls HUE and repeatedly over-claimed
"resolved." User called it out: at file-select, native Mario has a **black nose**, an **equipped
FLUDD** that shouldn't be there, black glove/body patches, and a wrong (crouch) pose — "not porting
the game properly." This entry records the tooling built to compare properly and the Ghidra/oracle
RE that establishes ground truth (no more inference/tweaking).

## Tooling built (committed)

**Cross-engine state-pin harness** — forces native into the oracle's exact state so a native-vs-
oracle pixel diff is pure render fidelity:
- Fork `--dump-state-json` (extern/dolphin_fork MainNoGUI): after `--load-state-at` settles, reads
  GMSE01 guest state (gpCamera pos/target/up/fovy, gpMarioOriginal raw) → JSON. Also raw-dumps
  gpMarioOriginal's first 0x1C0 bytes for layout RE.
- Native `SB_PIN_STATE=<json>` (sms-boot/runtime/pin_state.{h,cpp} + cameragc.cpp hook): each frame,
  forces gpCamera's final lookat inputs (unk124/unk148/mUp/mFovy) BY NAME (host layout != guest
  offsets — the first bug hit) to the dumped values.
- VALIDATED: native's own file-select camera eye=(1095,328,-13)/fovy40 already matches the oracle;
  pinning aligns the framing. At matched framing, static geometry matches the oracle tightly
  (cubeC d=[3,2,3], OPTIONS-sign d=[4,1,-5], water d=[-4,0,3]). Same-state Mario crop:
  `scratch/texcmp/mario_samestate.png`.

## Ghidra project

`scratch/ghidra_proj` (analyzeHeadless import of scratch/bin/sms.dol, GameCubeLoader,
`-loader-autoloadMaps false` REQUIRED or the import throws HeadlessException). Decompile by VA:
`SMS_DECOMP_VAS=0x...,... analyzeHeadless scratch/ghidra_proj SMS -process sms.dol -noanalysis
-scriptPath tools/ghidra_scripts -postScript DecompDump.py` → `build/decomp/<va>.c`.

## RE-confirmed root causes (ground truth, NOT inference)

### FLUDD wrongly equipped — CONFIRMED port bug
- Guest `TMario::mFlag` @ **0x118**, `HAS_FLUDD = 0x8000` (Ghidra: `build/decomp/80240a58.c`,
  `isUnUsualStageStart`, `*(param_1+0x118) & 0x8000`). Guest mPosition @ 0x10.
- **Oracle** file-select Mario: `mFlag@0x118 = 0x00040001` → **HAS_FLUDD OFF**, pos (950,100,-1000).
- **Native**: `SB_DBG_FLUDD` shows `TMario::load` reads `local_20=0 → HAS_FLUDD ON`, same pos.
- ⇒ Retail file-select Mario has NO FLUDD; the port wrongly enables it. The port's hand-written
  `TMario::load` (MarioInit.cpp:318 — retail `TMario::load` is CW-inlined, NOT a standalone symbol)
  is misaligned/incomplete: it reads the placement flag as 0 where retail yields bit0-set, and it
  only sets/clears HAS_FLUDD (`mFlag=0; ...`) while retail's mFlag carries other bits. FIX (todo):
  RE the retail inlined placement read (the option-scene actor loader) to correct the stream layout.

### Ruled OUT
- **TGuide** (Guide.cpp is a 1-byte stub) — decompiled placeMario/changeBotStatus (0x80179e0c/
  0x8017a060): both are 2D UI (guide/cursor panes, save-slot digit displays), NOT the 3D Mario.
  Porting TGuide fixes the 2D guide UI, not the Mario defects.
- **Overalls hue** — the earlier aurora-attnFn + port-L2-specular fixes were real (replay-verified)
  but only addressed hue; they do NOT touch nose/FLUDD/pose.

### Still to RE
- **Black nose + black glove/body patches** — shadowed surfaces clamp to black on live-native but
  not the oracle (replay renders Mario's nose fine → retail normals OK). Prime suspect: live-native
  **skinned normals wrong** → SIGN-diffuse (ch0) N·L too negative → black. The L2-specular fix
  un-masked it (the old over-bright hid it). Needs RE of Mario's skinning/normal calc vs oracle.
- **Crouch pose** — his file-select idle animation differs from the oracle's standing pose.

## Honest status
Nothing about the nose/FLUDD/pose is FIXED yet. What IS done: the pin/compare tooling, and the
Ghidra/oracle RE that CONFIRMS the FLUDD bug (retail=no FLUDD) and rules out TGuide/overalls-hue as
causes. The fixes (placement-load layout for FLUDD; skinned normals for the black nose; animation
for the pose) are real multi-function porting work to do against this ground truth.

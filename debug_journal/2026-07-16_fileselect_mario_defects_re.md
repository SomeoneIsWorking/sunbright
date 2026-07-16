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

## ✅ FLUDD — RESOLVED (2026-07-16, disasm-confirmed + live-native-verified)

Full RE chain: TMario vtable = 0x803dd660 (from the oracle guest dump, +0x0); load slot 4 =
**0x80276bd0** — retail `TMario::load` MATCHES the port (reads placement u32, bit0==0 → FLUDD ON;
scene.bin Mario entry: pos(950,100,-1000), then string "マリオ キャラ", u32s 0,0x64,0 → FLUDD ON at
load is CORRECT). The strip happens later: DOL-wide scan for `lwz 0x118 / rlwinm 0,17,15 / stw`
found 4 clear sites; the GC2D one @**0x8016d844** is inside TCardLoad's title→file-select
transition (state 10→9): `lwz gpMarioOriginal(r13-0x60d8); mFlag &= ~0x8000`. The port had
transcribed that line as `offFlag(MARIO_FLAG_GAME_OVER)` (0x400) — **wrong flag**. Fixed to
`offFlag(MARIO_FLAG_HAS_FLUDD)` (CardLoad.cpp, submodule e86b7b41). Verified live-native with the
pinned camera: FLUDD gone, Mario bare (scratch/texcmp/fluddfix_mario.png).

## REMAINING (isolated, unambiguous): black patches = skinned normals

Post-FLUDD-fix capture shows the leftover defect cleanly: BLACK patches on the nose, glove fronts,
overall legs, shoe tops, chin — surfaces whose native N·L goes strongly negative (SIGN diffuse
swallows the 0.5 ambient → clamp to 0 → black), while the oracle keeps them lit. The replay
(retail-recorded vertices) renders these fine → retail normals OK → **live-native's SKINNED normal
computation produces wrong normals for a subset of vertices**. Next RE: native J3D skinning normal
path (envelope/weighted NRM matrix) vs oracle.

## Black patches — det==0 CONFIRMED as the mechanism (2026-07-16)

`SB_LOG=nrmmtx` live-native probe: **>1M `J3DPSCalcInverseTranspose` det==0 bails** (1139 logged at
1/1000 sampling), all with src row0=[0,0,0] — **all-zero draw matrices** are being inverse-
transposed; the bail leaves the nrm-matrix slot as un-zeroed heap = garbage normals (black patches
on Mario's nose/gloves/legs/shoes). Normals VISUALIZE smooth (SB_NRM_VIZ) because the garbage is in
the per-draw NRM MATRIX PALETTE (used by the lit path's matrix-indexed transform), while the viz
samples the post-transform interpolant of mostly-valid slots. NEXT (probe running): calcNrmMtx-level
report of WHICH matrix indices are zero per model (zeroDrawMtx/first/fullWgt/wEvlp) to tell whether
Mario's shapes reference the zero slots (real defect: draw matrices not written — envelope count
mismatch?) or the zero slots are unused padding (then black patches need another explanation and
retail behavior at det==0 must be matched instead). All logging now via sb_log channels.

### Envelope-slot zeros FIXED (real bug) — but black patches PERSIST (not the cause)

viewCalc's envelope range is INDEX-mapped (86 entries over 43 matrices for Mario); the bulk
sequential concat left [fullWgt+wEvlp, DrawMtxNum) zero. Fixed with the indexed concat (submodule).
zeroDrawMtx: Mario 43/106 → 0. BUT the black patches are unchanged → those slots were never
uploaded/used; the patch cause is still OPEN. Next suspects: per-vertex NRM source data for the
multi-matrix (skinned) packets, or the per-vertex matrix-index stream, or aurora's live-native
(non-replay) indexed-normal fetch. Compare one black-patch vertex end-to-end vs oracle next.

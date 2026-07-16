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

### OCCLUDED-flag theory FALSIFIED for the black patches
GXPeekARGB stub returned 0 → MARIO_FLAG_OCCLUDED permanently ON (MarioMain.cpp:302 stamp test).
Changed the stub to the 0x10 not-occluded stamp (a more-correct documented STOPGAP; proper fix =
aurora EFB readback): black patches UNCHANGED (4069 near-black px before and after) → the occlusion
flag is NOT the cause. Remaining next step: single-vertex end-to-end normal comparison (black-patch
vertex: BMD NRM source → matrix index → transform) vs the oracle.

### TEV-stage-0 probe: inconclusive (global truncation confounds)
SB_TEV_STOP=0 truncates ALL materials, so the Mario region is dominated by truncated background —
not usable to split texture-vs-TEV for the patches. Better next instrument: per-DRAW TEV stop or a
Mario-texture dump compare (his gloves/shoes textures native vs replay via SB_TEX_DUMP content-hash,
which previously matched globally — recheck scoped to the black-patch textures), or the planned
single-vertex normal trace.

### NEW LEAD (untested): the black patches may be the DIRTY/GOO overlay misfiring
The patch SHAPES are splotch-like (body/gloves/shoes/nose) — Mario's tev=5 material's TEXA-compare
KONST stages are the DIRT/goo overlay system, and the port's TMario::load zeroes mFlag then only
touches HAS_FLUDD while retail's settled mFlag=0x00040001 (extra bits). If the dirty state/texture
(H_ma_rak.bti, cDirtyFileName in MarioInit) animates/gates differently in native, the dirt stage
could render BLACK splotches where retail renders none. TEST: force the dirty TEV stage off (or
compare the dirt texture/anim state native-vs-oracle) — cheaper and more shape-consistent than the
single-vertex normal trace. Check first next tick.

### Dirt/goo theory FALSIFIED: mDirty=0 (matches retail; replay K0.a=0.000 renders correctly)
Black-patch cause still OPEN. Falsified so far: nrm-palette zeros, OCCLUDED flag, dirt overlay.
Remaining: per-draw TEV-stage isolation on Mario's actual draws, or single-vertex normal trace, or
diff the full TEV/tex state of ONE black-patch draw native-vs-replay with the existing dump tools.

## ✅ BLACK PATCHES (nose/chin/face) — ROOT-CAUSED & FIXED (2026-07-16): TMirrorActor envelope-copy bound

**Correction first:** the earlier "nrm-palette zeros falsified / envelope fix verified 43→0" notes above
were WRONG — the zeroDrawMtx probe used `SB_LOG_EVERY(300)`, which sampled Mario's zero reports OUT.
Unsampled, Mario's body models still showed **zeroDrawMtx=28/106 every frame** (entries 49-58 + 92-105 =
envelope indices 29..42, twice).

**Chain (each step instrumented, not inferred):**
1. New aurora `SB_LOG=pn` (full 10-slot pos/nrm palette per dumped draw): replay(fsel_try_7300.dff) =
   ZERO bad slots; live-native Mario skinned draws = det==0 nrm slots per packet (`tools/render/pn_extract.py`).
2. New aurora `SB_LOG=pnzero` (indexed XF loads with zero rotation): zero uploads came from Mario-body
   buffers, indices 49-58 → the draw-mtx ARRAY itself has zeros at draw time.
3. `SB_LOG=drwidx` (DRW1 dump): flag-1 indices are all in-range (two sequential 0..42 runs) — not an
   index bug. `SB_LOG=evlp` (blend zero-witness): the envelope blend NEVER produces zero → the zeros
   belong to models whose blend never runs.
4. `SB_LOG=j3dbt` (creation backtrace): the zero-uploading models are the **TMirrorActor clones**
   (`TMario::initMirrorModel` → `TMirrorActor::init` news a second J3DModel over Mario's modelData;
   drawn in the Mirror Opa buffers in BOTH the 256x256 reflection pass and the 640x448 main pass).
5. `TMirrorActor::perform` copies the source model's matrices into the clone each frame. The port
   bounded BOTH copy loops by `getJointNum()` (29); **retail (0x80224910) bounds the second loop by
   wEvlpMtxNum (modelData+0x84) = 43**. Envelopes 29..42 were never copied → clone viewCalc concats
   zero sources → zero draw matrices → garbage nrm palette → black patches on envelope-skinned faces.

**Fix:** MirrorActor.cpp second loop bound → `getWEvlpMtxNum()` (submodule 53f4c130). **Verified:**
zeroDrawMtx 28→0 for both Mario body models, `pnzero` fully silent, and the settled file-select capture
shows nose/chin/face lit correctly (scratch/pndump/fix_mario_zoom.png vs scratch/texcmp/fluddfix_mario.png).

**Note on viewCalc:** retail fills the envelope draw-mtx range SEQUENTIALLY
(`J3DMTXConcatArray(view, wEvlpMtx, drawMtx+fullWgt, wEvlpNum)` — Ghidra 0x802deeb8), not index-mapped.
For Mario the DRW1 indices in [fullWgt, fullWgt+43) are 0..42 sequential, so the port's indexed fill is
identical there and additionally defines [63,106) (retail leaves them stale; no observed shape references
them). Equivalent-on-defined-range; keeping the indexed form.

**STILL OPEN (separate defects, unchanged by this fix):** black glove BACKS, gray chest blotch, black
leg patches, crouch-vs-standing pose. These were pixel-identical before/after the mirror fix and show
no zero-matrix signature (`pnzero` silent) — different mechanism. Remaining zeroDrawMtx models (14/14
joints=14 pair, 1/1) never upload (not drawn; likely hidden mirror clones) — harmless.

**Tooling kept (SB_LOG channels):** `pn`, `pnzero` (aurora 2961e45), `j3dbuf`, `j3dbt`, `drwidx`,
`evlp`, unsampled `nrmmtx`; parser `tools/render/pn_extract.py` (refuses logs without [pn] lines).

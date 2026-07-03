# 2026-07-03 — file-select slot labels use English literals under native

## Visible symptom

`title_sbs.sh` (Mario in SLEEP): native shows "NEW / NEW / NEW" all-caps for
the three file-select slots; oracle shows "Corrupt / New / New" mixed-case.

## Root cause

`reference/sms/src/GC2D/CardLoad.cpp` is a JP decomp. Its
`TCardLoad::selectBookmark` (state case 1) `strncpy`s hardcoded JP text:

- corrupt: `"こわれています"` (renders as garbage through the US font atlas)
- empty:   `"NEW"` (all-caps)

Oracle is Dolphin running the actual GMSE01 US binary — that binary's
literals are the English `"Corrupt"` and mixed-case `"New"`. Not a
BMG-lookup difference; both binaries use `strncpy`d hardcoded string
literals — they just differ per region.

## Fix

Under `SMS_NATIVE_PLATFORM`, `strncpy` the US literals. Both call sites
in `selectBookmark` (initial open at line 2032; post-selection re-render
at line 2214) updated with `#ifdef`ed English variants. Non-native path
(oracle-independent JP-decomp parity work) keeps the JP text.

## Verify

Native slot labels now read "New" mixed-case, matching oracle's letter
case. `title_overbright.py` mean_abs_pixel_delta 58.1 → 57.8.

## Note on residual state divergence

Native still shows "New" for slot A while oracle shows "Corrupt": native's
`scratch/memcard_chan0.raw` is a fresh file with no corrupt save; oracle's
Dolphin memcard has a corrupt slot-A save baked in. That's a memcard
content difference, not a rendering bug — the rendering pipeline handles
both branches correctly (`unk40[i].unk0 == 1` gate hits the "Corrupt"
strncpy when the card has a corrupt slot).

## Other visible defects deferred

- Sky cloud pattern differs (already-tracked cosmetic).
- Water color slightly different shade (already-tracked cosmetic).
- Palm tree height / camera projection subtle differences.
- "OPTIONS" duplicate/overlap on native (visible in SBS — needs
  separate investigation).
- Mario zzz bubble missing — deeper JPA particle rendering gap: the
  emitter chain (TMarioParticleManager::perform → JPAEmitterManager::draw
  → JPADraw::draw → drawParticle) all fire correctly with an alive
  particle, but the visitor `exec` chain never reaches
  `sb_gx_imm_begin` (JPADrawExecBillBoard::exec traced with 0 invocations
  while drawParticle iterates). One of the other visitor classes runs
  instead; needs full RE of the visitor set for `PARTICLE_MS_POI_ZZZ`.
  Journalled here for a future dedicated session.

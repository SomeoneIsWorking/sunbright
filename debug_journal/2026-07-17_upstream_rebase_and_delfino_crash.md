# 2026-07-17 — Upstream rebase onto doldecomp/sms + pre-existing Delfino boot crash

## Rebase (DONE, pushed)

Rebased our native port onto upstream `doldecomp/sms`.

- Fork-point (old merge-base): `0686c767` "Map matching/naming".
- Upstream was **26** commits ahead; our branch **368** commits ahead (all authored by us).
- New base: `4fa80205` "Mostly match SelectDir (#122)".
- Submodule tip after rebase+reconcile: `8a75d810` (force-pushed to `origin/sunbright`,
  SomeoneIsWorking/sms). Parent gitlink commit `9a54c48` (pushed to SomeoneIsWorking/sunbright).
- Safety tag: `pre-rebase-backup-20260717` (= old tip `8557e683`).

**Why not a plain `-X theirs` rebase result:** the mechanical rebase interleaved our and
upstream's edits hunk-by-hunk in files we both touched, producing internally-inconsistent
TUs (duplicate class-member decls; our `.cpp` calling old APIs upstream renamed:
`getUnkB4`→`getViewMtx`, `TLookAtCamera` 2-arg→6-arg, `TOrthoProj` 4-arg overload dropped,
`TMapCollisionBase::unk10`→`unk20`, `TBathWaterManager` field-layout refresh, `TMapXlu`
getter dropped; plus upstream JAudio/camera code reintroducing LP64 `ptr→u32` truncations
our port had fixed).

**Resolution = file-level "our port wins" on every contested file.** For each of the ~583
files EITHER side changed since the fork-point, took our known-good **pre-rebase** version.
Upstream improvements land only on the ~200 files we never touched, plus 7 genuinely-new
upstream files (`Animal/boid`, `Animal/fishoid`, `Enemy/BossManta`, `Enemy/Igaiga` headers,
`dolphin/gd/GDVert.h`, docs). Added `TVec3<f32> operator/` + `operator/=` (scalar divide) for
upstream's new `Animal/boid.cpp`. Builds clean; **boot behavior is byte-for-byte the same
decision path as the pre-rebase tip** (verified: both reach `APP_STATE_GAMEPLAY` at
`SB_STAGE=1` and both hit the identical crash below).

Trade-off: a first sync gets linear history + new files, not body-fills on existing files.
Those can be pulled per-file later where wanted.

## Pre-existing Delfino boot crash (NOT caused by the rebase)

`SB_STAGE=1` (Delfino) reaches `APP_STATE_GAMEPLAY`, populates the plaza (genObject skips
the unimplemented types), then **SIGSEGV (exit 139) deterministically (5/5)** during model
setup — in **BOTH** the pre-rebase tag `8557e683` and the rebased tip. So it is a port-HEAD
regression that predates and is independent of the rebase. (The Delfino-renders milestone
memory `delfino-gameplay-renders-2026-07-17` says "NO crash", so it regressed between that
milestone and HEAD `8557e683` — i.e. one of the recent plaza-population actor ports.)

### Symbolication is misleading — instrument, don't trust the unwinder
- `eu-stack` on the core first blamed `TWaterGun::changeBackup` — **wrong** (corrupted/optimized
  unwind picked the nearest symbol).
- `lldb` blamed `SDLModel::entryModelDataSDL+0x13` (`movq (%rsi),%rbx`, fault addr 0x0 ⇒
  "NULL `param_1->unk0`"). Also **misleading**: an unconditional `fprintf` at the top of
  `entryModelDataSDL` logged **150 calls, every one with valid `param_1` AND `unk0`**, then
  crashed. So the crash is NOT the assumed NULL `SDLModelData`; it is deeper in
  `entryModelDataSDL` while processing a **valid** model, and/or the reported PC is itself
  a mis-symbolized artifact of heap corruption.
- Caller chain of the crashing model: `SDLModel::SDLModel(SDLModelData*,u32,u32)` ←
  `TMActorKeeper::createMActorFromAllBmd(u32)` (a generic model-from-all-bmd path used by
  many actors — so the crash site is a victim, not the culprit).
- The crash **moves under different timing** (under `run.sh`+timeout it dies right after the
  `FruitsBoat` genObject skips; under `lldb` it got as far as J2D HUD pane searches). Moving-
  under-timing + symbol misattribution ⇒ **heap corruption**, almost certainly an
  out-of-bounds write from an earlier actor init that only manifests later in model setup.

### Next step for whoever picks this up
Do NOT keep chasing `entryModelDataSDL` — it is the victim. Bisect the recent plaza-population
commits (`a84d562` TMapObjBall::calcCurrentMtx, `d230b4a` TResetFruit, `cbfca2c` TMapObjTree,
`10c3cd4` TAnimalBase ctor, and the Seal/EggGen/AreaCylinder/EffectEnemy/DebuTelesa/HauntLeg/
FruitsBoat/MapObjBianco chain) against the last known-good "Delfino renders" state, OR rebuild
with AddressSanitizer to catch the OOB write at its source. Suspect a wrong field
size/offset in a recently-added actor class writing past its allocation (the LP64 field-add
work — "safe because access is by-name" only holds if no raw-offset/stream write overruns).

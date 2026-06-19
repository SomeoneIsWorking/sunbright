# Sun-occlusion (GXPeekZ) is UNREACHABLE in Delfino Plaza — quantified (2026-06-19)

Working the handoff's target #1 (EFB-readback effects), top-down. The GXPeekZ sun-occlusion
port already EXISTS (`runtime/overrides/efb_readback_native.cpp` `ov_gxpeekz` @ 0x8035dcf0) and
rests on the already-verified depth-readback primitive (`sb_ngx_efb_peek_depth`, the same one the
Mario-occlusion GXPeekARGB path uses, which IS verified live). The only thing blocking "done" is
verification: confirming GXPeekZ fires and the occlusion count is correct **in a scene where the
sun is on-screen**. It is not — proven below with live RAM, not assumed.

## The chain (RE — reference/sms/src/Camera/{sunmgr,sunmodel}.cpp)
- `TSunMgr::drawSyncCallback` (0x8002e270): `if (unk14) gpSunModel->getZBufValue()`. unk14=1 in plaza
  (sun model "太陽モデル" loaded), and drawSyncCallback FIRES every frame under ngx (the old
  "drawsync-dead under ngx" blocker stays falsified).
- `TSunModel::getZBufValue` (0x8002ea70): for each of 17 sample points `unkB4[i]`, calls
  `GXPeekZ(x,y)` **only if the point is on-screen** (`x != -1 && y != -1`) and Mario not indoor.
- `unkB4` is filled by `calcDispRatioAndScreenPos_` from the sun's 3D→2D projection (`unkF8`,
  `CLBCalc2DFPos`/`CLBScreenFPosToSPos`). Off-screen → the (-1,-1) sentinel; the float `unkF8[i]`
  stays at the ctor's 10000.0 sentinel when the sun is behind/off the camera.

## Live evidence (free-roam plaza, scratch/freeroam_plaza.sav)
- `SUNBRIGHT_DBG_EFB`: ~3000 drawSyncCallback hits, unk14=1, unk15=0; **0 GXPeekZ, 0 getZBufValue**.
- Read `gpSunModel` live: `drawSyncCallback` loads it from `*(r13 - 0x70F8)`; r13=0x804141c0 →
  gpSunModel ptr @ 0x8040D0C8 = **0x812b5680** (GMSE01; no E01 data-symbol file, derived from disasm).
- `gpSunModel+0xB4` (unkB4[17]) = all 0xffffffff (all off-screen). `+0xF8` (unkF8[0]) = 10000.0.
- Built a **live projection-feedback probe** (orbit camera via `/pad?do=cright`, poll `unkF8[0]`):
  at a few yaws the sun leaves the sentinel at NDC ≈ (−0.9..0.7, **~8.0**) — horizontally centred,
  but ~8 screen-heights ABOVE the top edge (screen NDC y ∈ [-1,1]).
- Tilting up (`cup`) bottoms out at NDC-Y ≈ **6.9** then the plaza camera pitch saturates/plateaus
  at ~8.8; `cdown` puts the sun behind (sentinel). The plaza camera **cannot** tilt up far enough.
- Sun rendered world pos `gpSunModel+0x198` = (87330, **219340**, 90298): Y≈219k vs plaza ground
  ~0–3k ⇒ sun at ~60° elevation. Moving Mario within the plaza (±~15k) can't flatten 60°.

## Conclusion
The sun-occlusion lens-flare is the **Noki Bay sun-warp** effect — the sun is only brought to a
frameable elevation at the warp point (sunmgr.cpp `perform`: `unk15&1` set iff `map==1 &&
TFlagManager::getBool(0x50004)`, sets next stage 9 = Noki Bay). It is NOT reachable from the plaza
save. Per the tooling-first rule, this is a STOP for this effect: do not declare the port done
without a sun-framed scene. The port stays dormant + honestly documented. Verifying it would need a
Noki-Bay-progress save (flag 0x50004, map 1) — a disproportionate build for one lens-flare; revisit
only if a Noki Bay save/state becomes available.

DO NOT re-grind the plaza camera for the sun — the reachability limit is quantified here.

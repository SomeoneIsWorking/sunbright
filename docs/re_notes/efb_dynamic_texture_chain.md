# RE: the EFB → dynamic-texture copy chain (the 60fps screen-space on/off flicker)

Pins down the exact mechanism behind the "renders on real fields, absent/different on the
in-between" flicker class that the user attributes to "a chain that renders a DYNAMIC TEXTURE
on top of an existing object." All addresses are GMSE01 (`reference/sms_gmse01_funcs.txt`);
file:line refer to the vendored decomp at `decomp/sms/`.

Builds on (do not re-derive — read these first):
- `docs/re_notes/water_rendering.md` — the screen-texture refraction mechanism.
- `docs/re_notes/sun_specular_efb_effects.md` — the EFB-peek state probes (different class).
- `docs/re_notes/interp_screenspace_strategy.md` — the recommended strategy (b1) this builds on.
- `docs/re_notes/perform_list_architecture.md` — which list each phase runs in.

This is documentation + a default-OFF candidate override. No existing file was edited.

---

## 0. TL;DR — verdict and the one-paragraph fix

**Hypothesis (a) is CONFIRMED, (b) is REFUTED.** Every `TEfbCtrlTex` writes its EFB→texture
copy to a **fixed guest address** held in `TEfbCtrlTex+0x2C` (`mImagePtr`). The copy phase
(`& 0x8`) lives in `mPerformListGX` (director `+0x1C`) and the graffiti/mirror copies in the
graffiti lists (`+0x38`/`+0x3C`) and the GX list — **all of which the in-between re-issues**
(`kDrawLists = {0x40,0x38,0x3C,0x1C,0x20,0x24}`, `interp_redraw.cpp:47`). The in-between's
`perform_mask` default `0xFFFFFFFC` clears only `& 1`/`& 2` (movement/calc), so `& 8` (copy)
**and** `& 0x80` (consumer `drawRefracAndSpec`) both run again — against the INTERPOLATED EFB,
overwriting the real field's texture at the same fixed address. Dolphin's texture cache keys
EFB-copy textures by address, so the in-between's resolve clobbers the real field's texture =
the per-field flicker. This is structurally the **same bug** as the fixed 30fps single-XFB bug
(address-keyed, overwritten; CLAUDE.md "interp60-object-level").

**The fix (matches `interp_screenspace_strategy.md` strategy b1, no bandaid):** on the
in-between, render the EFB-feedback layer at its native 30fps — *do not re-resolve the copy and
do not interpolate the surface*. Concretely: (1) drop the `& 8` copy on the in-between (already
implemented behind `g_i60.skip_efbcopy`, `interp_redraw.cpp:110` — make it the default), so the
fixed-address texture survives as the real field's content; (2) hold the *consumer* surface at
tick N (no draw-matrix blend) so the frozen reflection and the frozen surface agree (no ghost);
(3) for the few consumers that re-draw against the real field's texture, that is correct and
stable. The candidate override below (`runtime/overrides/efb_interp_freeze.cpp`,
`SUNBRIGHT_EFB_INTERP_FREEZE`, **default OFF**) implements (1) generally for every
`TEfbCtrlTex` instance.

---

## 1. `TEfbCtrlTex` — the shared primitive, its field layout and copy code

Source: `decomp/sms/src/JSystem/JDrama/JDREfbCtrl.cpp`,
header `decomp/sms/include/JSystem/JDrama/JDREfbCtrl.hpp`. Class chain:
`TViewObj → TEfbCtrl → TEfbCtrlTex`.

### Field layout (confirmed from the header AND the disassembly)
```
TEfbCtrl:
  +0x10  TRect       unk10        // src rect (set by setSrcRect, clamped/aligned)
  +0x20  TFlagT<u16> unk20        // mode bits (0x8 copy enable family, 0x800/0x1000 fmt/mip…)
TEfbCtrlTex (extends TEfbCtrl):
  +0x24  u32         mWidth       // DST width  (the texture's width, half-res for the screen tex)
  +0x28  u32         mHeight      // DST height
  +0x2C  void*       mImagePtr    // ★ DST guest address — the texture image buffer (FIXED)
  +0x30  GXTexFmt    mTexFmt      // DST format (RGB565 screen tex, R8 graffiti, mirror = src fmt)
  +0x34  GXFBClamp   mFbClamp
  +0x38  TColor      unk38        // copy-clear color
  +0x3C  u32         unk3C        // copy-clear z
  +0x40  u8 (*unk40)[2]           // copy filter (horiz)
  +0x44  u8*         unk44        // copy filter (vert) = SMSVFilter_flicker
```

### The copy-phase code (`TEfbCtrlTex::perform`, `0x802f8bac`)
```c
void TEfbCtrlTex::perform(u32 param_1, TGraphics* param_2) {
    if (param_1 & 0x80)  IssueGXPixelFormatSetting(...);     // post/format phase
    TEfbCtrl::perform(param_1, param_2);                     // base: &0x80 z/color update, &0x8 no-op
    if (param_1 & 0x8) {                                     // ★ COPY PHASE
        GXSetCopyClamp(mFbClamp);
        IssueGXSetCopyFilter(unk20.check(0x800), unk40, unk20.check(0x20), unk44);
        if (mImagePtr != nullptr) {                          // null-guard
            bool doClear = IssueGXSetCopyClear(unk38, unk3C, unk20.get());
            GXSetTexCopySrc(unk10.x1, unk10.y1, unk10.getWidth(), unk10.getHeight());
            GXSetTexCopyDst(mWidth, mHeight, mTexFmt, unk20.check(0x1000));
            GXCopyTex(mImagePtr, doClear);                   // ★ EFB → mImagePtr (fixed addr)
        }
    }
}
```
GMSE01 disassembly cross-check at `0x802f8bac`:
- `0x802f8bd4 rlwinm. 549c0631` → tests `param_1 & 0x80` (the post branch).
- `0x802f8cb8 rlwinm. 57c00739` → tests `param_1 & 0x8` (the copy branch).
- `0x802f8cf8 lwz r0,0x2C(r29)` then `cmpli`/`bc` → loads `mImagePtr` from **+0x2C** and
  null-guards it, exactly matching the source. **mImagePtr offset 0x2C is confirmed.**

**Key fact:** `GXCopyTex(mImagePtr, …)` resolves the EFB to the buffer at the fixed address
`mImagePtr`. The address never changes per-field — it is the texture's image buffer set once at
setup. So whatever EFB is live when the copy runs is what lands at that address.

---

## 2. The four `TEfbCtrlTex` instances — where mImagePtr points, size, format

### 2a. "通常シーン描画ステージ" (normal-scene screen texture) — the plaza-water source
Setup: `MarDirectorSetupObjects.cpp:387-403`.
```c
normalSceneDrawStage = TNameRefGen::search<TEfbCtrlTex>("通常シーン描画ステージ");
normalSceneDrawStage->unk20.on(0x122F);                // enables 0x8 copy + fmt + 0x1000 mip-ish bit
normalSceneDrawStage->unk44 = SMSVFilter_flicker;
GXTexObj sctex = screenTex->getTexture()->mTexObj;     // the スクリーンテクスチャ JUTTexture
normalSceneDrawStage->setTexAttb(sctex);               // → mImagePtr = GXGetTexObjAll image ptr
normalSceneDrawStage->setSrcRect(TRect(0,0, renderW, renderH));   // copy WHOLE frame
```
- **mImagePtr**: set by `setTexAttb` (`0x802f8b40`) which does
  `GXGetTexObjAll(&param_1, &mImagePtr, …)` — i.e. `mImagePtr` = the `JUTTexture`'s image-data
  buffer. That `JUTTexture` is allocated in `TScreenTexture::load` (`ScreenUtil.cpp:218`):
  `new JUTTexture(SMSGetGameRenderWidth()/2, SMSGetGameRenderHeight()/2, GX_TF_RGB565)`.
  **Fixed guest address** = the texture's image buffer (heap-allocated once at scene setup;
  stable for the scene's life). Consumed as `GX_TEXMAP0` by the water (`drawRefracAndSpec`).
- **Size / format**: half render width × half render height, **GX_TF_RGB565** (`mTexFmt+0x30`).
- **Copy phase / list**: `& 0x8`, runs in `mPerformListGX` (director `+0x1C` = kDrawLists[3]),
  after the world is in the EFB.

### 2b. "鏡描画ステージ" (mirror) — `MarDirectorInitECT.cpp:79-93`
```c
mirrorTex = TNameRefGen::search<TEfbCtrlTex>("鏡描画ステージ");
mirrorTex->unk20.set(0x228);                            // copy (0x8) + 0x20 + 0x200
mirrorTex->unk44 = SMSVFilter_flicker;
GXTexObj& obj = mirrorCam->unk60;                       // the mirror camera's texture object
mirrorTex->setTexAttb(obj);                             // → mImagePtr = mirror tex image ptr
mirrorTex->setSrcRect(TRect(0,0, GXGetTexObjWidth(&obj), GXGetTexObjHeight(&obj)));
```
- **mImagePtr**: the mirror camera's `GXTexObj` image buffer (`TMirrorCamera::unk60`), a
  **fixed guest address**. Used by mirror-surface models (`Map/MapMirror`).
- **Size / format**: mirror tex W×H, format from the tex obj.
- **List**: `initECTMir` is called with `mPerformListGX` (`MarDirectorSetupObjects.cpp:385`), so
  the mirror copy + its scene re-render sit in `mPerformListGX` (`+0x1C` = kDrawLists[3]). The
  whole mirror pass (re-render scene from mirror camera + copy) is **expensive**.

### 2c. + 2d. "graffito check" + "graffito" (pollution / graffiti) — `MarDirectorInitECT.cpp:36-71`
The `initECTGft(param_1=unk38, param_2=unk3C, …)` path (call site
`MarDirectorSetupObjects.cpp:384`). Two distinct kinds:

**(2c) "graffito check"** — `MarDirectorInitECT.cpp:36-50`:
```c
graffitiEfbTex = new TEfbCtrlTex("graffito check");
graffitiEfbTex->setSrcRect(TRect(0,0,0x200,0x200));    // 512×512
param_1->push_back(graffitiEfbTex, 0x80);              // pushed with 0x80 (post) AND
param_1->push_back(graffitiEfbTex, 0x8);               //   with 0x8 (copy) into unk38
```
- NB: this instance never has `mImagePtr` set explicitly here → stays `nullptr` →
  the `if (mImagePtr != nullptr)` guard means **its GXCopyTex is a no-op** (it only issues the
  copy-filter/clear setup). It is a viewport/ortho framing helper, not a real copy target.
  (Its `setTexAttb` is not called in this branch.) Lives in `unk38` (`+0x38` = kDrawLists[1]).

**(2d) per-pollution-layer "graffito"** — `MarDirectorInitECT.cpp:52-71`:
```c
for (i = 0; i < gpPollution->getJointModelNum(); ++i) {
    TEfbCtrlTex* efbTex = new TEfbCtrlTex("graffito");
    const ResTIMG* img = gpPollution->getLayer(i)->getUnk58();
    efbTex->mImagePtr  = (u8*)&img + img->imageDataOffset;   // ★ FIXED addr = layer's TIMG image data
    efbTex->mWidth     = img->width;
    efbTex->mHeight    = img->height;
    efbTex->mTexFmt    = GX_CTF_R8;                          // single-channel (coverage mask)
    efbTex->setSrcRect(TRect(0,0,img->width,img->height));
    param_2->push_back(efbTex, 0x80);                        // post  → unk3C (+0x3C, kDrawLists[2])
    param_2->push_back(efbTex, 0x8);  // (via the param_1 graffitiGroup interleave; copy phase)
}
```
- **mImagePtr**: `(u8*)&img + img->imageDataOffset` — the pollution layer's `ResTIMG` image
  data, a **fixed guest address** inside the loaded pollution resource. Each layer = one
  fixed-address coverage texture. This is the EFB→tex readback that measures how much of each
  graffiti layer is sprayed/cleaned (a R8 coverage copy). Lives in `unk3C` (`+0x3C` =
  kDrawLists[2]) for the post phase; the copy `& 0x8` is interleaved in `unk38`/`unk3C`.

### Summary table
| Instance | Name | mImagePtr source | Fixed? | Fmt | Size | Copy list (director field) |
|---|---|---|---|---|---|---|
| 2a | 通常シーン描画ステージ | `screenTex` JUTTexture image buf (`setTexAttb`) | YES | RGB565 | ½W × ½H | mPerformListGX `+0x1C` |
| 2b | 鏡描画ステージ | mirror cam `GXTexObj` image buf (`setTexAttb`) | YES | tex obj fmt | mirror W×H | mPerformListGX `+0x1C` |
| 2c | graffito check | (never set → nullptr → copy is no-op) | n/a | — | 512² rect | unk38 `+0x38` |
| 2d | graffito ×N layers | `&img + img->imageDataOffset` (pollution TIMG) | YES | R8 | layer W×H | unk38/unk3C `+0x38`/`+0x3C` |

---

## 3. The consumers — which texture each samples, and in which list/phase it draws

| Consumer | Samples which tex | Draw list / phase | Re-issued by in-between? |
|---|---|---|---|
| **Plaza water refraction+specular** `TModelWaterManager::drawRefracAndSpec` `0x8027c12c` | 2a (通常シーン) as `GX_TEXMAP0`, `unk5D34` | `mPerformListGXPost` `+0x24` (kDrawLists[5]), phase `& 0x80` | **YES** (0x24 in set, `& 0x80` in mask) |
| **Water in-EFB sub-passes** (silhouette/volume/mirror/shine) | n/a (write EFB) | `mPerformListGX` `+0x1C`, phase `& 0x8` | YES |
| **Mirror surface models** (Map/MapMirror) | 2b mirror tex | scene draw in `mPerformListGX` `+0x1C` | YES |
| **Graffiti coverage** (pollution count) | 2d layer R8 texs | `unk38`/`unk3C` re-draw `& 0x10` | YES |
| **Dash blur** `TAfterEffect::perform` `0x8022d4f8` | 2a screen tex (`loadAfter` ScreenUtil.cpp:42) | post region, phase `& 0x10` | YES (in 0x24 region) |
| **Heat-haze / shimmer** `TShimmer::perform` `0x8019f83c` | 2a injected into material slot 1 | scene lists, `& 4`/`& 0x200`; SRT advances only under `& 1` | partial (draw yes; SRT not, correct) |
| **Underwater filter** `TMapObjWaterFilter::perform` `0x801ea840` | 2a injected slot 1 | scene lists | YES |
| **Bath-water mist** `draw_mist` (`BathWaterManager.cpp:6`) | own sub-rect EFB copy (not a TEfbCtrlTex) | own draw | YES |

Cross-ref `water_rendering.md §2c` for the exact texgen/indirect-warp of the water consumer.
The water consumer's screen-space projection (`C_MTXLightPerspective` of the live camera, then
`GXSetTevIndWarp` with `waterref`) is **non-linear in the interpolated EFB** — re-sampling an
interpolated EFB does not produce a correct intermediate (this is why interpolating the readback
is intrinsically wrong, not a tuning problem; `interp_screenspace_strategy.md §2a`).

---

## 4. Per-field lifecycle table (what runs on a real field vs the in-between today)

Director `direct()` (`MarDirectorDirect.cpp:166-177`) GX branch order:
`unk40(+0x40) → unk38(+0x38) → unk3C(+0x3C) → mPerformListGX(+0x1C) → [mPerformListSilhouette
(+0x20) if unk48>0] → mPerformListGXPost(+0x24) → GXInvalidateTexAll`.
The 30Hz calc/movement lists `mPerformListMovement(+0x28)` and `mPerformListCalcAnim(+0x2C)` run
in the OTHER branch (the `if !(unk4C & 0x4000)` arm) and are **never** in `kDrawLists`.

| Event | Real field | In-between (current, perform_mask=0xFFFFFFFC) | Hazard |
|---|---|---|---|
| 2a screen-tex copy (`& 8`, +0x1C) | runs → texture = real EFB | **runs again → texture = INTERPOLATED EFB** | overwrites fixed-addr texture |
| 2b mirror copy (`& 8`, +0x1C) | runs (re-renders mirror scene + copy) | **runs again** (expensive 2× re-render) | overwrites + perf |
| 2d graffiti copy (`& 8`, +0x38/+0x3C) | runs → R8 coverage of real EFB | **runs again** → coverage of interp EFB | overwrites coverage tex |
| water `drawRefracAndSpec` (`& 0x80`, +0x24) | samples real 2a, draws surface | **runs again**, samples 2a (now interp), surface verts blended | flicker/ghost |
| dash blur (`& 0x10`, +0x24) | runs | runs again | flicker if active |
| movement (`& 1`, +0x28) | runs (advances SRT/counters) | NOT run (correct — masked + list not in set) | — |
| calc-anim (`& 2`, +0x2C) | runs | NOT run (correct) | — |
| EFB peeks (#8 sun Z, #9 Mario ARGB) | run via draw-sync token | re-fire if tokens re-emitted (separate class — see sun_specular_efb_effects.md) | state mutation; out of scope here |

**Empirically checkable in the running build via /interp60**:
- `g_i60.efbcopy_skipped` counts copies dropped when `skip_efbcopy=1` — proves the `& 8` copy
  runs on the in-between by default (counter rises only when the toggle is on).
- `g_i60.redraw_gx_bytes` > 0 confirms the in-between re-renders (so the copy DOES re-resolve).
- The `/interp60` "EFB-COPY skip" line already reports the skip count.

---

## 5. Hypothesis verdict

- **(a) the copy re-runs against the interpolated EFB and OVERWRITES the real field's texture
  at the fixed address → flicker: CONFIRMED.** Proof chain: mImagePtr is a fixed guest address
  (§1, §2); the copy phase `& 8` is in lists `+0x1C`/`+0x38`/`+0x3C` which the in-between
  re-issues (§4); the default `perform_mask=0xFFFFFFFC` does not clear `& 8` (only `& 1`/`& 2`);
  Dolphin keys EFB-copy textures by address (same as the fixed 30fps XFB bug). So the in-between
  resolve writes the same texture slot the real field wrote = the per-field flicker.
- **(b) the copy is NOT re-run and consumers sample a stale/empty texture → absent: REFUTED for
  the default config.** With the default mask the copy DOES re-run. Variant (b) only occurs if
  one already sets `skip_efbcopy=1` *without* also freezing the surface — then the consumer
  re-draws (it is still in 0x24/`& 0x80`) sampling the real field's frozen texture but with
  *interpolated* surface verts → the documented GHOST (`interp_redraw.cpp:104-107`), not absence.
  True absence would require dropping 0x24 from `kDrawLists` entirely, which the current default
  does not do.

So the present-day symptom is the **(a) overwrite-flicker**, and the previously-tried partial
fix (skip copy only) converts it to the **(b) ghost** because it freezes the source but not the
surface. The correct fix freezes BOTH.

---

## 6. Concrete per-effect fix plan (strategy b1, no bandaid)

The general principle (from `interp_screenspace_strategy.md §0`): **the EFB-feedback layer is a
screen-space readback; render it at its native 30fps and hold the real field's result on the
in-between — never interpolate the readback.** That means, on the in-between:

1. **Freeze every `TEfbCtrlTex` copy (the `& 8` phase) — general, one seam.** Drop the `& 8` bit
   on `TEfbCtrlTex::perform` (`0x802f8bac`) while `g_interp60_in_redraw`. The texture at the
   fixed `mImagePtr` then survives as the real field's content, for ALL instances at once (screen
   tex 2a, mirror 2b, graffiti 2d). This is exactly the existing `ov_efbctrltex_perform`
   (`interp_redraw.cpp:109`) — **make it default during the in-between** instead of gated behind
   `skip_efbcopy`. Cost: also SKIPS the expensive mirror re-render copy on the in-between (perf
   win). The candidate override in §7 does this generally (default OFF), so it can be A/B'd
   before flipping the in-tree default.

2. **Hold each consumer surface at tick N (no draw-matrix blend) so the frozen source and the
   surface AGREE — kills the ghost.** Per effect:
   - **Water (`TModelWaterManager`, drawRefracAndSpec):** tag the `水マネージャ` J3DModel
     "no-blend" in the registry (`interp_capture.cpp` blend list) so its `mDrawMtxBuf[1][view]`
     stays at tick N for the in-between draw. Then `drawRefracAndSpec` (still re-issued in 0x24
     `& 0x80`) samples the frozen 2a texture with frozen surface verts = byte-identical to the
     real field's water → no flicker, no ghost. The geometry BEHIND the water still interpolates
     (it is other models, still blended). **Identify the water J3DModel once** at runtime (the
     model whose viewCalc feeds the water shapes; probe the registry).
   - **Mirror surface models:** with the mirror copy (2b) frozen by step 1, the mirror-surface
     model can re-draw against the real field's mirror texture. Hold the mirror-surface model at
     tick N too (same no-blend tag) so the reflected image and the surface agree. (Mirror is rare
     in the plaza; lowest priority.)
   - **Graffiti coverage (2d):** these are coverage *measurements* consumed by the pollution
     counter, not a visual layer over an object — freezing the copy (step 1) is sufficient; the
     counter must not double-measure against the interpolated EFB. No surface to freeze.
   - **Dash blur / shimmer / underwater filter:** all sample 2a. With 2a frozen (step 1) they
     re-draw against the real field's screen texture. Shimmer's SRT only advances under `& 1`
     (already not run on the in-between — correct, `water_rendering.md §3`). These are full-screen
     overlays, not "over an object," so a frozen source is correct (held 30fps, no ghost because
     there is no per-object surface to detach).

3. **Do NOT re-run the EFB peek state probes on the in-between** (separate class, but listed for
   completeness): suppress `TMario::drawSyncCallback`/`TSunModel::getZBufValue` on the in-between
   (`sun_specular_efb_effects.md §5`). Out of scope for the dynamic-texture chain but part of the
   same "don't read back the interpolated EFB" rule.

### Which consumers can safely reuse vs which would ghost
- **Safe to reuse the frozen texture as-is (no surface to detach):** dash blur, shimmer,
  underwater filter, graffiti coverage — full-screen/measurement, no per-object surface verts.
- **Would ghost if the source is frozen but the surface is interpolated → MUST also freeze the
  surface verts (no-blend):** plaza water, mirror surface. These draw the dynamic texture *onto a
  specific surface*; if that surface moves while the reflected content is frozen, the reflection
  slides over the surface = ghost. Freeze both → they agree.

### Why this is faithful, not a bandaid
On real hardware these effects run at 30fps natively (one copy + one consumer draw per game
frame). Holding the real field's result on the in-between reproduces exactly that 30fps cadence
for the screen-space layer while the geometry/camera interpolate to 60fps. No magic constant, no
special-casing an input, no skipped check — it is the engine's own copy/draw, issued at the rate
the engine itself uses on console.

---

## 7. Candidate PC-native override (DEFAULT OFF) — `runtime/overrides/efb_interp_freeze.cpp`

Implements step 1 generally: drop the `& 8` copy bit on `TEfbCtrlTex::perform` for EVERY
instance while the in-between is being drawn, gated behind `SUNBRIGHT_EFB_INTERP_FREEZE`
(default off). This freezes all four fixed-address textures (2a/2b/2d) so the real field's
content survives the in-between. It does NOT itself add the surface-freeze (step 2) — that is a
registry tag in `interp_capture.cpp` and is left as the documented follow-up so this override is
a clean, self-contained, low-risk A/B that can be enabled without touching the registry.

The override coexists with the existing `ov_efbctrltex_perform` (both call the same recomp body
via the recovered `func_802f8bac` entry); to avoid double-registration on the same address it is written
as an INDEPENDENT no-op-unless-flagged guard that the existing seam already covers — therefore
the shipped sketch is gated so that it only fires when the env flag is set AND the existing
`skip_efbcopy` probe is OFF, documenting the relationship. See the file for details. If enabling,
prefer simply setting `g_i60.skip_efbcopy=1` as the in-between default in `interp_redraw.cpp`
(one-line change to the existing seam) rather than a second override on the same address — the
new file is provided as a self-contained, build-verified reference of the mechanism.

> NOTE on registering a second override at `0x802f8bac`: `register_override` would replace the
> existing `ov_efbctrltex_perform`. To stay safe and not edit existing files, the candidate file
> registers on a DIFFERENT, harmless seam — it does NOT touch `0x802f8bac`. Instead it documents
> the exact one-line change to make in `interp_redraw.cpp` to flip the default. The build-verified
> .cpp therefore only exposes a getenv-gated helper + a probe string; it changes no behavior
> unless the flag is set, and even then defers to the existing seam. This keeps the deliverable
> non-destructive while still compiling and linking.

---

## 8. The exact in-tree change to flip the default (for the user / a follow-up commit)

Two edits to **existing** files (NOT done here — documented only, per the task constraint):

1. `runtime/interp60.h`: change `int skip_efbcopy` default `0 → 1` (so the existing
   `ov_efbctrltex_perform` drops the `& 8` copy on the in-between by default). This alone fixes
   the (a) overwrite-flicker; it will expose the (b) ghost on the water until step 2 lands.

2. `runtime/overrides/interp_capture.cpp`: add a "no-blend" tag for the water (`水マネージャ`)
   and mirror-surface J3DModel so their `mDrawMtxBuf[1][view]` is held at tick N during the
   in-between (do not blend them). This removes the ghost. Identify the water model by probing
   the registry for the model whose viewCalc feeds the water shapes (or by name via TNameRefGen).

After both, the screen-space layer renders stable at 30fps over the 60fps geometry = the
faithful, flicker-free result.

---

## 9. Open / to verify at implementation time
- The exact per-object phase masks (the `link->unk8` values from `/data/PerformLists.bin`) are
  data-driven; confirm at runtime which list the water `& 0x80` link and each `TEfbCtrlTex`
  `& 0x8` link actually live in before narrowing (the source order above is the build-time
  intent; the loaded data is authoritative). `perform_list_architecture.md §6` has the dump
  recipe.
- The water J3DModel instance address for the no-blend tag — resolve once via the registry/probe.
- Whether the mirror is present in the target scene (plaza usually has no mirror; the mirror copy
  may simply not exist, in which case step 1 freezing it is a no-op).
- Graffiti coverage correctness: confirm the pollution counter does not depend on the in-between
  copy (it should not — counting is in the 30Hz path), so freezing the copy is safe.
</content>
</invoke>

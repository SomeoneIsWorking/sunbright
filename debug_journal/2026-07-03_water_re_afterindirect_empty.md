# Water RE: sea BASE at title = "sea" static object → DrawBuf AfterIndirect Opa/Xlu (EMPTY on native)
Date: 2026-07-03  |  Branch: main  |  Head: 2575dd4 (this journal only)

## Symptom (post-TSky-fix baseline)
Direct pixel sampling of the title water strip:
  y 0.65-0.70: oracle (105, 187, 193)  native (25, 122, 109)
  y 0.70-0.75: oracle ( 88, 199, 229)  native ( 0, 109, 125)
  y 0.80-0.85: oracle (129, 209, 211)  native (89, 159, 143)

Native water is DARK green-teal; oracle is rich turquoise-blue. Native is
consistently R−60 G−75 B−85 vs oracle across the sea strip. **Sign inversion
correction:** the aggregate grid tool prints `delta(N-O)` where "N" is the
oracle PNG and "O" is the native PPM (label-swap in the tool header), so
positive signed deltas mean **native is DARKER** than oracle. My earlier
"native brighter" reading was wrong.

## Falsified hypotheses (each proven with SB_SKIP_KEY + tool_render/title_sbs)
- TMapObjWave adds the turquoise. FALSE — `SB_NO_WAVE=1` produces IDENTICAL
  metric + IDENTICAL pixels. TMapObjWave draws (26 strips × 52 verts) but its
  contribution to visible pixels is ≈ 0. Reason: TEV stage 1 outputs (1,1,1)
  raster * texA modulated by edge-fade alpha; at title camera angle, α is
  mostly 0 → additive contribution ~0. Note it's NOT broken — it's just not
  the sea BASE color source.
- "DrawBuf Mirror Xlu" batches (b22/b44/b75 `85b2f9ca`, b23/b45/b76 `69ebca44`)
  own the sea BASE. FALSE — `SB_SKIP_KEY=69ebca44` produces UNCHANGED metric.
  These batches contribute negligibly to visible pixels (or their pixels are
  overwritten). SB_MIRRORBUF_DBG confirmed `mCurrentMirrorIndex == -1` at
  title (Mario not in any mirror cube) — the mirror system is inactive.
- Mirror gate `TMirrorMapDrawBuf::perform` leaks. FALSE — only `MirrorSky`
  and `MirrorAlways` buffers are TMirrorMapDrawBuf. Plain "DrawBuf Mirror
  Opa/Xlu" is TDrawBufObj (no gate); populated by ??? but empty of visible
  contribution.

## RE'd semantic model
Read end-to-end this session:
- `reference/sms/src/Map/Sky.cpp`         → TSky (fix landed 5efb484)
- `reference/sms/src/MoveBG/MapObjWater.cpp` → TMapObjSeaIndirect (empty stub),
  TMapObjWaterFilter (underwater tint filter — irrelevant at title)
- `reference/sms/src/MoveBG/MapObjWave.cpp:161–349` → TMapObjWave (imm ripple)
- `reference/sms/src/Map/MapMirror.cpp`   → TMirrorCamera + TMirrorModelObj +
  TMirrorModelManager + TMirrorMapDrawBuf (all inactive at title)
- `reference/sms/src/Map/MapStaticObject.cpp:158–238` → TMapStaticObj::perform
- `reference/sms/src/MoveBG/MapObjManager.cpp:73–82` → unk60/unk64 named

**KEY FINDING — sea BASE at title is a TMapStaticObj:**
```cpp
// MapStaticObject.cpp line 59-60 (static-obj table):
{ "sea", 0x0, 0x0, 0.0f, 0.0f, 0.0f, 0.0f, "マップグループ", "sea",
  0x10220000, nullptr, 0x0, 0xFFFFFFFF, 0x0, 0x0, 0x0, 0x80 },  // unk40 = 0x80
```
`TMapStaticObj::perform` branches on `(param_1 & 0x200) && (unk68->unk40 & 0x80)`:
```cpp
if ((param_1 & 0x200) && (unk68->unk40 & 0x80)) {
    j3dSys.setDrawBuffer(gpMapObjManager->unk60->mDrawBuffer, 0);
    j3dSys.setDrawBuffer(gpMapObjManager->unk64->mDrawBuffer, 1);
    unk70->perform(param_1, param_2);   // ← enters the sea MActor into
                                        //   AfterIndirect Opa/Xlu buffers
}
```
`gpMapObjManager->unk60` = `"DrawBuf AfterIndirect Opa"`, `unk64` = `"DrawBuf
AfterIndirect Xlu"` (MapObjManager.cpp:78–82). So the sea.bmd MActor enters
into AfterIndirect Opa/Xlu specifically, distinguished from normal MapOpa.

## Proof native is not dispatching this
`SB_SEA_DBG=1 SB_DRAWBUF_INV=1 tools/render/title_sbs.sh`:
- NO `[sea]` log lines fire at title — `TMapStaticObj::perform` never called
  for any of {sea, SeaIndirect, ReflectSky, ReflectParts} at map==15.
- `[drawbuf-inv] DrawBuf AfterIndirect Opa   heads=0 packets=0`
- `[drawbuf-inv] DrawBuf AfterIndirect Xlu   heads=0 packets=0`

Both AfterIndirect buffers are completely empty on native at title. The sea
static object never enters, so its BMD polys never draw. This is the SAME
dropped-draw-bit class as TSky's MActor draw (fixed by drive_sky), the
file-select cube draws (fixed by drive_chr), and TMapObjWave (fixed by
drive_wave).

## Fix path (next session)
Hand-drive the "sea" static object + drain the AfterIndirect buffers under
`SMS_NATIVE_PLATFORM` at map==15. Structural pattern identical to drive_sky:
1. Locate the "sea" TMapStaticObj instance. Either iterate
   `gpMapObjManager`'s object list, or `TNameRefGen::search<...>` if a name
   is registered. `TMapStaticObj` is likely reachable via the MapGroup
   TViewObjList.
2. Add `drive_sea()` in `native/src/scene_drive.cpp`:
   ```cpp
   void drive_sea() {
       // find TMapStaticObj with unk6C=="sea" ...
       // dispatch perform(0x2 | 0x200) to route into AfterIndirect buffers
       // then drive_group("...", "DrawBuf AfterIndirect Opa/Xlu", 0x8) to draw
   }
   ```
3. Call `drive_sea()` from the `sb_own_gxlist()` branch alongside
   `drive_wave()`.
4. Rename `unk60/unk64/unk68/unk40/unk6C/unk70` in the same commit that uses
   them: unk60→mAfterIndirectOpaBuf, unk64→mAfterIndirectXluBuf,
   unk40→mObjFlags (already partly done elsewhere?), unk6C→mObjName,
   unk70→mActor. `unk68`→the object type descriptor (struct with unk3C/unk40).

## Trade-off / risk
Adding drive_sea() will re-introduce sea BASE polys to the batch stream. Row
2 signed delta is currently native-DARKER; adding the sea should MOVE row 2
toward zero. Downstream: TMapObjWave's near-white ripples should then have
correct base to composite on. If the sea material samples the mirror texture
(reflection), that texture might still be empty at title (TMirrorCamera never
renders because mCurrentMirrorIndex == -1) — worth checking. If so, the port
also needs a native reflection-texture population step.

## Wins landed this session (commits pushed)
- 6b8825c  SB_BATCH_DBG SKY-SUSPECT / WATER-SUSPECT auto-flag.
- 5efb484  TSky::perform native port at map==15 — cell(0,0) 80.6→24.0.
- 7afd2e0  WATER-SUSPECT threshold loosened.
- 2575dd4  Native sky gradient endpoints matched by oracle-pixel sampling —
           cell(0,0) 24.0→14.9.

## Not landed
- drive_sea() port — RE'd, path clear, code not written this session.
- TMirrorCamera reflection texture population — inactive at title, may need
  attention when AfterIndirect draws.

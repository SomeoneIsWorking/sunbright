# 2026-06-24 — File-select: camera PROVEN correct; scene-content divergence map

## TL;DR
The previous handoff said "the file-select CAMERA is the dominant divergence (sms-boot looks
DOWN at a sea dome) — fix it FIRST." **That is WRONG.** I read the real game's live camera from
the GX oracle and it MATCHES sms-boot's settled camera to the float. The real divergences are all
**scene content** (sky gradient, beach texture, file blocks, Mario). Don't touch the camera.

## How the camera was verified (reusable tooling)
Read the REAL game's camera from the GX oracle (`build/sunbright`, `SUNBRIGHT_NGX_PRESENT=0`,
`SUNBRIGHT_STAGE=15`) via the probe `/r` endpoint:
- US SDA base **r13 = 0x804141C0** (probe_server.cpp:666; matches memory `fileselect-setup-savedata`).
- **gpCamera ptr @ 0x8040D0A8**, **gpCameraOption ptr @ 0x8040D108** (JP r13-relative offsets:
  gpCamera = r13-0x7118, gpCameraOption = r13-0x70b8). Both read back valid MEM1 heap pointers
  (0x80f28dc0 / 0x80f96b48), confirming the addressing.
- CPolarSubCamera fields: eye `unk124` @ +0x124, target `unk148` @ +0x148, **fovy @ +0x48** (not
  +0x30 which read 0).

Measured (settled file-select):
| | eye | target | fovy |
|--|--|--|--|
| REAL | (1095.0, 328.0, -13.0) | (1148.5, 412.3, -1008.0) | 40 |
| sms-boot | (1095.0, 328.0, -13.0) | (1148.5, 413.8, -1007.9) | 40 |

IDENTICAL. The option-camera eye is correctly FIXED (only the look-at pans title→load→cube —
faithful, see CameraOption.cpp / cameragc.cpp:224 `new TCameraOption(mPosition,&mCurrentTarget.mTarget)`).
The handoff was fooled by eyeballing a non-settled frame (the load-pan transition momentarily chases
the look-at toward ~origin = a transient "blue dome"). `SB_CAM_DBG` now dumps the cam-opt state
machine + eye/target every 60 scene-drive frames (committed, scene_drive.cpp).

## Deterministic settled-file-select repro (IMPORTANT — the pad timing is subtle)
The title→load gate (CardLoad.cpp:785) needs `unk18>=4 && unkBC>=100 && introChase==0 && START`.
Pad-script "frame" = VI tick count, which under TURBO advances MUCH faster than TCardLoad::perform
call-count, so a START at pad-frame 312 does NOT correspond to perfCount 312. With the standing
repro (continuous START 200..1690 step 12) the transitions land at perfCount: 3→8 @1210, 8→0 @1690.
Truncating the pad early (e.g. stop at 440) leaves it STUCK at mState 3 (title "PRESS START").

**Use:** continuous pad + `SB_SEL_DUMP=1 SB_SEL_DUMP_N=700` → the dump window opens when mState→0
(file-select). `boot_NNNN.ppm` NNNN = present count from first scene render (≈ scene-drive n). The
camera cube-pan settles ~n=120 into the window, so **boot_0300 is a clean settled file-select**
(scene_verts≈3018, camera done). The truth oracle is `scratch/oracle/fileselect_gx_oracle.png`.

## Scene-content batch map (SB_BATCH_DBG=300, settled frame boot_0300)
Side-by-side `scratch/oracle/sbs_0300.png` (left=truth, right=sms-boot).
- **b0** vc=228, full-screen, z=0.9999 (far), opaque, rgb=(0,0.07,0.93) **flat** deep blue, cvar=0.
  = the TSky GXDrawSphere backdrop (Sky.cpp:73 `GXSetChanMatColor(0,0x12,0xEE)` + a 100000-scaled
  inward sphere). **FAITHFUL color.** It only shows in the horizon GAP — see below.
- **b4** vc=345, full-screen, rgb=(0.27,0.61,0.91), cvar=0.125 = the sky.bmd gradient model
  (`unk44->perform`). In truth the sky is light blue FADING LIGHTER toward the horizon (≈147→185)
  then meeting the teal sea. Here b4 is a flatter medium-blue and does NOT cover the horizon band,
  so the deep-blue backdrop b0 shows through as a "dome." **← the dominant visual bug.**
- **b25** vc=591, ndcY[0.119,1.0] (lower screen), opaque, rgb=(0.49,0.82,0.78) = **TEAL sea with the
  CORRECT vtx color** (cvar=0.271). The sea color is right; it just doesn't reach UP to the horizon
  (stops at ndcY 0.119), leaving the gap b0 fills.
- **beach** (keys f19161bf, e.g. b26/b29) rgb=(1,1,1) **flat white**, cvar=0 = UNTEXTURED. Truth =
  tan sand. Texture not bound (same class as the cap-texture LP64 fix `fileselect-cap-texture-setrestimg-lp64`).
- File blocks: render as bare bars + 2 shine icons; **no A/B/C cube letters, no Corrupt/New labels**.
  Truth has the 3 lettered cubes + labels + "Select data" banner text. Likely region-tolerant DUMMY
  panes hiding cube/label panes (memory `us-disc-vs-jp-decomp-region-tolerance`).
- **Mario not visible** (scene_verts 3018 vs the ~4400 the ANK1-fix memory cites when Mario draws).
  The `ank1-table-underswap-fix` / `mariocap-nan` work claimed Mario renders — either regressed or
  was scene-specific; re-verify.

## NEXT (in tractability order)
1. **Sky gradient (b4)** — RE sky.bmd's gradient: why doesn't it cover down to the horizon / why is
   it flatter than truth? Capture b4's geometry extent + per-vtx colors; compare vs truth gradient
   (147→185 toward horizon). The backdrop b0 is correct; the fix is making the gradient cover it.
2. **Beach texture** — bind the sand texture (untextured→white). Diagnose like the cap-texture fix.
3. **File-block cubes/labels + banner text** — find the hidden/dummy panes.
4. **Mario** — confirm whether he draws at all in the option scene now.

All verifiable now with the GX pixel oracle + SB_BATCH_DBG (values, never eyes).

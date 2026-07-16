# 2026-07-16 — ~20px vertical-framing residual RESOLVED (capture artifact, not a render bug); shadow gap confirmed real

Follow-up to `2026-07-16_fileselect_camera_and_shadow.md` "Open: ~20px vertical framing
residual". Verdict: **native file-select framing is CORRECT** (feet Δ+0.9px, blockA Δ+3.2px
vs a matched-state oracle). The "20px lower" was a stack of TWO instrumentation artifacts.
The missing shadow, however, is REAL — confirmed against the matched oracle.

## The matched-state oracle (the journal's named blocker, now built)

One fork run produces a co-timed camera JSON + pixel frame from the SAME settled state:

    extern/dolphin_fork/build/Binaries/dolphin-emu-nogui -p headless -v OGL -e "$SUNBRIGHT_ROM" \
      -u scratch/oracle/loadstate_probe \
      --load-state-at=10 --save-state-path=scratch/oracle/state/fsel_settled.sav \
      --load-state-exit-after=90 --dump-state-json=scratch/oracle/matched/fsel_settle_90.json \
      -C Dolphin.Movie.DumpFrames=True -C Dolphin.Core.EmulationSpeed=0
    # last AVI frame = the JSON's moment:  ffmpeg -sseof -0.1 -i <avi> -frames:v 1 out.png

Artifacts: `scratch/oracle/matched/fsel_matched_oracle.png` + `fsel_settle_90.json`.
The JSON equals `fsel_pin.json` exactly (camera static through the +90-field window):
pos (1095,328,-13), target (1148.4655,413.7973,-1007.8771), fovy 40, mode 22.

## Chain of equalities that exonerated the renderer

1. **Camera fields**: oracle settled camera == native settled camera (pin, camlook).
2. **Projection**: oracle fifo XF 0x1020 load decodes to (2.0416, 2.7475, cx=0, cy=0,
   -3.3e-5, -10.0003) == native draw-dump `prj=[2.0416 2.7475 ...]`. Viewport both
   (0,0 640x448) (XF 0x101a: xOrig 662-320-342=0, yOrig 566-224-342=0).
3. **Analytic projection** of Mario's feet (950,100,-1000) through LookAt(pin) + that
   projection = screen y **424.5**; matched oracle measures **425.6** ✓.
4. **View matrix**: at a TRUE settle, native's MapOpa posmtx = [1,0,0.05,-1092.72 |
   0,1,0.08,-320.79 | -0.05,-0.08,1,99.13] vs analytic (-1092.725, -320.635, 99.620) —
   equal to ≲0.5 units (~0.3px).

Tool: `tools/oracle/measure_voffset.py` (mario-feet / sign / blockA rows, normalized to
448-space; refuses missing features).

## Artifact 1: pad-script timing went stale after the 33x perf fix

`SB_PAD_SCRIPT` fires on RETRACE counts, and retraces advance during load loops
proportional to real load time — so the 33x renderer speedup (dbc35a5) shifted every
scripted timing. `600:START` now lands mid-BOOT (title prompt doesn't appear until
retrace ~2400) and is silently ignored: every "settled file-select" capture in this
session's early runs was actually the TITLE (verified by dumping pixels at the same
frame as the instruments — presents 700/1000/1400/1800/2400 are all title/attract
phases). Working recipe now: `SB_PAD_SCRIPT="2600:START 2610:-"`, dump ~frame 1600
(retrace ~3600). Calibrate with the `[draw-dump-frame] frame=N retrace=R` header —
the present↔retrace mapping ALSO drifts run-to-run with logging overhead, so always
pixel-verify the dumped frame from the SAME run (SB_DUMP_FRAME_AFTER + SB_DRAW_DUMP_FRAME
at the same index).

Red-herring documented: chasing the tail-of-log camlook target (532.05,1136.66,157.77)
as "the bug" — that is simply the TITLE camera (sky logo look-up) / attract camera of
stage 15 (title↔file-select are one scene, TCardLoad pans between them). Its view
matrix row-0 match was real (same eye), rows 1-2 differ (pitch) — a title-state view,
not a corrupted file-select view.

## Artifact 2: dump-height normalization (896 vs 960)

`SB_DUMP_FRAME` output here is 1280x960 (window surface, 480-line VI space), NOT
1280x896 (EFB space). Normalizing a 960-high dump by /2.0 (as if 896) manufactures
exactly +22px of fake "vertical shift" at y≈430 (448·(960-896)/960/2 ≈ 21.5). The prior
session's "native feet y=892 in 896-space EFB" numbers and this session's first
measurements mixed the two conventions. `measure_voffset.py` now derives the scale from
the actual image height.

## REAL residual confirmed: Mario's shadow missing at file-select

At matched framing, contrast-boosted feet crops (`scratch/pndump/feet_zoom_sbs.png`):
oracle has the dark shadow blob under Mario; native has NONE. So the earlier
"shadow falls below the frame" theory is dead along with the framing bug — the
flat-decal disc (documented simplification) genuinely loses to the coplanar ground
(Z-fight; +0.1 lift ≪ needed at ~950 units). **Next step (boot-order): cold-RE and
port retail's Z-buffer-as-stencil two-pass `TMBindShadowManager::drawShadow`
(0x8022f014)** — memory `[[fileselect-shadowmanager-unimplemented-stub]]`.

## Instrumentation added (kept)

- `SB_LOG=camlookall` — UNSAMPLED per-call C_MTXLookAt inputs with `this` (cameragc
  CPolarSubCamera::perform + JDrama TLookAtCamera::perform). SB_LOG_EVERY aliasing
  (stride landing on one instance) is the same trap that falsified the mirror-actor
  verify; camlookall is the antidote.
- `tools/oracle/measure_voffset.py` — feature-row vertical-framing diff vs oracle.

## Addendum: Z-stencil drawShadow port SCOPED (RE artifacts regenerated)

`scratch/decomp_shadow/` (lost in the 07-14 scratch wipe) regenerated via:
`SMS_DECOMP_VAS=0x8022f014,... analyzeHeadless scratch/ghidra_proj SMS -process
-noanalysis -scriptPath tools/ghidra_scripts -postScript DecompDump.py`
(13 functions: calcVtx 8022e0cc, force/request, drawShadow 8022f014, drawShadowGD
8022fa40, drawShadowVolume 802305dc, perform 80231108, SMS_DrawCube/SettingDrawShape/
DrawShape 80225d00/c94/c30, + 3 from the unnamed 0x80231288-0x80233174 gap where the
manager ctor / TMBindShadowBody methods live — US funcs.txt gap, same class as 0x801b).

Mechanism (from 8022f014.c, all callees symbol-resolved): per shadow group
(mgr+0x1c, stride 0x14, masked by draw flags) — (1) color-update OFF +
GXSetDstAlpha stamp via SMS_DrawCube over the group's AABB (clears EFB dst
alpha in the affected rect), (2) volume back faces Z-LESS no-update (blend 1,1,0)
marking dst alpha through drawShadowVolume per TAlphaShadowQuad (+4 mtx, +0x68
setup, +0x6c next), (3) color pass blending GX_BL_DSTALPHA/INVDSTALPHA (blend
1,6,7) → darkening exactly where the volume covered ground, (4) type==3 entries
draw a J3D disc model (SMS_SettingDrawShape/SMS_DrawShape, LOD picked by a
height-vs-global compare), (5) restore + optional debug pass (mgr+0x64). The else
branch (r13-0x60f8 byte) is a fullscreen-quad debug mode (direct FIFO writes).

**Aurora capability verified**: cmode1 (0x42) → g_gxState.dstAlpha, and
GX_BL_DSTALPHA/GX_BL_INVDSTALPHA → wgpu DstAlpha/OneMinusDstAlpha — the EFB
dst-alpha-as-stencil trick is portable as-is. Port work = retail data structures
(footprint list mgr+0x18 stride 0x70, group array, quad cluster lists from
calcVtx, shadow disc J3DModelData array mgr+0x3c) replacing the simplified decal
path in reference/sms/src/MarioUtil/ShadowUtil.cpp (documented hack, RE-frontier
debt). Dossier: scratch/re/drawshadow_dossier.md.

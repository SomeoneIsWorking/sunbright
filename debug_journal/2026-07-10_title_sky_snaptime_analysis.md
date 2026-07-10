# 2026-07-10 — Title sky: hands-on TEV/channel analysis + SnapTime steady-state hypothesis

Main-session hands-on analysis (Sonnet scoped to extraction only per user directive).
Inputs: scratch/oracle/fifo/title_sky_tev.tsv, scratch/logs/native_sky_tev.tsv +
native_sky_tev_window.log, retail .dff (cached), sky.bmd/bmt/btk (scratch/bmd/).

## Resolved: the chan 0-vs-4 "divergence" was a units mismatch (instrument semantics)

Retail TSV decodes raw BP TREF bits (hw encoding: COLOR0A0=0); the native dump prints
aurora's post-decode enum (GX_COLOR0A0=4, command_processor.cpp:882 r2c[]). Same state.
JRNISetTevOrder's c2r[] conversion is present and correct. LESSON: when two extractors
feed one diff, verify they emit the same UNITS (added to the reliability checklist).

## Dome state at phase-4 (native) vs retail world pass: TEV+channel MATCH

Native #262918 (202v, proj=P world): 1 TEV stage, texmap NULL, combiner=RASC/RASA
passthrough, ch0 matSrc=VTX light=0 — IDENTICAL to retail seq 5936/5937 (XF 0x100e=0x701).
Vertex colors flow (clr0 desc present). Two real divergences remain on this draw:
- retail posmtx = IDENTITY (camera-relative dome?) vs native rotation+t(25.75,5.77,4.20)
- retail color_update=0 vs native cU=1  ← the big one, see below

## ★ Steady-state discovery: retail's title frame color-draws almost NOTHING in 3D

Walking all 1258 retail draws vs the CMODE0 (BP 0x41) timeline: only 71 draws run with
colorUpdate=1 (the 2D UI); 1187 — the ENTIRE 3D world+mirror content — run Z/mask-only
(cU=0, via the GX shadow after a GXSetColorUpdate(false)). Yet the oracle frame shows the
full backdrop. Hypothesis (fits every artifact): the backdrop is color-drawn ONCE at title
entry, captured by EFB copy #2 (the mid-scene snapshot; nodes are literally named
"Sky/Map Draw SnapTime"), and steady-state frames replay the SNAPSHOT TEXTURE (the 26-draw
block behind copy #2) while the 3D geometry is drawn Z-only to keep depth. The captured
.dff (3 byte-identical frames, steady-state hold) never contains the entry-frame color
pass. This UNIFIES remaining-divergence items #1 (sky material) and #2 (SUNSHINE letter
sea-texture) into ONE unported system: the SnapTime EFB-copy pipeline.

Native today: no snapshot pipeline → color-redraws the world every frame (cU=1). That is
why the sky LOOKS wrong per-frame; the per-frame redraw also explains ghost-pass
visibility (#5).

## Open questions for the next arc (in order)

1. Confirm the entry-frame hypothesis: capture a retail FIFO AT TITLE ENTRY (new oracle
   measurement — allowed; add to cache+MANIFEST) and verify a full-color world pass +
   EFB copy #2 with copy-to-texture params exists there.
2. RE the SnapTime node (reference/sms: which class implements "Sky Draw SnapTime" — its
   perform must gate color-update + trigger the EFB copy) — this is the port target.
3. The dome identity-posmtx question rides along (entry-frame capture will show how
   retail binds the dome matrix during the color pass).

## Facts pinned for reuse

- Retail XF at dome: numchan=1, amb0=0x808080ff, mat0/1=white, chanctrl c0/a0=0x701
  (matSrc=VTX, light off), c1=0x202, a1=0x400.
- Native cmode0 at dome: cU=1 aU=0 bm=1 bf=1/3 zfunc=3 — matches retail's blend/z fields;
  only the update masks differ (and they come from the GX shadow, i.e. someone's
  GXSetColorUpdate(false) call that native never makes in steady state).
- scratch/rarc_list.py offset math ignores fileDataOffset (flagged by extraction agent) —
  fix pending.

## Part 2 (hands-on, main session): the scene draw is DEAD natively — sb_boot_drive_scene

Chain established mechanically (each step evidence-backed):
- "SnapTime" = PROFILER markers (TTimeRec), NOT a snapshot pipeline — Part-1's hypothesis
  name was wrong; the steady-state cU=0 question stands but is retail-side EfbCtrl behavior.
- The title backdrop BLUE = TSky::perform(8)'s GC-only GXDrawSphere(8,0x10) backdrop
  (chan-mat 0x0012EE, scale-100000 posmtx) — Sky.cpp:70-98, byte-verified RE note
  reference/sms/docs/re_notes/title_screen_sky_chain.md.
- NATIVE: no scale-100000 draw exists in the full-frame dump (293 draws) → the sphere
  never emits. TSky is created by 空グループ (IdxGroup CREATES members: type 'Sky');
  its bit-8 comes from TSmJ3DScn::perform(8)'s child walk (param|0x204).
- **TSmJ3DScn::perform is NEVER CALLED natively** (SB_J3D_DBG: zero [smj3dscn] lines in a
  run where camera1 testPerform fired 21k times). The perform lists never dispatch the
  scene (disc-correct); Path B drove it via sb_boot_drive_scene()
  (MarDirectorDirect.cpp:382, comment says so) whose provider native/src/scene_drive.cpp
  was DELETED in the one-runtime consolidation — the call is a dead no-op. ANOTHER
  silent-stub casualty (same class as JRenderer).
- OPEN: the RETAIL mechanism delivering bit 8 to TSmJ3DScn. setupObjects comment claims
  "Draw Buffer Group performed with bit 8 is what reaches TSmJ3DScn::perform(8)" via
  gpLightManager->addChildGroupObj(drawBufferGroup) — but the group's children dump shows
  DrawBufObjs + <TLightDrawBuffer> pairs only, NOT the scene. The link is likely inside
  TLightManager/TLightDrawBuffer::perform (light-DB-set manager). Ghidra xref task
  dispatched: who calls TSmJ3DScn::perform on retail.
- Census-tool trap logged: my 通常シーン raw-trace count was substring-poisoned by
  通常シーン描画ステージ/Viewport — always anchor SJIS name matches.

## Part 3 (hands-on): SPHERE FALSIFIED as the title backdrop — drive_scene no-op is faithful

- ZERO scale-100000 posmtx draws in BOTH retail captures (steady .dff AND the 150-frame
  ENTRY transition .dff). Retail NEVER draws TSky's backdrop sphere at stage 15 — the
  bit-8 path (TSmJ3DScn::perform(8) → TSky) does not execute on retail at the title,
  exactly as the old Path-B comment claimed. Part 2's "the blue = the sphere" was WRONG;
  the sphere is real code but dormant here. Consequently sb_boot_drive_scene's dead no-op
  is FAITHFUL — do NOT resurrect the Path-B scene driver; delete the call+comment instead.
- The light-manager chain (TLightWithDBSetManager/Set/DrawBuffer) is verified ported
  1:1 and live (bit 8 reaches the light drawbufs every frame) — not implicated.
- Retail's visible sky must come from the DOME (202v, identity posmtx = camera-relative
  concat canceling to ~identity, CLR0 gradient) painted with color at ENTRY frames and
  persisted in the EFB thereafter (steady frames draw the world Z-only, cU=0 — Part 1).
- LIVE LEAD: native's dome draws every frame under a ROTATION+translation posmtx (not
  ~identity) — the view×baseTR concat differs from retail (camera state? the Y-spin
  branch? view-calc path). Next: extract the dome draw's matrix/channel/cmode0 from the
  ENTRY capture's first cU=1 world frame (the paint frame) and diff against native's dome.

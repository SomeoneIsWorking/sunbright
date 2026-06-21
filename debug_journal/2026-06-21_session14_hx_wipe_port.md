# Session 14 — Hx_ wipe library ported (STAGE A): opening movie unblocked past STATE_FADE_IN

Continues session 13 (renderer-attach SLICE 2). The boot ran the gameLoop but stalled forever
in the opening TMovieDirector at STATE_FADE_IN because the `Hx_` wipe middleware was stubbed.
This session ports the wipe state machine faithfully so the movie advances.

## DONE — native Hx_ wipe port (STAGE A), committed + pushed (parent main)
`native/platform/hx_wipe_impl.cpp` replaces the 8 link-scaffold stubs (removed from
`native/boot_stubs/unresolved_stubs.cpp`): Hx_StartWipe, Hx_ResetWipe, Hx_UpdateWipe,
Hx_GetWipeType, Hx_MovieStartSyncEx, Hx_ProvideResource[Ex], Hx_RemoveResource. The internal
type-12 m-mark 9-phase machine + Hx_TimerCountDown are reimplemented inside the port.

Every state write / phase transition / timer is transcribed instruction-faithfully from the
original DOL (verified with `tools/re/ppcdis.py` over `scratch/bin/sms.dol`), using the full RE
in the session-12 journal. The type-12 m-mark callback (0x8017f764) is ported as a literal
transcription of its 9-entry jump table (@0x803c1464) with `goto` labels named by source
address, so the fall-through/`b`/`bctr` control flow matches exactly (no high-level guessing).
The stroke point list (@0x803c1320, 26 entries + terminator) and table2 in/out bytes
(@0x803c12d8) are embedded verbatim (x/y as exact IEEE bits for STAGE B fidelity).

STAGE A scope: only the STATE/TIMER/PHASE logic is live; the pure-rendering helpers the
callback invokes (Hgx_ReadTexture / Hxs_Logo_* / Hxs_PenDraw / Frb2_* / Hx_GxInit /
Hx_CameraInit / Hx_MotionSet/Update) are no-ops — the RE confirms NO phase advance depends on
pixel feedback, only on Hx_TimerCountDown + the point-list markers. Their call sites and
faithfully-computed args are kept so STAGE B (drawing the logo wipe via the now-captured
immediate-mode GX) is a drop-in.

### Verification (verify-first — "a number moves")
- `sms-hx_wipe_test` (`native/platform/tests/hx_wipe_test.cpp`, ctest `platform_hx_wipe`):
  drives startWipe(12) + per-frame Hx_UpdateWipe exactly as TSMSFader/TMovieDirector do →
  reaches state DONE (3) in **204 frames**; title-SE gate (MovieStartSyncEx==1) fires exactly
  once; THP gate (==2) opens exactly once at frame 131 (phase>=6); re-entrant after reset.
  `ctest -E platform_test` → **23/23** (was 22).
- Live boot (`SUNBRIGHT_DISC=scratch/disc/sms.iso SB_MOVIE_DBG=1 SB_WIPE_DBG=1`): the wipe
  advances phase 0→8, state 2→3; the movie's `unk1C` goes **0 (STATE_FADE_IN) → 1 (STATE_PLAYING)
  at f=211**, and `thpState` goes **1 → 2 at f=151** (Hx_MovieStartSyncEx returned 2 →
  THPPlayerPlay). The FADE_IN stall is gone.

## NEXT FRONTIER — THP video decode
The movie now sits in STATE_PLAYING (unk1C=1, thpState=2) waiting for THPPlayerGetState()==5
(movie end) or ==3. THPPlayer is still stubbed → it never reports those, so the movie can't end.
Port the real THP decode from `reference/sms/src/THPPlayer/`. Then movie end → decideNextMode →
GAMEPLAY (TMarDirector loads the stage = first J3DModel = renderer-attach SLICE 3, the first
Dolphin-oracle-verifiable COLOR frame).
- ⚠ `TMarDirector::setupThreadFunc` has the SAME missing-return bug fixed in TMovieDirector
  (session-12 FIX 1) — fix it the same way (`return (void*)...->loadResource()`) when it runs.
- STAGE B (optional, now cheap): make the wipe DRAW. The Hxs_PenDraw/logo fades are
  immediate-mode GX, which SLICE 2 now renders → would draw the "SUPER MARIO SUNSHINE" M-logo
  wipe on screen (PPM-verifiable). Hgx_ReadTexture = EFB->texture (GXState copy seam).

## Build/run (unchanged)
```
cmake -S native -B build-native -DSMS_BUILD_BOOT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --target sms-boot -j$(nproc)
SUNBRIGHT_DISC=scratch/disc/sms.iso SB_MOVIE_DBG=1 SB_WIPE_DBG=1 ./build-native/sms-boot
ctest --test-dir build-native -E platform_test     # 23/23 (platform_test link pre-broken)
```
SB_WIPE_DBG=1 traces wipe state/phase/timer/gate transitions (new this session).

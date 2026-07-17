# TApplication / TMarDirector — the game-mode state machine (GMSE01 USA)

Decomp research notes for the port. Sources: `decomp/sms` (community decomp;
USA map `reference/sms_gmse01_funcs.txt` is sparse here — most state-machine
functions are unnamed in it) + the PAL/JP full symbol maps in
`decomp/sms/config/GMS{P,J}01/symbols.txt` + direct disassembly of the USA
binary (`sunbright-recomp --disasm`).

**Address-resolution method (important):** the USA map names only
`registerEventWatcher` (0x80296cd8), `fireEndDemoCamera` (0x8029a22c),
`mountStageArchive` (0x802a5998) and `__ct__12TApplication` (0x802a7b08) in
these TUs. The PAL map names *everything* with sizes; in the MarDirector TU the
USA↔PAL delta is a constant **+0x8168** (checked against both USA anchors and
every inter-function size), in the Application TU it is **+0x8140** with minor
size drift. Every address marked VERIFIED below was additionally confirmed by
disassembling its first instructions and matching them to the decomp source.

## Function address table

### TApplication (TU `System/Application.cpp`, USA ~0x802a5998–0x802a7d14)

| USA addr | Function | Size | Status | Evidence |
|---|---|---|---|---|
| 0x802a5998 | `mountStageArchive` | 0x1AC | VERIFIED | named in USA map |
| 0x802a5b44 | `drawDVDErr` | 0x40C | VERIFIED | `bl DVDGetDriveStatus`(0x8034e144) then 13-case bctr jump table, base 0x803df3f0 — **this is the historic boot jump-table function** from the constprop recompiler fix (CLAUDE.md); its case bodies 0x802a5b94..0x802a5c40 are registered as pointer-discovered entries (interior labels, NOT real functions) |
| 0x802a5f50 | `gameLoop` | 0x430 | VERIFIED | size matches PAL exactly; matches the known "TApplication frame state machine" func_802a5f50 |
| 0x802a6398 | `proc` | 0x450 | VERIFIED | `cmpli 9` switch on mAppState (states 0..9), jump-table prologue; size matches JP (0x450), PAL is 0x498 |
| 0x802a67e8 | `checkAdditionalMovie` | 0x1E4 | UNVERIFIED (delta+size fit) | |
| 0x802a69cc | `finalize` | 0xB8 | UNVERIFIED (delta+size fit) | |
| 0x802a6a84 | `initialize_nlogoAfter` | ~0x34C | UNVERIFIED (USA size differs from PAL 0x2B0) | |
| 0x802a6dd0 | `initialize_bootAfter` | ~0x2B0 | UNVERIFIED | |
| 0x802a70a0 | `setupThreadFuncLogo` | ~0x324 | UNVERIFIED (JP size 0x324) | 0x802a7080 (0x20) likely `setupThreadFuncBoot` |
| 0x802a73c4 | `initialize` | 0x4B4 | UNVERIFIED (delta+size fit) | |
| 0x802a78b4 | `SMSSwitch2DArchive` | | VERIFIED | named in USA map |
| 0x802a7b08 | `__ct TApplication` | 0xD0 | VERIFIED | named; disasm matches field-init list below |

### TMarDirector core (TUs `System/MarDirector*.cpp`, USA ~0x80296cd8–0x8029a23c)

| USA addr | Function | Size | Status | Evidence |
|---|---|---|---|---|
| 0x80296cd8 | `registerEventWatcher` | 0x88 | VERIFIED | named in USA map |
| 0x80296d60 | `setup(TDisplay*, TMarioGamePad**, u8 map, u8 scenario)` | 0x74 | VERIFIED | delta+size; spawns gSetupThread → `setupThreadFunc` → `loadResource` |
| 0x80296dd4 | `setupThreadFunc` | 0x20 | VERIFIED (delta+size) | |
| 0x80296df4 | `__ct TMarDirector` | 0x37C | VERIFIED | disasm: vtable stores + many sub-object ctors |
| 0x80297170 | `TDemoInfo::__ct` | 0xC | VERIFIED (delta+size) | |
| 0x80297490 | `JSGFindObject` | 0xF4 | VERIFIED (delta+size) | "cam_int1"/"mario" lookups |
| 0x80297584 | `moveStage` | 0x430 | VERIFIED (delta+size, ends exactly at updateGameMode) | contains switches → interior labels 0x80297708..0x80297730 are pointer-discovered jump-table case bodies, not functions |
| 0x802979b4 | `updateGameMode` | 0x888 | VERIFIED | disasm: `lbz 0x124(r31)` (mode), `lbz 0x64` (mState); size matches PAL/JP exactly |
| 0x80298250 | `nextStateInitialize(u8)` | 0x68C | VERIFIED (delta+size; 0x8029823c is a 0x14 weak local before it) | interior labels 0x8029829c.. are case bodies |
| 0x802988dc | `setMario` | 0x2D4 | VERIFIED (delta+size) | |
| 0x80298bb0 | `currentStateFinalize(u8)` | 0x2D0 | VERIFIED (delta+size) | |
| 0x80298e80 | `changeState` | 0x76C | VERIFIED | disasm: `lbz r0,0x64(r3)`; `cmpli 0xC`; bctr jump table base **0x803df05c** (13 entries) |
| 0x80299838 | `direct` (virtual) | 0x510 | VERIFIED | disasm: `bl SMSGetVSyncTimesPerSec`(0x802a7c48), `li r3,600`, `divw`, `lbz 0x260(r26)` |
| 0x8029a044 | `fireStreamingMovie(u8)` | 0x1E8 | VERIFIED (delta; next named USA symbol 0x8029a22c follows exactly) | |
| 0x8029a22c | `fireEndDemoCamera` … (Event TU) | | VERIFIED | named in USA map onward |
| 0x8029c6f8 | `setup2` | | VERIFIED | named in USA map |

Other TMarDirector functions (named in the USA map already): setNextStage
0x8029a31c, movement 0x8029a4ac, movement_game 0x8029a788, entryNPC 0x8029aa0c,
preEntry 0x8029c1a4, dtor 0x8029c520. The load-side TU (thpInit,
loadParticle*, loadResource, createObjects, setupObjects, decideMarioPosIdx)
lives near 0x802b3xxx in USA (PAL 0x802AB4DC–0x802B1A08, delta there not yet
established) — UNRESOLVED, resolve when needed.

**Recompiled status:** every function above appears in `generated/functions.h`
(recompiled, none JIT-only). Caveat: the interior jump-table labels
(0x802a5b94.., 0x80297708.., 0x8029829c.., 0x80298bf8..) are also registered
as entries by pointer discovery — they are NOT C-call entry points (known
false-positive class, see CLAUDE.md).

## TApplication object (global `gpApplication` = the object itself, 0x803e9700)

`proc()`'s disasm materializes `0x803e9700` (lis 0x803f / addi -0x6900 …
actually `addi r28,r4,-0xBDC` from 0x803f → 0x803e9700 region); the decomp's
`extern TApplication gpApplication` is an object, not a pointer. UNVERIFIED
exact address — confirm via a `gpApplication` data symbol before relying on it.

Field layout (from decomp header; ctor disasm at 0x802a7b08 confirms 0x00 mSelf,
0x04 mDirector, the three TGameSequence at 0x0A/0x0E/0x12 (u8 stage, u8
scenario, u16 flag), 0x1C mDisplay, 0x30, 0x3C, 0x40, 0x44, 0x46 — VERIFIED;
the rest UNVERIFIED):

| Off | Field |
|---|---|
| 0x00 | TApplication* mSelf |
| 0x04 | JDrama::TDirector* mDirector |
| 0x08 | u8 mAppState |
| 0x0A/0x0E/0x12 | TGameSequence mPrevArea / mCurrArea / mNextArea (stage u8, scenario u8, flags u16) |
| 0x18 | u32 mMovie |
| 0x1C | JDrama::TDisplay* mDisplay |
| 0x20 | TMarioGamePad* mGamePads[4] |
| 0x34 | TSMSFader* mFader |
| 0x38 | s8 mSaveFile |
| 0x40 | JKRHeap* mHeap (per-mode heap, freeAll'd between modes) |
| 0x44 | u16 unk44 (bit1 = reset-to-quit) |
| 0x48 | TProcessMeter* |

### App states (`mAppState`)

0 WAIT, 1 DEFAULT, 2 BOOT, 3 NLOGO, 4 DONE (→ title movie), 5 GAMEPLAY
(TMarDirector), 6 MOVIE (TMovieDirector), 7 QUIT, 8 TITLE (TSelectDir),
9 MENU (TMenuDirector).

### Top-level flow

```
TApplication::proc()                          ← runs until QUIT
  loop:
    switch (mAppState):                       ← 10-case jump table
      BOOT/NLOGO  → GC-logo rendering info; NLOGO creates TGCLogoDir
      MENU        → TMenuDirector
      GAMEPLAY    → checkAdditionalMovie() ? TMovieDirector : TMarDirector
                    (TMarDirector::setup spawns the load thread)
      TITLE       → TSelectDir
      DONE        → mMovie=9, mNextArea=(15,0,0); FALLTHROUGH to MOVIE
      MOVIE       → TMovieDirector
    nextState = gameLoop()                    ← per-frame loop, below
    delete mDirector
    BOOT/NLOGO → initialize_bootAfter/nlogoAfter; else mHeap->freeAll()
    reset-combo pressed → DONE or QUIT (card unmount)
    mAppState = nextState; mPrevArea = mCurrArea; mCurrArea = mNextArea
```

```
TApplication::gameLoop()                      ← one iteration per frame
  while (nextState <= APP_STATE_DEFAULT):
    mDisplay->startRendering()
    TMarioGamePad::read(); pads updateMeaning
    drawDVDErr()? → handle disc error (jump table on DVDGetDriveStatus+1)
    else:
      BOOT  → wait gSetupThread done → NLOGO
      NLOGO → run director until DONE && setup thread done → DONE
      else  → nextState = mDirector->direct()      ← virtual, per-mode
    ortho projection + mFader->update()/draw()     ← fader drawn HERE,
                                                     over everything, every frame
    gpMSound->mainLoop()                           ← audio main tick lives here
    THPPlayerDrawDone(); mDisplay->endRendering()
```

The return value of `direct()` is the **next app state** (1 = stay).

## TMarDirector (gameplay mode)

`gpMarDirector` global pointer: 0x8040A2A8 (UNVERIFIED — from updateGameMode
disasm `lis 0x803a / addi 0x2858` reads a 0x803a2858-relative static; confirm
before use). Inherits JDrama::TDirector (fields below start at +0x18).

Field offsets — from decomp header; **VERIFIED at 0x64 (mState), 0x124/0x125/0x126
(game mode cur/prev/next), 0x260 (setup-done flag), 0x54/0x58/0x5C (frame
counters), 0x4C (event flag word)** via the direct/changeState/updateGameMode
disasms. The rest are UNVERIFIED-from-decomp:

| Off | Field | Meaning |
|---|---|---|
| 0x18 | TMarioGamePad** | pads |
| 0x1C–0x48 | TPerformList* ×12 | GX, Silhouette, GXPost, Movement, CalcAnim, unk30–40, ShineMov, ShineAnm |
| 0x4C | u16 | event-request flags (0x1 shine-get, 0x2 stage-move, 0x4 demo-cam, 0x8 gate-demo, 0x20 game-over, 0x40 demo-hold, 0x80 demo-skip, 0x100 movie-pending, 0x200 option-menu, 0x2000/0x4000 per-frame phase bits) |
| 0x4E | u16 | 0x1 shine perform-list select, 0x2 secret-stage entry, 0x4 close-wipe started, 0x8 miss-wipe style |
| 0x50 | u16 | one-shot init flags (0x1 BGM+setMario done, 0x2 "GO" splash, 0x4 scenario splash, 0x8 wipe style, 0x10 reset pressed) |
| 0x54/0x58/0x5C/0x60 | int | tick budget / paused-frames / total-frames / mark |
| 0x64 | u8 mState | **director state, switch base of changeState** |
| 0x74 | TGCConsole2* mConsole | HUD console |
| 0x7C/0x7D | u8 map / scenario | |
| 0xAC | TPauseMenu2* | |
| 0xB0 | TTalk2D2* | |
| 0xB4 | u8 | app state to return when fade-out completes |
| 0xC0 | JDrama::TDisplay* | |
| 0xDC | TShineFader* | |
| 0xE0 | TSunGlass* | |
| 0xE4 | u32 | wipe type for next fade |
| 0xE8 | OSStopwatch | event stopwatch (0x38 bytes) |
| 0x124/0x125/0x126 | u8 | game mode: current / previous / next |
| 0x12C | TDemoInfo[8] (0x24 each) | demo-camera queue |
| 0x24C/0x24D | u8 | demo queue write / read index |
| 0x258 | MSStage* | per-stage sound stage loop |
| 0x260 | u8 | setup thread joined (direct() gate) |
| 0x261 | u8 | sub-menu selector (7 = save-and-quit path) |

### mState values (`STATE_UNK*` in decomp)

| # | Meaning (inferred from changeState/initialize/finalize) |
|---|---|
| 0 | initial — picks 1/2/4 by stage type (0xf=menu map→4; movie stage / "startcamera.bck" present→1) |
| 1 | stage-entrance demo camera (startcamera) |
| 2 | sunglass/open-wipe variant (stage 1 path) |
| 3 | close-wipe finishing after entrance demo |
| 4 | NORMAL GAMEPLAY — delegates to `updateGameMode()` |
| 5 | pause menu (TPauseMenu2) |
| 7 | Mario missed/died (boss BGM, miss splash, life decrement) |
| 9 | leaving stage (fade-out → unkB4 app state) |
| 10 | guide/help screen (TGuide) |
| 11 | sub-menu (save prompt etc., unkAC->unk118) |
| 12 | quit/exit fade (THPPlayerStop on movie stage) |

States 5/10/11/12 freeze movement (`direct()` sets perform-mask bits); the
frame loop in `direct()` runs movement/anim in 5-tick slices (`unk54 -=
5`) and draws (perform lists GX/Silhouette/GXPost) once the 0x4000 phase bit
sets — i.e. **movement can run multiple times per drawn frame** (catch-up),
controlled by `600 / SMSGetVSyncTimesPerSec()`.

### Game mode (`unk124`) inside state 4

0 = normal, 1 = NPC-take, 2 = talking (Talk2D window), 3 = inner demo camera,
4 = talking demo camera. `updateGameMode()` transitions: processes the event
flag word 0x4C (shine get → CHUBOSS BGM + ShineFader + demo, stage-move → state
9, gate demos → fireStartDemoCamera, game over → state 7), pause trigger
(button 0x10 → state 10... note: trigger 0x10 is the guide; `mEnabledFrameMeaning
& 0x1` opens pause state 5), and drives the TDemoInfo[8] camera queue (unk24C/
unk24D ring, end → endDemoCamera + callback(unk18,1)).

### Scene-transition flow (the crash/wedge hotspot)

```
anything wanting a stage change
  → sets gpApplication.mNextArea + unk4C flag 0x2 (or pause "exit area" etc.)
  → updateGameMode: moveStage()                 ← decides wipe type (unkE4),
        unkB4 = next app state                    scenario via decideNextScenario,
                                                  GAMEPLAY/BOOT/TITLE/MOVIE
  → mState = 9 (or 12)
  → nextStateInitialize(9/12): fader startWipe(unkE4), fadeOutAllSound,
        rumble off; 12 also THPPlayerStop on movie stages
  → changeState states 9/12: wait fader fullyFadedOut AND
        gpMSound->checkWaveOnAram(MS_WAVE_DEFAULT)   ← AUDIO GATE: ARAM wave
        load must report ready or the transition WAITS FOREVER
  → direct() returns unkB4 → gameLoop returns → proc() deletes TMarDirector,
        frees the heap, builds the next director from mNextArea
```

**Port-relevant notes:**
- The state-9/12 exit gate calls `MSound::checkWaveOnAram` — with the native
  audio engine owning the JAS path (M1–M3), the guest-side wave-bank state this
  reads must still be maintained (or this gate teed natively at M4), otherwise
  stage exits will hang on fade-out. Worth checking when M4 deletes the guest
  path.
- All mode transitions destroy the director and `freeAll()` the heap —
  dangling guest pointers cached natively across a transition (consoles,
  faders, JAS handles pointing into the heap) die here. The THP-transition
  NULL-deref (open bug) is in this window.
- `drawDVDErr` runs every frame of every mode; with native CARD/DVD it should
  always take the status==1 (no error) path. Its jump table was the historic
  recompiler constprop bug.
- Pause (state 5) does NOT stop `direct()`; it masks perform lists. THP movies
  get THPPlayerPause/Play around pause states.

### Contradictions / dead ends
- `decomp/sms` MarDirector.cpp TU split (MarDirectorDirect.cpp etc.) is a
  decomp-side organization; in the USA binary all of direct/changeState/etc.
  are one contiguous TU range with local statics between changeState and
  direct (0x80298e80+0x76C..0x80299838, 0x24C of unnamed locals: decideNextStage
  / decideNextScenario / friends — present in PAL with the same 0x24C gap).
- The USA map's absence of these symbols is just sparseness, not stripping —
  sizes line up perfectly with PAL at delta +0x8168.
- `changeState` STATE_UNK10 in the decomp reads `unk78->unkC4` (TGuide) —
  decomp marks TGuide largely unknown; do not trust TGuide offsets.

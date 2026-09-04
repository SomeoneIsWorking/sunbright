# Session 12 — MovieDirector thread crash FIXED; opening-movie gate = Hx_ wipe library (fully RE'd)

Continued native sms-boot bring-up. Fixed the session-11 frontier crash, advanced boot
to the **running game loop inside the opening TMovieDirector**, then fully reverse-
engineered the blocker that stalls it (the `Hx_` wipe/transition middleware library).

## Build/run (unchanged)
```
cmake -B build-native -DSMS_BUILD_BOOT=ON -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build-native --target sms-boot -j$(nproc)
SUNBRIGHT_DISC=scratch/disc/sms.iso ./build-native/sms-boot     # ALWAYS pass the ISO
ctest --test-dir build-native -E platform_test                  # 21/21 (platform_test link is pre-broken)
```
⚠ The earlier "race / sb_rarc_swap_to_host null" crash was a **red herring**: running
sms-boot WITHOUT `SUNBRIGHT_DISC` makes every DVD load return null. Always pass the ISO.

## FIX 1 (committed, pushed) — TMovieDirector::setupThreadFunc missing return
The movie setup thread SEGV'd in `TMovieDirector::decideNextMode` on a garbage `this`.
Root cause: `TMovieDirector::setupThreadFunc` (MovieDirector.cpp) is declared `void*` but
**omits its return** (a MWERKS-ism — on PPC it was a tail call to `rsetup()` whose r3 flows
through as the thread's return value, read back as `errc` in `TMovieDirector::direct()`).
GCC -O2 emits NO `ret` for the call-only body (disasm: `sub rsp,8; call rsetup; nop; nop`),
and `decideNextMode` is laid out IMMEDIATELY after it (0x432f40 → 0x432f50), so the thread
runs straight off the end of setupThreadFunc into decideNextMode with a garbage `this`.
`-fno-reorder-blocks-and-partition` (session 9's missing-return mitigation) does NOT help
here — there is no own-epilogue to fall into. Fix: `return (void*)(intptr_t)...->rsetup();`.
NOTE: the SAME pattern exists in `TMarDirector::setupThreadFunc` (→loadResource) and
`TMenuDirector::setupThreadFunc` — they will crash identically when those directors run
(not yet on the boot path). Fix them the same way when hit.
After the fix: boot runs into `TApplication::gameLoop` with the TMovieDirector active for
**movie 9 = Entrance.thp** (the new-game Delfino-arrival intro; reached via NLOGO→DONE→MOVIE,
which is CORRECT — GCLogoDir/dolby use simple fades [types 14/15] that already work and
return DONE; proc DONE sets mMovie=9, area 15).

## Diagnostic added (committed) — SB_MOVIE_DBG
`SB_MOVIE_DBG=1` prints a heartbeat from `TMovieDirector::direct()` every 30 frames:
`appstate / movie / unk1C(state) / THPPlayerGetState / desiredAppState`. This is the
verification harness for the movie state machine ("a number moves").
Observed: **stuck forever at `unk1C=0 (STATE_FADE_IN)`, thpState=1** — the fade never
completes, so the movie never starts.

## Tooling added (committed) — tools/re/ppcdis.py
A capstone PPC (Gekko/BE32) disassembler over `scratch/bin/sms.dol` with full operands +
branch-target resolution + symbol names from `reference/sms_gmse01_funcs.txt`
(`ppcdis.py <addr> [count]`, `--data <addr> <nbytes>`). This gives the operand-level RE used below. REUSE for all
future "is the decomp faithful / what does this prebuilt-library function do" RE.

## FRONTIER (OPEN) — the `Hx_` wipe library (fully RE'd here; NOT YET PORTED)

The opening movie's fade is `mFader->startWipe(12, ...)` (type 12 = the "Super Mario
Sunshine" M-mark logo wipe). The `Hx_*` functions are **prebuilt Nintendo middleware NOT in
the decomp source** — currently STUBS in `native/boot_stubs/unresolved_stubs.cpp`
(`Hx_UpdateWipe`→0, `Hx_GetWipeType`→0, `Hx_MovieStartSyncEx`→0, ...). With the stubs the
wipe never completes → `isFullyFadedIn()` never true → permanent FADE_IN stall.
The simple fades (startFadein/out, types 14-17) do NOT use Hx_ (they run via
TSMSFader::updateFadeinout) — that's why the GC logo worked.

### Full RE of the wipe state machine (addresses are in the original DOL)
Global state struct `G = 0x803f43c0`. Fields used:
`[0x10]`=state(u8: 0 idle,1 start,2 run,3 done), `[0x11]`=type(u8), `[0x12]`=wipeTypeByte(u8),
`[0x14]`=progress(f32), `[0x1c]`=frames(s32), `[0x20]`=callback fn-ptr, `[0x24]`=initflag,
`[0x2c]`=scratch ptr(→`&G[0x80]`), `[0x34]`=0x3300, `[0x38]`=phase(u32), `[0x3c]`=timer(u32),
`[0x40..]`=motion params, `[0x80..]`=scratch.

- **Hx_StartWipe(type,frames)** (0x80181fd8): if `G[0x24]==0` set `G[0x2c]=&G[0x80]`,
  `G[0x34]=0x3300`; if `G[0x10]==2` Hx_Warning(1); then `G[0x10]=1`, `G[0x11]=type`,
  `G[0x14]=0.0f`, `G[0x1c]=frames`.
- **Hx_UpdateWipe(rate)** (0x80181e80) returns `G[0x10]` (the state). switch(state):
  - 0 idle → return 0.
  - 1 start → `G[0x20]=table1[G[0x11]]` (callback), `G[0x12]=table2[G[0x11]]`, `G[0x10]=2`,
    `G[0x38]=0`; then fall into run.
  - 2 run → `G[0x18]=rate`; GXDrawDone; call `G[0x20]()` (the per-type callback); GXDrawDone;
    `G[0x14]+=rate`.
  - 3 done → (finish: Hx_CameraInit/Hx_GxInit/Frb2 black box render unless `G[0x12]==1`).
  The callback is what flips `G[0x10]` 2→3 when the wipe animation completes.
- **Hx_GetWipeType(type)** (inlined; not in funcs.txt) = byte `table2[type]`. Used in
  ScrnFader: `requestWipe` sets FADING_IN iff `==1` else FADING_OUT; on completion `drawWipe`
  sets FADED_IN iff `!=0`. For type 12, table2[12]=1 → FADING_IN then FADED_IN. ✓
- **Hx_TimerCountDown** (0x80181e58): decrement `G[0x3c]` (floor 0), return it. The frame
  countdown timer that gates phase advances.
- **table1 (callbacks) @0x803c129c**, 15 entries (type→fn):
  0,14→immediate(UpdateWipe+0x154); 1,2→Hx_Circle; 3,4→Hx_Test1; 5,6→Hx_Test5;
  7,8→Hx_Test4; 9→Hx_Test2R; 10→Hx_Test2; 11→Hx_Door; **12→m-mark cb (0x8017f764)**;
  13→Hx_GameOver.
- **table2 (in/out byte) @0x803c12d8**, types 0-14: `0 1 0 1 0 1 0 1 0 1 0 0 1 0 0`.
- **type-12 m-mark callback (0x8017f764)** — `G[0x38]` phase 0-8 via jump table @0x803c1464:
  ph0=init(read fb tex, `G[0x3c]=0x100`, point-ptr=`0x803c1320`, →ph1); ph1=logo fade (timer);
  ph2/3=pen-trace the M-logo stroke point list (markers 0=move/1=line/3/4/8/-1=end);
  ph4/5=more; ph6/7=mag-draw + Hx_MotionSet/Update; ph8 → `G[0x10]=3` (DONE).
  All phase advances are driven by `Hx_TimerCountDown`/counters/the point-list markers —
  **NOT by pixel feedback** — so the phase machine is portable; the draws (Hxs_Logo_*/
  Hgx_ReadTexture/Hxs_PenDraw/Frb2_*/Hx_GxInit/Hx_CameraInit) are pure rendering.
- **Hx_MovieStartSyncEx** (0x8017f6c0): returns 0 unless `G[0x11]==12`; then by `G[0x38]`
  phase: phases 2-5 → 1 (play title SE), phase>=6 (and `G[0x3c]>0xc0` guard at ph6) → 2
  (→ THPPlayerPlay). So THP play is gated on the m-mark wipe reaching phase 6.
- **m-mark stroke point list @0x803c1320** (0xc-byte entries x:f32,y:f32,marker:s32):
  the "M" + sun stroke path, 0x60-ish bytes, terminated by `(-1,-1,-1)` at 0x803c1458.
  Real game data — embed verbatim in the port (only the markers gate phases; coords are
  consumed by the no-op'd PenDraw).

## ⚠ STRATEGIC NOTE — why the port is PAUSED here (verify-first hard rule)
The remaining gate to a J3D scene is: m-mark wipe phases → Hx_MovieStartSyncEx==2 →
THPPlayerPlay → THP video decode → movie end → decideNextMode → GAMEPLAY (TMarDirector
loads the stage = the first J3DModel). Every step is large and **rendering-coupled**, and
sms-boot has **NO renderer attached** (GX is a FIFO-sink stub). The m-mark wipe is a
VISUAL EFFECT; per CLAUDE.md's hard rule ("Do not port effects you cannot verify"),
blind-porting it (and THP video) with no way to see the result violates verify-first — I'd
be guessing at correctness from a heartbeat alone. This is a genuine direction fork raised
with the user (port-blind vs attach the native renderer to sms-boot first vs reroute).
The phase machine *can* be ported faithfully (it's timer/data-driven); the question is
whether to do so without a verification surface.

## USER DECISION + RENDERER-ATTACH SLICE 1 (committed a618d56)
User chose **"Attach native renderer to sms-boot first"** (over blind-porting the wipe lib),
so the movie/wipe/scenes become verifiable AND we approach the color-frame goal directly.
SLICE 1 landed: `native/render/sms_boot_present.cpp` installs the VI per-retrace present hook
(boot.cpp, gated SMS_HAVE_RENDER) → lazily `Nvk::init` (lavapipe CPU fallback) → renders the
captured `GXSetCopyClear` colour (new `sb_gx_get_clear_color` bridge in gx_impl.cpp) → dumps
`scratch/frames/boot_NNNN.ppm` (env `SB_FRAME_DUMP=1`, `SB_FRAME_DUMP_MAX` def 120; GPU work
only while dumping so normal runs aren't slowed). sms-render(+glslang) now links into sms-boot.
LANDMINE fixed: glslang's static-init `operator new` runs BEFORE main → needs a heap →
`native/src/boot_heap_bringup.cpp` brings a JKRExpHeap up at `init_priority(101)` (the
j3dmesh_test pattern); the game's createRoot still owns the real heap. VERIFIED: 640x480 PPMs,
black (faithful — movie stalled at fade-in from black); GXSetCopyClear confirmed firing (live
capture, not a default). ctest 21/21; non-dump boot reaches the gameLoop unchanged.
Full slice plan: **scratch/handoff_renderer_attach.md**. NEXT = SLICE 2 (immediate-mode 2D
capture: the GX vertex writers are inline macros to the FIFO sink in dolphin/gx/GXVert.h →
under SMS_NATIVE_PLATFORM make GXBegin/GXPosition3s16/GXColor1u32/GXEnd capture into a native
imm buffer; present converts via the 2D ortho) → then port the Hx_ wipe (now visible) → SLICE 3
J3D scene = first color frame.

## State banked
- FIX 1 committed+pushed (submodule fork `sunbright` 39977e7 + parent gitlink).
- ppcdis.py + SB_MOVIE_DBG committed+pushed (parent main).
- Full Hx_ wipe-library RE captured above (durable; the hard part is done).
- Renderer-attach SLICE 1 committed+pushed (a618d56).

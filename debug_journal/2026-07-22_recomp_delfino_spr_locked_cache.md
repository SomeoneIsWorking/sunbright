# Loading a save bounced to the title — per-thread SPRs broke the locked cache, and with it THP

2026-07-22. User report: "I tried loading a save file but it brought me back to the title screen"
(sms-recomp). Root cause found, fixed, verified: **Delfino Plaza now loads and renders in the
recomp** (`scratch/screenshots/recomp_delfino.png` — Mario, FLUDD, HUD, NPCs, palms, the
graffitied statue, dialogue).

## The chain, outermost to innermost

1. `TApplication::proc` (Application.cpp:757): when a director's setup fails it goes to
   `APP_STATE_DONE`, whose body is literally `mNextArea.set(15, 0, 0)` — stage 15 is the
   title/file-select scene. **"Back to the title screen" IS the stage-load failure path.**
2. `TMarDirector::direct` (MarDirectorDirect.cpp:100): `OSJoinThread(&gSetupThread, &local_40);
   if (local_40) return 4;` — the setup thread's exit value is `loadResource()`'s result, and
   nonzero means DONE.
3. `TMarDirector::loadResource` (MarDirectorLoadResource.cpp:122) ends with
   `if (mMap == 1) { int errc = thpInit(); if (errc) return errc; }`. **`mMap == 1` is Delfino
   Plaza and only Delfino Plaza** — which is exactly why the title and file-select were fine and
   loading a save was not.
4. `thpInit` opens `/data/ex128x144_q0.thp`; the plaza runs a persistent THP video
   (`currentStateFinalize` calls `THPPlayerPlay()` on four state transitions, each guarded by
   `mCurrArea.unk0 == 1`). Our `THPPlayerOpen` override declined every movie, so this failed.
5. Letting the real open run got further and then failed inside the codec.
   `THPVideoDecode` (THPDec.c:50) returns **28 = locked cache not enabled**:
   `if (!(PPCMfhid2() & 0x10000000)) goto _err_lc_not_enabled;`

## The actual defect: SPRs were per-thread

`THPPlayerInit` calls `LCEnable()`, whose `__LCEnable` asm sets HID2 bits `0x100F0000`. The codec
then runs on the **video decode thread**. SPRs lived in `CPUState::spr[]`, and every guest thread
has its own `CPUState` — so the decode thread read HID2 as 0 and correctly concluded the locked
cache was off.

HID0/HID1/HID2, L2CR, the BATs and friends describe the **machine**, not a thread. The GameCube
has one core and one set of SPRs. `CPUState::spr` is now a proxy over one shared array
(`CPUState::SprFile::s_spr`); the genuinely per-context registers (SRR0/1, GQR) stay per-CPUState,
which is also what the GC OS saves and restores per thread.

Measured after the fix: `THPVideoDecode -> 0`, `HID2 = 0xf00f0000`, plaza holds
`GAMEPLAY curr={1,5}` indefinitely, Mario at `(0, 300, 7400)` (a plaza coordinate; file-select is
`(950, 100, -1000)`).

**This class of bug is worth remembering: any machine-scope state modelled per-CPUState is
invisible across threads and shows up as an unrelated subsystem "not being ported".**

## Second defect found on the way: OSCancelThread was unmodelled

`OSCancelThread` (0x80348b4c) unlinks the target from scheduler queues, which only works because
retail's own sleep/resume maintained those links. This runtime blocks by token hand-off and never
writes them, so the real body walked a null queue pointer (write to 0x4) when the THP player tore
down its decode threads. Now overridden: `gsched_cancel` marks the thread dead so it is never
scheduled again and publishes MORIBUND to the guest struct, mirroring `gsched_exit`.

## Still open

Opening a SECOND THP session after one is cancelled faults with a null message queue
(`OSMessageQueue+0x1c`, from `PopReadedBuffer`). So THP **decoding** works, but session teardown
and reopen does not. Policy env, default `stage`:

- `SBR_THP=stage` (default) — only the stage-resident player opens. Delfino works; attract movies
  and cutscenes do not play (the game's own movie-setup-failure path, movies marked already-seen).
- `SBR_THP=all` — everything opens and plays, until the second session.
- `SBR_THP=none` — the old behaviour; Delfino Plaza cannot be entered.

The fix for `all` is the THP session lifecycle, NOT the codec — do not re-open "THP is unported".

## Also landed: fastboot is back

`sms-recomp/overrides/fastboot_native.cpp`, ported from the retired Dolphin-era
`runtime/overrides/fastboot_native.cpp` (git `9283f44^`) — same RE and addresses, adapted to this
runtime's override/memory API. `SBR_FASTBOOT=1` boots File 1 straight into Delfino Plaza with the
episode resolved from the save; `SBR_STAGE=<n> [SBR_SCENARIO=<n>]` forces a destination (naming a
stage implies fastboot). This is what let the whole bug be reproduced **deterministically with no
input at all** — the file-select head-butt was never involved.

Diagnostics added: `mario` channel logs Mario's position via the RE'd
`SMS_GetMarioPos` (`lwz r3,-0x60B4(r13)`, r13 = 0x804141C0, so 0x8040E10C holds a POINTER to the
Mario object and the TVec3 is at +0x00 — it is not a position global). The `app` channel now also
reports on AREA changes, not just `mAppState`, which is what made the one-frame bounce visible.

## Retracted

The "Mario's arms are missing/wrong at file-select" investigation is **withdrawn** — the user
reports arms render normally in the real window. That was measured off 320x240 headless dumps;
the "white sliver" was downsample aliasing. Do not re-open it from those dumps.

## Follow-up: Mario's arms in Delfino were a RECOMPILER bug, not a renderer bug

User: "Delfino plays fine but Mario has some visual oddities like the arm collapse". Captured and
zoomed (`scratch/screenshots/mario_crop.png`): rigid geometry (cap, overalls, shoes) correct, the
**arms absent** and FLUDD's parts sitting at wrong transforms. In a standalone recomp every line
of game code is the real thing, so wrong geometry has only two possible sources — the GX/matrix
seam, or codegen. It was codegen.

**`ps_sum1` summed the wrong operand.** Per the 750CL manual both paired-single sums take the
SAME addend and differ only in which half of frD receives it:

    ps_sum0: frD(ps0) = frA(ps0) + frB(ps1);  frD(ps1) = frC(ps1)
    ps_sum1: frD(ps0) = frC(ps0);             frD(ps1) = frA(ps0) + frB(ps1)

The emitter used frB(ps0) for ps_sum1, corrupting the second component of every dot product built
with it. J3D matrix concatenation is exactly that shape, which is why MOST transforms were right
and a few were not. Fixed -> arms and gloves render. Both cases also wrote one half of frD before
reading the other operands, so frD == frA destroyed the addend; now staged through temps.
6 ps_sum1 sites, 41 ps_sum0 sites.

**`dcbz_l` was decoded as ps_sum0.** Opcode 4 XO=1014 is dcbz_l (locked-cache dcbz), not a
paired-single op — the line even carried a `// actually 512+502?` comment. A memory clear was
being executed as a float add. 1 site.

**Lesson: in a recompiler, "some transforms are wrong and most are right" points at an
instruction emitter, not at the renderer.** A wrong emitter for a rarely-used opcode is invisible
until the one subsystem that uses it renders visibly wrong. Audit the emitter against the manual
before instrumenting the GX layer.

### Still open (visual, Delfino)
FLUDD's components still sit at wrong transforms and a yellow slab intersects Mario's head. Also
unexplained: a large gold/blue disc behind Mario (may be the plaza's ground mosaic seen at a
shallow angle — unverified) and green/magenta goo with light rays when walking. NEXT STEP: get
retail ground truth from the Dolphin fork (headless, `--fifo-record` / NoGUI per
[[dolphin-fork-headless-tools-2026-07-15]]) rather than reasoning from our own output — the two
runtimes share the aurora GX layer, so a per-draw diff localizes it. Do NOT assume the remaining
oddities are renderer bugs; the emitter audit above found two codegen defects in one sitting, and
the paired-single/quantized-load family is where to look next.

## 60fps interpolation restored (2026-07-23) — replay, not re-simulation

**Measured: 30.0 ticks/s, 60.1 presents/s in Delfino Plaza.** The game still simulates at 30 Hz;
only the render is decoupled.

The retired Dolphin-era implementation began by re-issuing the engine's draw perform-lists on the
in-between field and converged, after a long fight with crashes and double-stepped animation, on
REPLAYING the captured GX command stream instead. This runtime is already built that way —
`dev_gxfifo` collects a frame's commands and hands them to `aurora_fifo_replay` — so "render the
same frame again" is one call.

The part that makes the second render show something different is simpler here than in the Dolphin
era, and worth recording: **aurora reads indexed arrays through host pointers into guest RAM**
(`emit_arraybase`), so it re-reads the draw matrices at replay time. Blending the guest buffers
between the two replays is therefore sufficient. No aurora change, no second game frame, no game
code re-entered — which is what removes the entire crash class the old path fought.

Where the two ticks live (RE from the retired interp_capture.cpp, J3DModel::viewCalc @0x802deeb8):
viewCalc swaps the double buffer and recomputes each joint's draw matrix, so after a real field
`mDrawMtxBuf[0][view]` is tick N-1 and `[1][view]` is tick N — and `[1]` is what the shape packets
load. The in-between writes lerp(N-1, N, alpha) into `[1]`, replays, then restores N. **Restoring is
mandatory**: leaving blended values in place makes them the base for the next interpolation and the
scene drifts backwards a fraction of a tick every frame.

### Freshly-spawned models must be excluded — measured, not theorised
A model that first appears this field has no previous tick: `mDrawMtxBuf[0]` holds whatever the
allocator last left there. Before gating on "was this model live last field", peak joint delta was
**1.2e27** — a vertex flung across the screen. With the gate: 386 models rejected per run and the
peak drops to 8.3e10. A per-joint magnitude clamp is NOT the fix (the retired code removed one
because it misfired on distant geometry during camera rotation); the gate is about provenance.

### Still open
- **Cut detection.** A camera cut still interpolates across the discontinuity. The retired code
  detected cuts at the CAMERA level and skipped the whole in-between for that tick; the residual
  8.3e10 peak is probably exactly this. Do this before chasing per-model heuristics.
- **Screenspace effects** (the user's own warning, and the last blocker before retirement): the
  replay re-runs the frame's EFB-copy and screen-texture commands, so effects that sample the
  screen (water refraction, mirror, dash blur, mist) can sample the wrong half-step. The retired
  tree carried efb_interp_freeze.cpp / shadow_interp.cpp / cast_shadow_interp.cpp and a per-list
  mask for this. Treat those as evidence of WHICH passes misbehave, not as a finished answer.
- **Cost**: ticks/s drops ~30.0 -> 28 with interpolation on (the extra replay). Worth profiling
  before blaming the blend — replaying the whole stream re-parses it.

Live tuning without a rebuild: `curl '127.0.0.1:17654/interp60?alpha=0.5'` (or `on=0` to A/B).

# Engine port roadmap — RE + native ownership of everything the game uses

Standing directive (user, 2026-06-12): all engine code the game uses gets reverse-engineered
and ported. This file is the tracker; nothing is "done" by vibe — each subsystem moves
right-ward only with cited verification (oracle A/B, frame dumps, harness verdicts).

Two meanings of "ported", kept distinct:
- **Recompiled (baseline)**: every function already executes as native x86 via the static
  recompiler. This is the floor, not the goal.
- **Native-owned**: hand-written PC port of the subsystem's behavior (RE'd from the binary,
  decomp as map), parameterized for PC reality (aspect, host audio clock, host threads),
  guest body kept callable for A/B. The goal state for engine layers.

Function counts from reference/sms_gmse01_funcs.txt (9,680 named total; ~5,700 are
per-actor gameplay logic that stays recompiled until the engine layers are owned).

| Subsystem | ~funcs | Status | Evidence / notes |
|---|---|---|---|
| OS/threading/IRQ (Dolphin SDK) | 51+ | **Native-owned** | nthr scheduler, native IRQ dispatch, OSLoadContext handoff, native CARD/EXI |
| Audio: JAS synth/sequencer | 416 | **Native-owned (M1-M3)** | native_jas.cpp engine; BMS parser, voices, envelopes; harness-verified |
| Audio: JAI sound interface | 207 | **Partial** | SE/BGM intake, handles, 3D layer, lifecycle, port tees native; MISSING: per-scene category gating, priority stealing, fxmix/dolby buses |
| Audio: JAL / MSound game layer | 157 | **Partial** | MSHandle curves + category table ported; MSBgm slots ported; rest recompiled |
| HUD / J2D 2D pipeline | 83 | **Partial** | HUD widescreen-anchored natively; J2DScreen ortho still guest (640x480 hardcode documented in docs/decomp/j2d_fader_2d_pipeline.md) |
| Screenspace effects (fader/after-effect/EFB passes) | ~30 | **Partial** | fader, after-effect, EFB-to-texture, sun probes, mist replay owned for widescreen; inventory in docs/widescreen_effects.md |
| Water rendering (TSea/TWaterManager/TWater*) | 41+ | **PORTED — awaiting frame-dump verification** | RE in docs/decomp/water_rendering.md; port in runtime/overrides/water_native.cpp (screen-texture lookup matrices owned natively; default = guest-identical 4:3, SUNBRIGHT_WATER_WS=1 = true-aspect). water_widescreen.cpp draft superseded (tombstone). Verify: 4:3 pixel A/B → Delfino sea → FLUDD spray → mirror regression |
| J3D model/anim pipeline | 451 | Recompiled | render-port direction says own the object model eventually (docs/model_interpolation.md); after water |
| JPA particles | 320 | Recompiled | no screen-space emitters found (widescreen audit); port for interpolation/aspect later |
| TMap (stage/collision) | 497 | Recompiled | document first (docs/decomp/), port where PC seams demand |
| TMario / player | 290 | Recompiled | gameplay logic; lowest port priority by doctrine |
| TMarDirector / TApplication | ~60 | Documented | docs/decomp/mar_director_application.md; M4 gate: stage transitions block on MSound::checkWaveOnAram |
| THP movie player | 21 | Documented | docs/decomp/thp_player.md; M4 gate: THP audio via JASDriver mix callback; open NULL-deref hypothesis |
| Camera (CPolarSubCamera) | 76 | Recompiled | next documentation target |
| JKR heaps/archives | 232 | Recompiled | works under recomp; ROM data already decoded natively for audio |
| JUT / JSU / JGeometry / JMath | ~250 | Recompiled | utility layers; port opportunistically with their consumers |
| JDrama / JStudio (cutscene direction) | ~190 | Recompiled | document with camera |
| GX/VI/DVD/SI/EXI/PAD SDK | ~210 | Hybrid | GX via Dolphin GPU (by doctrine: keep), DVD FastDisc, CARD native, input override native, drawsync native |
| Boss/enemy/NPC/item actors | ~600 | Recompiled | stays recompiled until engine layers done |
| Remaining game-misc | ~5,700 | Recompiled | same |

## User-reported issue backlog (2026-06-12, Delfino Plaza gameplay) — all fix-by-RE+port
(Pre-existing issues, NOT fastboot regressions — user confirmed.)
1. **Stage-title banner not widescreen** — the dark band behind "DELFINO PLAZA" (stage-name
   card on stage entry) covers only the 4:3 region; left edge visible at 16:9. Same class as
   fillrect/fader widescreen. → identify the drawing element (SUNBRIGHT_2DID), widen natively.
2. **Audio: sounds not dying properly** — some SEs linger past their source (lifecycle/stop
   propagation in the native JAS layer?). Needs concrete repro + per-handle trace.
3. **Audio: drums in Delfino Plaza BGM that shouldn't play** — user: the percussion layer is
   the ride-Yoshi dynamic-BGM variant. SMS BGM mutes/unmutes per-track layers at runtime;
   suspicion: native BGM ignores the track-mute state (all tracks audible from start).
4. Potentially more audio issues behind those two.
4a. **FLUDD nozzle-change SE too loud** (user, 2026-06-12 pm) — distance attenuation not
    applying, or wrong base volume, when the water device changes shape.
4b. **Spray-spam silence — FIXED** (1b18a0c, user-verified): four stacked lifecycle bugs
    (missing worker stop-interrupt → permanent worker leak; wrong capacity/steal model;
    finished-instance revive replaying one-shots; stale stops killing living instances).
    SE worker protocol RE'd + documented in docs/re_notes/audio_re_findings.md. Next
    audio milestone: delete the worker indirection — direct per-sound snippet dispatch.
    (Delfino drums issue #3: FIXED cf8ad85 — BMS 0xE7 = syncCPU, ported callback.)
5. **60 fps model interpolation** — standing task (docs/model_interpolation.md).
6. **Map-screen freezes — root-caused + fixed (pending user verify)**: the freeze was
   synchronous Vulkan pipeline compilation on first-seen materials (map open / level entry)
   stalling the GPU thread; the CPU then hit the backpressure wait (vi-perf showed
   backpressure=2684ms windows). Fix: GFX_SHADER_COMPILATION_MODE = AsynchronousUberShaders
   (main_sdl.cpp). The backpressure wait itself STAYS: a 300 s A/B proved removing it
   trades freezes for VK_ERROR_OUT_OF_DEVICE_MEMORY at ~4 min (the wait incidentally
   bounds Dolphin's GPU-side resource growth; that leak is a separate open item).
7. **User directive:** if any of these trace to a Dolphin dependency, reduce that dependency
   (own the behavior natively) rather than working around it.

## PC-game architecture directive (user, 2026-06-12 — supersedes seam-patching)
"I want PC game mumbo jumbo, not emulation mumbo jumbo… I don't care if it will take months."
Stop guarding emulation seams; delete the emulation-era path and own the subsystem:
- **Audio M4 (STARTED 2026-06-12):** guest JAI/JAS must not run. Cuts so far: guest
  per-frame SE processor (checkNextFrameSe 0x80305204) and checkMonoSound (0x80017ddc)
  no-op'd, mono rule reimplemented at native dispatch.
  **WHY THE SPRAY KEPT BREAKING (3 regressions, root pattern):** the startSoundBasic tee
  still RUNS THE GUEST BODY "for bookkeeping" — that populates the guest SE registry with
  sounds that never finish (guest JAS is dead), and every guest reader of that registry
  (frame culls, mono checks, handle-state logic, whatever we haven't found yet) misfires
  and emits stops that the tee faithfully forwards. Whack-a-mole by design; we are blind.
  **VERIFICATION WARNING:** the synthetic spray driver (SUNBRIGHT_NJAS_TEST=spray) injects
  requests directly into the native engine, BYPASSING the guest game-code path — it passed
  during all three real-world spray regressions. It validates engine-internal lifecycle
  only. Real verification = the USER playing via ./run-dev.sh (hand them the command; do
  not launch headed runs yourself) + reading the tee events in the log.
  **THE DECISIVE CUT — native JAISound handle pool (THE next task, fresh context):**
  startSoundBasic stops calling the guest body for handled ids. Instead the override
  returns a handle from a NATIVE-owned pool of JAISound-shaped objects in guest memory
  (static arena; populate id @+0x8, actor/pos @+0x20, the fields actor code reads; all
  lifecycle native). Game actor code keeps working — it null-checks and calls
  setVolume/setPan/stop on the handle, and the existing tees key on handle->id. The guest
  registry is never populated → nothing stale to read → this whole bug class becomes
  unrepresentable. Then: guest audioproc/updateDac bookkeeping off entirely;
  worker-snippet indirection → direct per-sound dispatch; respect the two M4 gates
  (MSound::checkWaveOnAram stage-transition block, THP audio).
- **Graphics (months-scale arc):** native renderer direction — own the GX command stream →
  Vulkan with pregenerated game-tailored pipelines. Dolphin GPU semantics (FIFO
  backpressure, first-use shader compile, device-resource lifetime/OOM) cease to exist
  rather than being tuned. Interim state: backpressure wait removed, synchronous shaders +
  persistent cache + boot precompile.

## Order of battle (current)
1. Audio JAI frame layer (category gating, priority stealing) + fxmix/dolby — finishes the
   "sounds from everywhere" class; then audio M4 (guest path off; respect the two M4 gates above).
2. Water rendering native port (user priority; staged, 4:3 fidelity gate first).
3. J2DScreen ortho ownership (kills the 640x480 hardcode class for all 2D).
4. J3D object model + interpolation (render-port direction).
5. Camera + JDrama documentation → port.
6. JPA, TMap, then actor layers as the seams demand.

## Doctrine reminders
- Binary is ground truth; decomps are maps. Oracle for behavior; harness/frame-metrics for verdicts.
- Guest body stays callable (super-call) for every owned subsystem — A/B forever.
- Every stage commits with its verification cited; falsified theories get written down
  (docs/re_notes/) so they stay dead.

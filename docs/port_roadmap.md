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
| Water rendering (TSea/TWaterManager/TWater*) | 41+ | **IN PROGRESS — next** | user priority; staged plan: RE → docs/decomp/water_rendering.md → native draw matching guest at 4:3 → aspect-true 16:9. Draft notes in runtime/overrides/water_widescreen.cpp (to be superseded) |
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

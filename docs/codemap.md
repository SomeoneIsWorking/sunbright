# Codemap

The single-page answer to **which subsystem owns each responsibility and where it lives.**
Update the relevant row in the SAME commit that changes a subsystem, then run
`python3 ~/.claude/skills/codemap/codemap.py check --map docs/codemap.md sms-recomp sms-boot`.
(Architecture rules live in CLAUDE.md; findings in debug_journal/; this is orientation only.)

Legend: ✅ done (verified on real data) · 🟡 partial (documented gap) · 🔬 built/parsed but not wired · ⬜ missing.

## Renderer/runtime matrix

| Runtime | Aurora GX renderer | SDL3-GPU GX compatibility renderer | PC-native semantic renderer |
|---|---|---|---|
| recomp | ✅ Default displayed path: parsed FIFO → Aurora → present | 🟡 Explicit `run-render.sh` diagnostic path: a project-owned GX implementation consumes the same parsed FIFO while Aurora renders offscreen as its reference | 🟡 Audit/preview renders rigid unlit single-texture J3D models plus ordered pane/immediate pictures, resource-font glyphs, complete J2D windows, and filled rectangles; `./run.sh --semantic-preview` gives that incomplete target sole ownership of the live window while Aurora stays offscreen |
| decomp | ✅ Existing `sms-boot` oracle and moddable runtime | — Not a product goal | 🟡 The same audit and explicit visible-preview modes consume native-layout rigid unlit single-texture J3D models plus the picture, glyph, J2D window, filled-box, and GC2D fill stream; broader 3D materials and effects remain absent |

The SDL3-GPU FIFO path was formerly mislabeled Native. It is now classified as GX compatibility
tooling and does not satisfy G003 or G004. `run.sh --diagnostic` owns the recomp/decomp Aurora
smokes; `run-render.sh` currently owns the explicit recomp GX-compatibility diagnostic.

## Two runtimes (CLAUDE.md 🏛️ TWO RUNTIMES, 2026-07-21)

1. **recomp + native overrides** (`sms-recomp/`, `run.sh` → `tools/launch/run.py` → `run-recomp.sh`) — the game's real PPC statically
   recompiled, native C++ overrides only at HW/OS seams. **The primary active runtime.** Boots into
   Delfino Plaza and renders it; title + file-select render; 16:9 widescreen; 60fps interpolation.
   **ALL 24 STAGES BOOT AND RENDER** (1-15, 20-28; verified 2026-08-11 by sweep, 400 presents each
   with interpolation on, every one exit 0). Three crashes were fixed to get there and each had a
   real cause rather than a workaround: the arena was published over the game's own main stack so
   the heap grew onto it (issue #1), aurora's index staging region was a third the size GX geometry
   needs (issue #3), and interpolation nulled render-pass state the render worker was still encoding
   (issue #2). Only stages reached by `SBR_STAGE` are covered — this is a statement about what has
   been RUN, not about the whole game.
2. **decomp + Aurora** (`decomp/sms` + `extern/aurora`, `run-decomp.sh`) — the hand-decompiled game on the
   native platform. The moddable end-state and the **verification oracle** (renders title/file-select
   /Delfino correctly, so recomp output diffs against it). Crashes partway into Delfino gameplay
   (plaza-population stubs) — see its game-flow row.

## Game flow

| Stage | recomp | decomp/aurora (oracle) | Notes |
|---|---|---|---|
| GC logo → title (stage 15) | ✅ renders | 🟡 renders (cosmetic residuals) | recomp: `SBR_STAGE=15` / press START |
| File-select / save screen | ✅ renders | 🟢 at parity incl. Mario | recomp file-select correct; the mip/has_mips fix closed the "sea wash" |
| Delfino Plaza (stage 1) | ✅ **renders + playable** | ⬜ crashes into gameplay | recomp: Mario/FLUDD/HUD/NPCs/statue/dialogue; heat haze + water refraction render; `SBR_FASTBOOT=1` |
| Other stages | 🟡 reachable via `SBR_STAGE=<n>` | — | Gelato Beach verified wide; per-stage fidelity unaudited |
| Audio | ✅ **audible** (music + SFX) | ✅ **audible** (title BGM) | decomp: `sms-boot/runtime/jas_kernel_native.cpp`, verified 2026-07-17. recomp: `sms-recomp/runtime/devices/dsp_mixer.cpp` + the DSP frame/sub-frame interrupts in `dev_aid.cpp`, verified 2026-08-07. Both: v1 L/R only. `docs/audio/recomp_plan.md` |
| Movies (THP) | 🟡 decodes, no reopen | 🟡 | recomp: `SBR_THP=stage` default; second session faults (null msg queue) |

## sms-recomp/ — the recomp runtime (primary)

### Recompiler (`tools/recompiler/`)
| Subsystem | Status | Where | Gap / next |
|---|---|---|---|
| PPC decoder | ✅ | `tools/recompiler/ppc_decoder.cpp` | reachable opcode set complete; only data-as-code "unknown" remains |
| C emitter | ✅ | `tools/recompiler/c_emitter.cpp` | audited vs 750CL: paired-single, sraw/divw/lwarx/stwcx. Direct `bl`/out-of-function `b` sites cache their final override-aware target; only true indirect transfers use general dispatch |
| function discovery | ✅ | `tools/recompiler/function_discovery.{h,cpp}` | narrow discovery interface collects direct-call targets across text-section boundaries and clips function starts to the owning section; focused tests exercise both cases |
| generated code | ✅ | `sms-recomp/generated/` | 2.79M lines, machine-generated by the recompiler; never hand-edited |
| host entry | ✅ | `sms-recomp/host/main.cpp`, `sms-recomp/host/render_composition.{h,cpp}`, `sms-recomp/host/aurora_config.{h,cpp}`, `sms-recomp/host/dol_loader.{h,cpp}` | `main.cpp` composes settings, hidden UI runtime, Aurora, render clients, disc, memory, and guest execution; the game is the unconditional startup surface. `render_composition` owns the mutually exclusive GX-compatibility and semantic audit/preview policies, pauses Aurora pipeline compilation only around SDL device creation, and unwinds semantic collection, the active Aurora frame, UI, renderer resources, then Aurora. `aurora_config` owns Aurora launch policy; `dol_loader` owns DOL parsing/install and arena-low derivation |
| host settings + RmlUi | 🟡 | `sms-recomp/app/`, `sms-recomp/ui/`, `res/rml/`; behavior map: `docs/app/settings.md` | Dusklight ownership split: `app/` owns typed persisted policy; `Document → Window → SettingsMenu` owns presentation; `ui::Runtime` starts hidden, routes Escape, and blocks at the frame seam while the modal is open. The copied Dusklight window/tab/pane presentation uses its Fira Sans family and CC0 notice. Headless control pushes Escape through SDL, exercises the modal loop, validates 8/8 nonzero controls, and closes through Escape. All five framerate choices are live; GX Compatibility selection owns visible SDL-GPU presentation while Aurora remains its offscreen oracle |
| collection (linear/CFG) | ✅ | `tools/recompiler/func_collect.cpp` | `kForceCFG` in `main.cpp` for draw/thread entries |
| unit tests | ✅ | `tools/recompiler/tests/recomp_test.cpp` | `sunbright-recomp-test` (ctest); per-opcode operand asserts, reintro-validated |

### Runtime substrate (`sms-recomp/runtime/`)
| Subsystem | Status | Where | Gap / next |
|---|---|---|---|
| core dispatch / SPRs / call_ppc | ✅ | `sms-recomp/runtime/rt_core.cpp`, `guest_address_table.h` | SPRs are MACHINE-wide (not per-CPUState) — the locked-cache/THP fix. Recompiled functions and native overrides share a tested sparse direct-address table. Compile-time direct-target caching reduced general resolution from 3.96% to 1.22% of profile samples (84,148 direct sites; 7,409 indirect sites) |
| memory + MMIO | ✅ | `sms-recomp/runtime/devices/mmio.cpp`, `intrinsics.h` | flat MEM1 fast path + locked-cache @0xE0000000; the authoritative range router retains a stable per-thread last-device cache for the write-gather hot path. The dead Dolphin-era read-poll logger no longer mutates globals on every 8/16/32-bit RAM read |
| HW devices | ✅ | `sms-recomp/runtime/devices/` (gxfifo, vi, ai, si, exi, dsp, aram, mi, pi, di, sram) | `dev_gxfifo.cpp` frames GX and hands it to Aurora. `gx_fifo_contracts.hpp` is the checked producer boundary: legal zero-byte display lists classify as empty operations, while nonempty display-list ranges/nesting/exact consumption, full texture mip and TLUT spans, unknown commands, incomplete tails, and explicit empty-frame rotation all fail at their source. Array commands carry upload extent separately from backing capacity, so Aurora can auto-size an upload while proving the derived read stays in MEM1. `gx_fifo_input.{h,cpp}` owns write-gather reassembly and `gx_fifo_vertex_layout.{h,cpp}` owns VCD/VAT sizing (including NBT3), both with negative controls. `gx_fifo_2d.{h,cpp}` owns orthographic-draw decoding and completeness telemetry |
| guest scheduler | ✅ | `sms-recomp/runtime/guest_sched.cpp` | host thread per guest thread, single token; `gsched_cancel` (OSCancelThread) |
| disc (nod) | ✅ | `sms-recomp/runtime/devices/disc.cpp` | serves the disc directly, no DVD worker |
| boot environment (apploader) | ✅ | `sms-recomp/runtime/boot_env.cpp` (+ guard `sms-recomp/overrides/guard_arena.cpp`) | publishes disc ID, FST, arena HI. Arena LO is published as **0 on purpose** so OSInit uses the DOL's own `__ArenaLo`, which sits above the game's stacks — publishing the end of the DOL image instead put the heap on top of the main stack (issue #1, fixed 2026-08-11). The guard aborts on any arena lo at/below the caller's live SP |
| probe server | ✅ | `sms-recomp/runtime/probe_server.cpp` | `SBR_PROBE=1`, frame-seam dispatch; `/r /w /help` + module endpoints |
| GPU incident recorder + report | ✅ | capture/parser `sms-recomp/runtime/gpu_incident_recorder.{h,cpp}`, allocation-free formatter `gpu_incident_report.{h,cpp}`, reader `sms-recomp/tools/gpu_flight_dump_main.cpp`; Aurora schema `extern/aurora/include/aurora/aurora.h`, selected pass-stream probe `extern/aurora/lib/gfx/gpu_submit_probe.{hpp,cpp}`, replay source/writer contract `replay_lineage.{hpp,cpp}`, pass-owned marker storage `debug_markers.{hpp,cpp}`; external watcher/device-coredump owner `tools/render/gpu_watch.py`, `gpu_events.py` | armed before Aurora init; fixed pwrite ring plus synchronized `.report.txt` sidecar survive device loss and Dawn uncaptured errors. V2 records aggregate pass/resource/cache state, coherent readback-callback lifetime, and the last nine semantic GX/Rml/Clear gfx-pass draw fingerprints/ranges. V3 adds explicit untouched replay-source frame/command/uniform lineage; the worker re-observes installed source bytes before encode, then re-hashes the final selected command stream and expected uniform prefix before unmap/submit and checks writer epochs for the source vertex/index/storage prefixes. Selected command hashing covers pass metadata, marker bytes, palette conversions, recorded commands, and resolves including resource generations/path/sample count; it explicitly excludes standalone texture-copy FrameOps, attachment load/store/clear values, stencil clear, buffer bytes, and later readback-copy/present/ImGui/profiler commands. `--kernel-real-ns` selects only submits outstanding at the first kernel event; `--submit` inspects one retained submit and its eventual callback without rewriting the event-time window. Only a successful Dawn `OnSubmittedWorkDone` callback becomes a completed baseline; missing/error/cancelled callbacks do not prove successful completion. Uncaptured errors preserve a bounded Dawn type/message before Aurora aborts and label the latest submit as temporal context, not cause. After immediate process-group kill, the watcher records a bounded, PCI-correlated Linux device-coredump disposition and preserves readable bytes without writing sysfs. Controls: real 2026-08-26 submit-1608 known-positive, pre/between/post submit timing, callback status, v1/v2/v3 payloads and zero-lineage v3, replay mutation/writer controls, marker lifetime, every-field hash sensitivity, fork-abort, corrupt/torn/wrap/stale/bounds rejection, chronological tail, destination-alpha sentinel, concurrent readback snapshot invariants, and captured/unavailable/stale/unrelated coredump outcomes (I033) |
| probe: settings menu | ✅ | `sms-recomp/overrides/ui_probe.cpp` | `/ui` pushes a real SDL Escape so an automated run can open the RmlUi menu over a live game. One-way: the game pauses inside `pause_while_open` and never reaches the seam, so the probe cannot close it |
| J3D geometry decode | 🟡 | shared decoder `native-render/{include/sunbright/native_render,src}/j3d_mesh_decode.*`; recomp guest adapter `sms-recomp/runtime/render/j3d_decode.*`; decomp native adapter `sms-boot/runtime/native_j3d_adapter.cpp` | One primitive/layout meaning for both runtimes; display commands stay big-endian while array byte order is explicit. Covers POS/NRM/CLR0 + four texcoord sets. Gaps: TEX4-7 sets and per-vertex TEXnMTXIDX |
| GX compatibility scene | 🟡 | `sms-recomp/runtime/render/scene.cpp`, `scene_geometry.{h,cpp}`, `gx_light.{h,cpp}`, `gx_texgen.{h,cpp}` | Owns geometry reconstruction and interpolation for the SDL3-GPU GX reference path. It may supply evidence and reusable mesh decoding, but its GX material/state model is not the PC-native renderer interface. |
| SDL3-GPU GX compatibility renderer | 🟡 | GX renderer client: `sms-recomp/runtime/render/native_render.cpp`, `native_gpu_guard.{h,cpp}`, `native_gpu_admission.{h,cpp}`, `native_gpu_pipeline.{h,cpp}`, `native_raster_state.h`, `native_efb_copy_plan.{h,cpp}`, `native_efb_copy_clear_draw.{h,cpp}`, `native_tev_uniform.{h,cpp}`, shaders, `scene.cpp`, `state_oracle.cpp`; shared GPU host owner: `native-render/src/sdl_gpu_{platform,presenter,frame_target,calls}.cpp` | Owns the diagnostic FIFO→GX-state→SDL3-GPU implementation, exact Aurora comparison, and its safety boundary. It consumes the shared platform with its own linear target; it does not own a second device or window presenter. The legacy selector is `SBR_RENDERER=native`; UI and documentation call it GX Compatibility. Do not add new PC-native product semantics here. |
| PC-native semantic renderer | 🟡 | public schema/API and shared model/material/image/J3D decoders plus `model_context.*`: `native-render/include/`, `native-render/src/`; ordered `SemanticDraw` frame/sink, 2D implementations, `semantic_3d_pass.cpp`, shared `sdl_image_cache.cpp`, and `sdl_semantic_frame_client.cpp`: `native-render/src/`; PC shaders and controls: `native-render/shaders/`, `native-render/tests/`; recomp J3D model/material/guest-camera adapters: `sms-recomp/overrides/{semantic_j3d_*,j3d_scene_projection_adapter.*,guest_j3d_texture_adapter.*}` with the sole shared `TViewObj::testPerform` hook composed in `sms-recomp/frame_interp/subframe_legacy.cpp`; recomp J2D adapters remain under `sms-recomp/overrides/`; decomp J3D adapters: `sms-boot/runtime/native_j3d_*` plus the high-level dispatch hook in `decomp/sms/src/JSystem/JDrama/JDRViewObj.cpp`; decomp J2D adapters: `sms-boot/runtime/native_{jut_texture,picture,window,resource_font,j2d_fill_box,solid_rectangle}_adapter.cpp`; all decomp semantic adapters are owned as the `sms-native-semantic` library; composition/frame seams remain in each runtime | `semantic_sink` owns the exclusive producer lease and `semantic_frame_bridge` owns begin/seal lifecycle. `Semantic3dPass` consumes only triangle meshes, matrices, decoded RGBA images, and the first exact unlit material family; it depth-tests before `Semantic2dPass` overlays the ordered UI. One bounded immutable-revision image cache implementation serves both passes. Runtime adapters feed copied values only and retain both original bodies. Camera projection is copied from high-level `TGraphics` draw dispatches, classified as perspective/orthographic, and nested through a fixed-capacity renderer-neutral scope; pre-camera traversals explicitly mask outer state. The material boundary carries high-level cull, depth, alpha-cutout, and blend values; the GPU pass supports the exact common opaque, texture-edge, and translucent policy families and refuses other full policies. Neither model adapter consults GX SDK/FIFO/compatibility state. Broader materials, lighting, skinning, particles, and effects remain next. None belong in the GX compatibility renderer. |
| 2D / HUD overrides | 🟡 | `sms-recomp/overrides/diag_2d.cpp`, `j2d_picture_adapter.{h,cpp}`, `j2d_window_adapter.{h,cpp}`, `semantic_j2d_window.{h,cpp}`, `sms-recomp/overrides/hud.cpp` | `diag_2d.cpp` remains the sole `0x802cc7c0` registration owner and publishes semantic state at entry before always super-calling the recompiled body. Its older post-body `SBR_J2D_CAPTURE` path belongs only to GX compatibility. `hud.cpp` owns current widescreen corner layout and publishes the adjusted window geometry immediately before the retained `J2DWindow::draw_private` body. |
| GX TEV reference + tests | ✅ | `sms-recomp/runtime/render/tev_eval.{h,cpp}`, `sms-recomp/tests/tev_eval_test.cpp` | the TEV pipeline as testable C++ and the DEFINITION of it — the shader mirrors it. Expectations hand-derived from the SDK (`GXSetTevColorOp` packing, `GXTevOp`, konst ramp, `GXCompare`); negative-control verified. `SBR_TEV_TRACE=<tick>` + `SBR_TEV_TRACE_BLACK=1` explain one drawable's pixel stage by stage |
| GX lighting reference + tests | ✅ | `sms-recomp/runtime/render/gx_light.{h,cpp}`, `sms-recomp/tests/gx_light_test.cpp` | the scene calls the shared evaluator for both GX colour channels; CPU controls cover attnFn decode, diffuse modes, distance/angle attenuation, accumulator clamp-before-material multiplication, independent alpha control, and dead-light NaN handling |
| per-draw state oracle | 🔬 | `sms-recomp/runtime/render/state_oracle.{h,cpp}` + weak hooks in `extern/aurora/lib/gx/command_processor.cpp` | Owns parsed-state comparison at a shared draw key. The target join key is `(frame identity, FIFO stream offset)`; frame identity must travel through both producers before the instrument is trusted again (I003). |
| render A/B harness | 🔬 | `sms-recomp/runtime/render/render_compare.{h,cpp}`, `render_compare_join.{h,cpp}` + Aurora frame-sink ABI/readback job in `extern/aurora/include/aurora/aurora.h`, `extern/aurora/lib/frame_sink_schedule.hpp`, and `extern/aurora/lib/aurora.cpp` | Owns exact-frame GX-compatibility/Aurora scoring and controlled operation attribution. Aurora's capture-time frame identity belongs in the sink job and callback; `render_compare_join` owns the bounded exact-ID rendezvous for compatibility baselines, variants, and delayed oracle pixels. Unknown, duplicate, or missing identities and a failed no-op control must suppress verdicts (I008). Exact pairing validates each row's delta, but the current round-robin samples different scene frames per operation, so cross-row ranking remains exploratory. This instrument does not score the PC-native semantic renderer. |
| screen-effect registry | ✅ | `sms-recomp/frame_interp/effects.h`, `effects_screen.cpp`, `effects_afterimage.cpp` | names the per-frame screen-sampling set; `/screenfx`; for the interpolated in-between frame |
| SB_LOG channels | ✅ | `sms-recomp/runtime/sb_log.cpp` | linked into the executable (weak-undef trap) |

### Native overrides (`sms-recomp/overrides/`)
| Subsystem | Status | Where | Gap / next |
|---|---|---|---|
| frame seam / present | ✅ | `sms-recomp/overrides/native_frame.{h,cpp}` | present + pace + SIGTERM/window-close + scene tick/render + `sbr_audio_frame`. The first and every later `aurora_begin_frame` result is authoritative: guest execution waits at the seam while WSI is unavailable, and FIFO replay is impossible without an active Aurora frame packet. Holds the GPU-submission ceiling (`SB_MAX_PRESENT_HZ`, default 120): `SB_TURBO` stops pacing the GAME, and used to also remove the only bound on how fast frames reach the driver — thousands a second with no gap for the compositor. A sleep, never a skipped shipping present |
| GX seam | ✅ | `sms-recomp/overrides/native_gx.cpp`, `sms-recomp/runtime/devices/dev_gxfifo.cpp` | GXWaitDrawDone/DrawDone; stream handed to aurora |
| CARD | ✅ | `sms-recomp/overrides/native_card.cpp` | host Dolphin card image, inline |
| DVD / ARQ / THP | 🟡 | `sms-recomp/overrides/native_dvd.cpp`, `native_arq.cpp`, `native_thp.cpp` | THP decodes; session reopen faults (`SBR_THP`) |
| PAD | ✅ | `sms-recomp/overrides/native_pad.cpp`, `native_pad_policy.{h,cpp}` | keyboard 12/12 bound (calls aurora PADInit); `SBR_PAD_SCRIPT`; the pure policy owns script-clock parsing/key selection. Deterministic cross-mode captures use `SBR_PAD_SCRIPT_ONLY=1` plus `SBR_PAD_SCRIPT_CLOCK=guest-retrace`, so live input cannot contaminate a capture and the same script key means the same guest time |
| OS threads / MMU | ✅ | `sms-recomp/overrides/native_os_thread.cpp`, `native_os_mmu.cpp` | token hand-off; OSCancelThread |
| AID audio-DMA engine | ✅ | `sms-recomp/runtime/devices/dev_aid.cpp` | 0xCC005030-3C its own device (was swallowed by dev_aram as inert halfwords — the dead link). Latch/wrap/re-arm, `__AID_Callback` delivered 57/s = 32000/560, paced to a 100 ms host backlog. Verified 2026-07-23 |
| DSP voice mixer | ✅ | `sms-recomp/runtime/devices/dsp_mixer.cpp` | native voice renderer reading GUEST VPBs (AFC + PCM8/16, linear resample, L/R bus mix). The DSP's frame AND sub-frame interrupts are supplied by `dev_aid.cpp` — gSubFrames=7 per frame, and supplying only the frame one gives 1/7 tempo. Music + SFX audible, verified 2026-08-07; v1 is centre-panned, no aux/filters/Dolby. `docs/audio/recomp_plan.md` |
| fastboot | ✅ | `sms-recomp/overrides/fastboot_native.cpp` | `SBR_FASTBOOT`/`SBR_STAGE`/`SBR_SCENARIO`; ported from git 9283f44^ |
| widescreen (16:9) | ✅ | `sms-recomp/overrides/widescreen.cpp` | aspect widened at `C_MTXPerspective` (input, not output); `SBR_WIDESCREEN` |
| widescreen HUD | ✅ | `sms-recomp/overrides/hud.cpp`, `hud_window_layout.{h,cpp}` | per-`.blo`-name edge anchoring; announcement `te_w` uses one centered frame/content transform. `TGCConsole2::perform` draws `tet1`/`tet2` directly under `unk544`, so the text scissor is derived from the frame's post-projection EFB interval rather than a J2D child clip or a duplicated pillar offset. Pure layout tests and 1280x960 captures verify the continuous band and complete text; `/2d` |
| widescreen effects | ✅ | `sms-recomp/overrides/widescreen_effects.cpp` | 2D full-screen widen + EFB-tex/mirror suspend; `/wsfx /fills` |
| **frame cadence / interpolation** | 🟡 | **`sms-recomp/app/frame_rate.{h,cpp}`** owns user policy; **`sms-recomp/frame_interp/`** owns interpolation; `presentation_label.{h,cpp}` owns main/sub dump roles keyed to the shared guest tick, `subframe_pose.{hpp,cpp}` owns CPolarSubCamera view synthesis/restoration and TMario's `calcAnim(2)` seam, while `subframe_guest.hpp` owns shared big-endian guest scalar/name access. Aurora **`extern/aurora/lib/gfx/common.cpp`** and **`indexed_interp.{hpp,cpp}`** own retained replay samples; `extern/aurora/lib/aurora.cpp` snapshots each dump label before asynchronous encoding. Map: **`docs/60fps/README.md`**, UI contract: **`docs/app/settings.md`** | All five modes apply at the next tick. Native modes apply BSE's retrace, SMS animation-rate, and ModelGate timing contract before the retail `JDrama::TVideo::waitForRetrace` body. Optimization is selected from internal work and sampling, never wall-clock frame averages: a settled plaza frame currently carries ~30.4k auto-sized primitives / 169k scanned vertices / 506k field visits / 1.01 MB index bytes, then merges to ~1.42k Aurora draws. The exact scan is specialized by cached indexed-layout shape; the write-gather path no longer performs one generic vector insertion for each of ~123k guest stores. The next structural target is repeated state/cache hashing and the duplicated root/Aurora command parse, not dormant diagnostics (that hypothesis was falsified). Interpolated modes retain 30 Hz logic and resample the retained draw plan. Direct vertices, matrices, billboards, camera, and indexed XYZ-f32 arrays are covered. Dynamic TDL batches are paired per four-vertex member across births/deaths using retail owner identities and the exact `reset → request → draw` lifecycle; the live FLUDD control reported zero unkeyed arrays, zero layout mismatches, and 100% interpolation for continuously visible TDL arrays. `SBR_DISPLAY_HZ` is the windowless rate input |
| BetterSunshineEngine FPS compatibility | 🟡 | `sms-recomp/bse/frame_rate_fixes.cpp`, `frame_rate_logic.{h,cpp}`; source contract pinned in `debug_journal/2026-08-21_bse_native_frame_rate.md` | Base timing plus boid, AnimalBird/Boss Eel, TJointCoin/Sand Bird, textbox, and HX motion behavior are runtime overrides with retail super-calls. CPU tests exercise the shipping HX formula. Remaining: BSE's discrete mid-function HX timer/frame-rate initializers need a proper static-recompiler patchpoint mechanism or full owning-function ports; no value-based timer hack is used |
| 2D-class diagnostics | 🔬 | `sms-recomp/overrides/diag_2d.cpp` | `/2dclass` (SBR_DIAG_2D=1); pane→class census |

## Aurora (`extern/aurora` — the GC platform surface; submodule, fork remote branch `sunbright`)

Shared by both runtimes: the recomp hands it a GX stream, the decomp calls its GX/DVD/CARD/VI/audio.

| Subsystem | Status | Where | Gap / next |
|---|---|---|---|
| GX command processor | 🟡 | `extern/aurora/lib/gx/command_processor.cpp`, `auto_array_sizing.hpp`, `extern/aurora/lib/gx/fifo.cpp`; replay encode contracts `extern/aurora/lib/gfx/replay_draw_validation.{hpp,cpp}` | Replay + native emission. The extension decoder rejects unknown subcommands, indexed-draw count/index overruns, invalid XF channel/texgen counts, and indexed-XF reads without a proven array capacity. Frame finish rejects orphan one-shot state. Array commands carry both upload extent and backing capacity; auto-array maximum-index scanning may derive the former but cannot exceed the latter. Before Dawn encoding, replay validation checks exact operation high-water/capacity windows plus GX/RML vertex, index, uniform, alignment, count/byte, replay-prefix, interpolation spans, all indexed-array byte extents/used slots, and RmlUi dynamic binding extents (issue 17) |
| render-worker queue ownership | ✅ | `extern/aurora/lib/gfx/render_worker.{hpp,cpp}`, `persistent_upload.{hpp,cpp}`, queue sinks in `common.cpp` | Dawn queue submission and persistent indexed-array `WriteBuffer` calls share one typed FIFO. Producer bytes are copied before enqueue; worker ownership is asserted at the write sink, preserving older submit → upload → current submit without a GPU wait (issue 16) |
| GPU diagnostics policy | ✅ | `extern/aurora/CMakeLists.txt`, `extern/aurora/cmake/aurora_core.cmake`, `extern/aurora/lib/webgpu/gpu.cpp`, debug-group snapshot owner `extern/aurora/lib/gfx/debug_group_snapshots.hpp` + `extern/aurora/lib/gfx/common.cpp` | `off/standard/full` selects backend/API validation, robustness, and debug groups independently of `NDEBUG`. Standard is the Sunbright default. Recorded commands hold a 32-bit group-stack snapshot ID; strings are interned once per push/pop revision rather than copied into every command. Startup logs the effective request, including the pinned Dawn build's partial-backend-validation limitation. |
| GX state→wgpu | 🟡 | `extern/aurora/lib/gx/gx.cpp`, `shader.cpp` | WGSL gen |
| EFB copies / XFB present | 🟡 | `extern/aurora/lib/gx/`, `extern/aurora/lib/gfx/common.cpp`, `clear.cpp`, `extern/aurora/lib/aurora.cpp`, `extern/aurora/lib/window.cpp`, `extern/aurora/lib/webgpu/gpu.cpp` | Present ✅. Surface availability is atomic across the render worker/main-thread WSI boundary; a lost/minimized surface is invalidated during present and only synchronized/released by the main thread after current texture/view lifetimes end. Zero-size windows never enter aspect/swapchain math, and guest execution pauses at the recomp frame seam until a new Aurora frame packet can be opened. WebGPU initialization is transactional across backend attempts; shutdown drains bounded staging/depth-peek callbacks before device destruction. Partial `GXCopyTex(clear=true)` and the 7-tap vfilter remain fidelity work |
| windowless host path | ✅ | `tools/launch/sdl_video.sh`, `extern/aurora/lib/webgpu/gpu.cpp` | `SB_HEADLESS` selects SDL offscreen and a surfaceless WebGPU adapter; offscreen render targets remain active without X11/Wayland/WSI |
| texture cache | 🟡 | `extern/aurora/lib/gfx/texture*` | (texObjId, version) keyed; `has_mips` derives from TexMode0 min-filter |
| dolphin SDK layer | 🟡 | `extern/aurora/lib/dolphin/` | dvd sync ✅; pad defaults ✅; CARD host-alloc gating open |

## decomp + Aurora runtime (`sms-boot/` — the oracle)

| Subsystem | Status | Where | Gap / next |
|---|---|---|---|
| main / boot | ✅ | `sms-boot/main.cpp` | one-runtime, single thread |
| frame seam / present | ✅ | `sms-boot/runtime/frame_seam.cpp`, `sms-boot/runtime/semantic_render.{h,cpp}` | `sb_frame_present` in TVideo::waitForRetrace; semantic composition consumes every sealed frame while the host-allocation gate is active. Audit mode owns device-only setup/teardown around Aurora; preview mode disables Aurora presentation and claims its window through the shared SDL GPU presenter. The seam also owns the strict decomp present limit and the same `SB_MAX_PRESENT_HZ` submission ceiling as recomp |
| FIFO replay harness | 🟡 | `sms-boot/runtime/fifo_player.cpp` | CI-format TLUT synthesis missing (fail-fast) |
| audio pump | ✅ | `sms-boot/runtime/audio_out.cpp`, `sms-boot/runtime/jas_kernel_native.cpp` | native voice renderer; title BGM audible + WAV-verified (2026-07-17) |
| SDK stubs | 🟡 | `sms-boot/runtime/sdk_stubs.cpp` | audited; every stub documented or loud |
| decomp shims/stubs | 🟡 | `sms-boot/shims/`, `sms-boot/boot_stubs/`, `sms-boot/assets/` | each boot_stub = porting worklist; the five matrix-effect stubs displaced by the upstream rebase are removed because `MtxUtil.cpp` now owns them. Native decomp compilation stages ordinary literals as Shift-JIS octal bytes (`tools/build/encode_sjis_literals.py`) while keeping authoritative sources UTF-8. SPC reload uses structural endian detection rather than remembered pointer identity, so a fresh BE blob at a reused address is swapped again |

## Reference decomp (`decomp/sms` — submodule, SomeoneIsWorking/sms fork)

The real game source, native-platform-guarded (`SMS_NATIVE_PLATFORM`). Current fork tip
`9e7a2105` is rebased onto the 2026-08-30 upstream tip and audited with
`tools/re/rebase_upstream.py`; its Clang build and guarded decomp runtime smoke test are green. Its
standing loop is **rebase → rename known `unk*` semantics → expand remaining gaps**. The restored upstream
`MtxUtil` implementation keeps the native-safe 4×4-to-3×4 light-projection adaptation, and the typed
`MActorAnmData` accessors replace five known `getUnk*` names. Matching-MWCC proof remains externally
blocked on the absent Japanese Rev-0 disc. Rendering-affecting code is always native. Screen effects
(heat haze, water refraction, dash blur, TScreenTexture) are FULLY implemented — see
`docs/60fps/screen_effects.md`.

## Tools (`tools/`)

| Tool | Purpose |
|---|---|
| `recompiler/` | the static recompiler (`sunbright-recomp` DOL→C++); `sunbright-recomp-test` |
| `tools/re/rebase_upstream.py` | upstream doldecomp/sms sync (status→rebase→audit→converge) |
| `cmake/SunbrightBuildPolicy.cmake`, `tools/build/profile_check.py` | one optimized-Debug policy shared by both runtimes; the checker reads CMake's emitted commands and refuses missing optimization, symbols, assertions, or Aurora diagnostics |
| `tools/build/encode_sjis_literals.py` | lexically transforms only non-ASCII ordinary C/C++ literal content into fixed-width Shift-JIS octal escapes for Clang native builds; comments and identifiers remain UTF-8 |
| `tools/re/port_dossier.py`, `tools/re/vtable_re.py` | per-function RE dossiers; weak-vtable slot resolution |
| `tools/re/gap_worklist.py` | hand-port gap tracker (`docs/port/worklist.md`) |
| `tools/re/ppcdis.py`, `tools/re/disasm_range.py` | capstone disasm over the DOL with funcs.txt symbols |
| `dol_sda.py`, `ghidra_scripts/` | SDA/r13 constants; analyzeHeadless helpers |
| `scratch_clean.py` | gated scratch cleaner (refuses paths outside `scratch/`) |
| `tools/gfx/graphics_db.py` | the graphics registry's worklist + curation (`docs/graphics/`) |
| `tools/interp/cadence.py`, `tools/interp/compare_modes.py` | pixel-level ALTERNATION/JUDDER plus the schema-5 Native60/repeat/Lerp60 comparison contract: exact guest ticks and camera matrices, complete provenance, GPU-clean runs, forced-snap control, spatial localization, and explicit refusal to infer draw identity from a screen cell |
| `tools/oracle/capture.sh`, `record_fifo.sh` | pixel + FIFO ground truth from the Dolphin FORK (`extern/dolphin_fork`); both print WHICH binary they picked, because a stock Dolphin has none of the hooks and its capture looks identical |
| `tools/launch/run.py`, `arguments.py`; `tools/render/run_render.py`, `gpu_events.py`, `gpu_preflight.py`, `gpu_watch.py`, `gpu_watch_selftest.py`, `radv_hang_trace.py` | locked-Python owners of the default and native-preview launcher policies; one GPU-kernel fault definition; boot-monotonic cooldown refusal; live fail-fast watcher that kills only its guarded process group before persisting incident + submit-flight evidence, then preserves a PCI-correlated Linux device coredump through a parent-bounded, killable reader process when readable. `gpu_watch_selftest.py` owns the process/signal fixtures, including timeout-final-barrier and every-terminal RADV-report controls, rather than growing the production guard. Explicit `SBR_RADV_HANG_DIAG=1` adds the independent exact-child RADV progress trace and attempts bounded `CAPTURED`/`UNKNOWN` result publication on every terminal path; direct ambient `RADV_DEBUG=hang` is rejected, and the lane is never default because driver synchronization can mask the defect (I034). Both `run.sh` and `run-render.sh` use the same watcher |
| `tools/perf/count_getenv.c` | validated `LD_PRELOAD` interposer that counts environment lookups by name; reports an explicit broken-instrument result when it intercepts nothing |
| `tools/info/registry_paths.py` | no live registry entry may name a file the tree does not have |
| `tools/info/stale_triage.py` | splits the rot check's STALE list into "code EDITED" (needs a run) vs "file MOVED" (bookkeeping) |
| `tools/docs/doc_paths.py` | no live document may name a source path the tree does not have |
| `diag_registry.py` | no switch named in CLAUDE.md/docs/scripts may go unread by code |
| `cpp_quality.py` | changed first-party C/C++ must pass the tracked clang-format style and clang-tidy checks using the runtime and recompiler's real Clang compile databases |

These are the repo's own **gates**: `diag_registry.py` and `doc_paths.py` run in
`.githooks/pre-commit` (with `selftest_all.py`, which runs every tool's `--selftest` and the changed
C++ format/lint gate). Between them a name and a path in the documentation are both
machine-checked — the two halves of the same defect. Five separate document-vs-tree reconciliations
on 2026-08-12 each turned up a LIVE bug rather than confirming the document, which is why they are
gates and not a habit. `structure_check.py` recursively applies the 1,200-line default to
first-party C/C++ and freezes the few larger legacy files at their current size.

## The rest of the tree

Subsystems that carry no status of their own — they are inputs, archives or one-off helpers — but
which a session will look for and should not have to find by guessing.

| Where | What |
|---|---|
| `decomp/sms/src/Player/MarioMain.cpp` (submodule `decomp/sms/`) | the reference decompilation. Compiles native under `SMS_NATIVE_PLATFORM=1`; also the source of truth every RE note is checked against |
| `extern/dolphin_fork/` | the Dolphin FORK (SomeoneIsWorking/dolphin@sunbright) — the pixel and FIFO oracle, carrying the draw-log hook and `--fifo-record`. NOT retired. `extern/dolphin` is pinned upstream, never initialised, used by nothing |
| `reference/sms_gmse01_funcs.txt` | US symbol + function-address list. Every address in an RE note resolves through it |
| `sms-boot/assets/anm_swap.cpp` | byte-order swappers for the decomp runtime's asset loads — the BE-swap catalogue in practice |
| `tools/audio/ab_harness.py` | the audio A/B harness and its residual set |
| `tools/ghidra_scripts/DecompDump.py` | `analyzeHeadless` helpers — the default RE path |

## Live diagnostics (recomp, `SBR_PROBE=1` → 127.0.0.1:17654)

`/help` · `/r /w` (guest memory) · `/screenfx` (screen-sampling effects) · `/wsfx /fills /2d /2dclass`
(widescreen/2D) · `/j3d` (geometry capture + scene matching). Plus `SBR_LUCENT_DEBUG=<chan>` (app, mario, frame,
gxfifo, widescreen, thp, …) and `SB_DUMP_FRAME`/`SB_DUMP_FRAME_AFTER`.

## Where is X?

- boot destination / fastboot → `sms-recomp/overrides/fastboot_native.cpp` (`SBR_FASTBOOT`/`SBR_STAGE`)
- GX stream → aurora → `sms-recomp/runtime/devices/dev_gxfifo.cpp` (`gxfifo_flush`, `emit_arraybase`, EFB copies)
- widescreen aspect → `sms-recomp/overrides/widescreen.cpp` (`ov_c_mtx_perspective`)
- a screen effect (heat haze etc.) → `docs/60fps/screen_effects.md` + `sms-recomp/frame_interp/effects_screen.cpp`
- **60fps, ANY part of it → `docs/60fps/README.md`** — the map of the native simulation path and all three interpolation implementations, every hook by
  guest address, every switch by path, and the unification target. Do not start from the source:
  the hooks are spread over eight files under three different names (`lerp60`, `interp60`,
  `interp60_replace`). An older entry pointed at a since-deleted per-file 60fps path; all of
  it lives under `sms-recomp/frame_interp/` now.
- **what graphics exist, what is RE'd, what interpolates → `docs/graphics/README.md`** and the file it
  describes, `docs/graphics/graphics_db.tsv`. The game writes a row for every emitter it sees draw
  (auto-detected at the three GX waists), with the interpolation verdict aurora measured for it; the
  `re`/`note` columns are curated. Worklist: `tools/gfx/graphics_db.py next`
- which perform list is which → `SBR_INTERP60_LISTS=1` (resolves `gpMarDirector` by scan, names every slot)
- whether a sub-frame is real or the same image twice → `tools/interp/subframe_gate.py` on a consecutive-present series
- WHERE a sub-frame sits between its neighbours (the arc's `asymmetry` number) → `tools/interp/subframe_position.py`
- do the diagnostic tools still work → `tools/selftest_all.py` (runs every tool's `--selftest`; in the pre-commit gate)
- an opcode's emitted C → `tools/recompiler/c_emitter.cpp` + `sms-recomp/generated/functions_*.cpp`
- Mario's position at runtime → `/r?a=0x8040E10C` then deref (pointer, not a position global)

## Source tree

```
sms-recomp/  —  the recomp runtime
├─ generated/     PPC→C++ (2.79M lines, regenerated by tools/recompiler); never hand-edited
├─ app/                        typed persisted host policy and authoritative cadence semantics
├─ bse/                        BetterSunshineEngine-derived game-rate compatibility overrides
├─ ui/                         Dusklight-style RmlUi document/window/settings ownership
├─ runtime/                    11 files   dispatch (rt_core), guest scheduler, boot env, probe
│  ├─ devices/                 19 files   the GC hardware model — one file per device
│  └─ render/                  GX compatibility renderer/reference plus the target semantic-adapter area
├─ overrides/      6.6k lines  27 files   native HW/OS seams + widescreen/HUD/j3d capture
├─ frame_interp/   8.5k lines  29 files   ALL interpolated-60fps code — one API (docs/60fps/)
│                                        + graphics_db.* — the graphics registry (docs/graphics/)
└─ host/          main.cpp
extern/aurora/    the GC platform surface (SDL3 + WebGPU/Dawn), shared by both runtimes
sms-boot/         the decomp+Aurora runtime (the oracle)
native-render/    shared renderer-neutral semantic commands/layout/sinks + SDL3 PC passes
decomp/sms/       the reference decompilation (submodule)
reference/        the US/JP symbol + function-address lists every RE note cites
tools/            every tool lives under a subject directory; launch/ owns shared launcher policy;
                  only cpp_quality.py, diag_registry.py, scratch_clean.py, selftest_all.py and
                  structure_check.py are repo-wide and sit at the root
├─ recompiler/  render/  oracle/  interp/  re/  audio/  perf/  ghidra_scripts/  gfx/
└─ info/  docs/            the registries' own gates — see below
docs/             docs/README.md is the index; codemap.md is the map
debug_journal/    dated findings, dead ends included. NOT a place for current state
```

Repo root launch ownership: `run.sh` is the slim default-product shim into
`tools/launch/run.py`, which owns convenience flags and the live GPU watcher for both unlimited
interactive play and bounded `--diagnostic` runs. `play.sh` is a policy-free compatibility shim;
`run-recomp.sh` is the raw product/build harness, `run-decomp.sh` is the raw decomp oracle harness,
and `run-render.sh` is the guarded native-preview harness. The default launcher refuses during the
GPU cooldown and stops its exact process group on the first new kernel GPU fault while preserving
an incident bundle.
`run-render.sh` additionally requires the explicit per-session `SBR_RENDER_APPROVED=1` accident
gate, which Native device initialization checks again.

## Open heads (details in debug_journal/ and docs/)

- **audio (recomp only)** — DSP voice mixer; the decomp side is already audible and is the oracle. `docs/audio/recomp_plan.md`.
- **THP session reopen** — second movie faults on a null message queue.
- **decomp Delfino gameplay crash** — plaza-population stubs (oracle side).

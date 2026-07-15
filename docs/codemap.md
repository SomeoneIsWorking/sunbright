# Codemap

The single-page answer to **what is where, what's done, what's missing.**
Update the relevant row in the SAME commit that changes a subsystem.
(Architecture rules live in CLAUDE.md; findings in debug_journal/; this is orientation only.)

Legend: ✅ done (verified on real data) · 🟡 partial (documented gap) · 🔬 built/parsed but not wired · ⬜ missing.

## Game flow (boot-order target)

| Stage | Status | Notes |
|---|---|---|
| GC logo | 🟡 | renders + advances, but the **Nintendo logo shows BLUE** (user-observed 2026-07-15) — likely a channel-swap/TEV issue on the boot-logo pass specifically (title colors are correct, so not global). Capture headlessly (record_fifo.sh / fork framedump) before investigating; do NOT hand-debug. |
| Title (stage 15 attract) | 🟡 | renders faithfully at settle (means within 3 of oracle, 2026-07-14); residuals: seagulls missing, anim phase offsets, no EFB copies (mirror/logo reflection), ghost pass |
| File-select / save screen | 🟡 | reachable (Enter=START), renders; formal oracle gate pending |
| Gameplay (Delfino) | ⬜ | boot clears tree init + the GXBegin/GXEnd abort (2026-07-15) → drains the fifo, then aborts. TWO INDEPENDENT failures in the same frame (which fires first varies per run — frame-content non-determinism): (a) the per-frame **storage** staging buffer overflows (Delfino geometry genuinely > the 8MB title cap; sized to 32MB, still can exceed under full load), and (b) a **fifo desync** `unknown opcode 0x70` at a `StaticMapObj ShadowOpa` block boundary (real correctness bug — pinned to a 32-byte-aligned gap; see journal). ⚠️ The earlier "phase-1 ghost pass doubles storage" claim is FALSIFIED — `SB_SKIP_GHOST` was a phantom env (no-op). Real next step: fix the desync (correctness) + right-size storage from a completed frame. See `debug_journal/2026-07-15_delfino_storage_overflow_ghost_pass.md`. Still GATED behind the title oracle gate |
| Audio (everything) | ⬜ | silent by omission; arc plan in docs/audio_native_mixer_plan.md (M1 kernel wiring in progress) |

## Runtime (sms-boot/)

| Subsystem | Status | Where | Gap / next |
|---|---|---|---|
| main / boot | ✅ | `sms-boot/main.cpp` | one-runtime, single thread (CLAUDE.md architecture) |
| frame seam / present | ✅ | `runtime/frame_seam.cpp` | sb_frame_present in TVideo::waitForRetrace |
| FIFO replay harness | 🟡 | `runtime/fifo_player.{cpp,h}` | translator ✅ (mips, display copy, arrays); CI-format TLUT synthesis missing (fail-fast) |
| audio pump | ⬜ | `runtime/audio_out.cpp`, `runtime/sms_boot_audio.cpp` | BARC loader ✅; JAS kernel + DsyncFrame2 mixer = the audio arc |
| SDK stubs | 🟡 | `runtime/sdk_stubs.cpp` | audited 2026-07-10; every stub documented seam or loud |
| pad scripting | ✅ | `runtime/pad_script.cpp` | SB_PAD_SCRIPT virtual pad |
| diagnostics | ✅ | `runtime/phase_track.cpp`, `trace_seq.cpp`, `watchdog.cpp` | phase tags, seq counter, SIGALRM backtraces |
| decomp shims/stubs | 🟡 | `sms-boot/shims/`, `sms-boot/boot_stubs/` | each boot_stub = porting worklist |
| unit tests | 🟡 | `runtime/tests/`, `shims/tests/` | per-port spec tests; gx_yscale, jaudio_release, etc. |

## Aurora (extern/aurora — the GC platform surface; submodule, fork remote branch `sunbright`)

| Subsystem | Status | Where | Gap / next |
|---|---|---|---|
| GX command processor | 🟡 | `lib/gx/command_processor.cpp` | replay + native emission; rich SB_* diag toolkit (draw-dump, ndc-probe, tex-id, per-draw z/acmp bisects) |
| GX state→wgpu | 🟡 | `lib/gx/gx.cpp`, `shader.cpp`, `shader_info.cpp` | WGSL gen; open: seagull zero-fragments (journal 2026-07-14_seagull_narrowing.md) |
| EFB copies / XFB present | 🟡 | `lib/gx/`, `lib/aurora.cpp` | display-copy present arc ✅ (RMSE 0.068 vs oracle); 7-tap copy vfilter unported; SB_RDOC trigger blocked on Dawn crash |
| dolphin SDK layer | 🟡 | `lib/dolphin/` (dvd, pad, vi, gx, card, os…) | dvd fully sync ✅; pad keyboard defaults ✅ (2026-07-15); CARD host-alloc gating open |
| texture cache | 🟡 | `lib/gfx/texture*` | (texObjId, version) keyed; mip chains honored |

## Reference decomp (reference/sms — submodule, SomeoneIsWorking/sms fork)

Compiles native via SMS_NATIVE_PLATFORM + SMS_AURORA. **Native-only, no recomp** (decided
2026-07-15, CLAUDE.md) — decomp gaps are hand-ported. Known decomp-bug classes fixed so far:
BE swaps, LP64 overlays, fused-immediate phantom constants (mBlack 2026-07-14), swapped anim
args (sparkles 2026-07-15), region count skew (JP 13 vs US 18 panes), retail overflows benign
on PPC but host-corrupting (4x4-into-3x4 Mtx, SMS_GetLightPerspectiveForEffectMtx 2026-07-15),
dropped GC-no-op calls (GXEnd 2026-07-15).

**Decomp gaps** = ~64 empty `src/**.cpp` files (still hand-port surface). Accelerator: the
`upstream` remote is `doldecomp/sms` (we are 310 commits ahead / 24 behind); a full merge is
conflict-risky, but our EMPTY gap files can be taken from `upstream/main` file-by-file. As of
2026-07-15 only 6 of our 64 gaps are filled upstream: Animal/{AnimalBase,boid,fishoid},
Enemy/{bossManta,gatekeeper,egggen}. NOT drop-in: because our fork is 310 commits ahead,
upstream bodies reference newer decomp symbols our headers lack — AnimalBase.cpp needed ~11
header reconciliations (missing enums CUE_MOVE/CUE_CALC_*, LIVE_FLAG_UNK20, type
TAnimalBaseUnk150, method decls initNoLoad_/flyToCurPathNode/animalWalkIn, API renames
setEulerX→setEuler, MSound::startSeRandPlay). So each cherry-pick = copy body + drop the
matching boot_stubs + reconcile ~10 header divergences + build/verify — a focused per-file
cycle, modest leverage. (Tried AnimalBase 2026-07-15, reverted: needs the header pass first.)
Ports landed 2026-07-15: TMapObjTree::initMapObj/initEach, SMS_GetLightPerspectiveForEffectMtx.
When JP decomp misbehaves on the US disc, check the US disasm first
(`tools/re/disasm_range.py scratch/bin/sms.dol …`; regenerate the DOL with
`tools/re/dol_extract.c` — build cmd in its header).

## Tools (tools/)

| Tool | Purpose |
|---|---|
| `oracle/parse_fifo_dff.py` | .dff ground-truth parser (draw counts, BP/CP/XF state; posmtx blind to LOAD_INDX) |
| ⛔ `oracle/xdrive.py` | XTest GUI driver — DEPRECATED, do NOT use. Driving the Dolphin GUI is banned (user, 2026-07-15). Use the fork's headless tool below. |
| `oracle/record_fifo.sh` | ✅ Headless .dff capture (no GUI). Wraps the fork's `DolphinNoGUI --fifo-record`. `record_fifo.sh <out.dff> [after=7500] [frames=3]`. Recaptured `title_press_start.dff` (settled, pixel-validated). |
| Dolphin **fork** headless | `extern/dolphin_fork/` (SomeoneIsWorking/dolphin@sunbright, gitignored scratch clone; BUILT: `build/Binaries/dolphin-emu-nogui`). `--fifo-record` NoGUI flag = fork commits dc57256+05c8f74. Build: submodules `--init --depth 1` (deinit Qt/mGBA/FFmpeg-bin), `-DENABLE_QT=OFF -DENABLE_EVDEV=OFF -DUSE_MGBA=OFF`, target `dolphin-emu-nogui`. Stock Fedora dolphin-emu has headless bugs the fork fixes. TODO: repoint the framedump pixel-oracle path (capture.sh uses stock `-b`; NoGUI needs `-p headless`) at the fork too. |
| `oracle/capture.sh` | Dolphin boot framedump capture (repoint at the fork binary) |
| `re/ppcdis.py`, `re/disasm_range.py` | capstone disasm over scratch/bin/sms.dol with funcs.txt symbols |
| `re/dol_extract.c` | main.dol from RVZ via the build's nod prebuilt |
| `dol_sda.py` | SDA/r13 constant resolution |
| `ghidra_scripts/` | analyzeHeadless decomp/disasm helpers |
| `render/`, `audio/`, `interp/` | 🟡 older harnesses — verify liveness before trusting (several are dead-era) |

## Open investigation heads (details in debug_journal/)

- Seagull zero-fragments (aurora GPU-side; next: VS-writeback debug or RenderDoc fix) — `2026-07-14_seagull_narrowing.md`
- Ghost pass frame-head dispatch (needs Dolphin CPU oracle) — `2026-07-14_ghost_pass_re.md`
- EFB copies at title (mirror capture + logo environment reflection) — unported
- Anim-phase residuals (PRESS START / SUNSHINE offsets vs oracle)

## Known doc rot (pending prune)

`docs/dolphin_integration.md`, `gx_sdlgpu_switch.md`, `interp60*.md`,
`model_interpolation.md`, `native_threading.md`, `render_ab_harness.md` describe retired
eras (recomp/Dolphin-hook/Path-B). Per the no-tombstones rule they should be deleted once
any still-true facts are lifted into living docs — not yet audited file-by-file.

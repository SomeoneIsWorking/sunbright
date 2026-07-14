# Codemap

The single-page answer to **what is where, what's done, what's missing.**
Update the relevant row in the SAME commit that changes a subsystem.
(Architecture rules live in CLAUDE.md; findings in debug_journal/; this is orientation only.)

Legend: ✅ done (verified on real data) · 🟡 partial (documented gap) · 🔬 built/parsed but not wired · ⬜ missing.

## Game flow (boot-order target)

| Stage | Status | Notes |
|---|---|---|
| GC logo | ✅ | renders + advances |
| Title (stage 15 attract) | 🟡 | renders faithfully at settle (means within 3 of oracle, 2026-07-14); residuals: seagulls missing, anim phase offsets, no EFB copies (mirror/logo reflection), ghost pass |
| File-select / save screen | 🟡 | reachable (Enter=START), renders; formal oracle gate pending |
| Gameplay (Delfino) | ⬜ | OSPanics at unported TMapObjTree::initMapObj — GATED behind title oracle gate |
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

Compiles native via SMS_NATIVE_PLATFORM + SMS_AURORA. Known decomp-bug classes fixed so
far: BE swaps, LP64 overlays, fused-immediate phantom constants (mBlack 2026-07-14),
swapped anim args (sparkles 2026-07-15), region count skew (JP 13 vs US 18 panes).
When JP decomp misbehaves on the US disc, check the US disasm first
(`tools/re/disasm_range.py scratch/bin/sms.dol …`; regenerate the DOL with
`tools/re/dol_extract.c` — build cmd in its header).

## Tools (tools/)

| Tool | Purpose |
|---|---|
| `oracle/parse_fifo_dff.py` | .dff ground-truth parser (draw counts, BP/CP/XF state; posmtx blind to LOAD_INDX) |
| `oracle/xdrive.py` | XTest GUI driver (Dolphin FIFO recorder on Xvfb) |
| `oracle/capture.sh` | Dolphin boot framedump capture |
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

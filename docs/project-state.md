# Project state

Factual capability ledger for Sunbright. Epic intent is `docs/project-goals.md`; architecture and
ordering are `docs/architecture.md` and `docs/port/migration.md`; atomic work is `docs/issues/`;
placement is `docs/codemap.md`.

## Current focus

S004/S003: a gameplay composition target that consumes `gcnport`'s executor, then a native override
that *replaces* guest behaviour at `J3DShape::draw` (`0x802e0390`) instead of observing it — every
material GMSE01 draws now classifies, so the remaining renderer work is fidelity, not coverage.

## Capability inventory

| ID | Capability | State | Evidence or gap |
| --- | --- | --- | --- |
| S001 | Exact `GMSE01` boots under `gcnport`/Dolphin JIT and reaches the `J3DShape::draw` hook at `0x802e0390` | verified | `tools/gcnport_boot/gmse01_boot.cpp`: 4B-block run, 13,595 blocks compiled, 0 invalid guest accesses, 1,496 VI retraces, 245,874 entries to `J3DShape::draw`; details and the superseded readings in `docs/issues/0037` |
| S002 | `gcnport` supplies a title-neutral Dolphin dynarec executor | partial | `RuntimeSession`, `BootAuthenticatedImage`, `ExecuteJitBlock(s)`, `ExecuteOriginalOnce`, `CallOriginalSynchronously`, `InvalidateGuestCode`, typed `ExecutionCounters`; `1,109,684` fallbacks over 22 sites are all Gekko `mfspr`/`mtspr` (99.97% compiled), so no missing-instruction gap remains |
| S003 | Sunbright native overrides and original calls use robust image-scoped dispatch | partial | 1,428 native → original → native round trips through `TApplication::drawDVDErr`, each caller reading r3 (`0` on 1,393 frames, `'em_3'` on 35, matching `decomp/sms/src/System/Application.cpp`); gap: every hook installed today is diagnostic — none replaces guest behaviour |
| S004 | The PC-native semantic renderer covers the complete visible J3D/J2D/particle/effect stream | partial | all 51,236 material packets classify into 14 families (0 refused), 59,776 draws composed and submitted, 0 rejected by the sink; gap: no per-region diff against a matched-state oracle capture, so retail's additive sun glow is unverified |
| S005 | Native decomp adapters and recovered source provide independent semantic evidence | partial | `sms-boot` adapters exercise the same semantic values as `title-adapter` without sharing objects; `decomp/sms` parses 580/580 units in both build modes under the gate; gap: 30 files behind upstream, `TPortArgs::unk1C`/`unk20` unnamed, 8 TUs include `sms-boot/shims` headers outside the native guard |
| S006 | Smooth presentation covers every eligible moving source | partial | stable identities recorded for J3D shapes, matrices, cameras, billboards and reached indexed quads; gap: residual palm/sky motion has no stable draw identity, and native-rate modes are not integrated with the executor |
| S007 | Reached decomp behavior is upstream-converged, named and implemented from evidence | partial | merged with `doldecomp/sms` at `6ae2aa86` (205 commits, 1,135 files), diverging files 1,194 → 222; gap: 30 files / 1,429 lines behind, and 19 functions the decomp's own authors marked wrong (issue 42) |
| S008 | The native/dynarec product passes representative interactive gameplay conformance | missing | unattended half measured — `gpApplication` walks the whole attract cycle twice over 23,064 retraces, and a scripted run reaches file-select and Delfino Airstrip; gap: no bounded interactive scenario through the real gameplay target, no oracle comparison, no native subsystem owners active |
| S009 | Offline generator, emitted corpus, static dispatcher, tests and launch paths are absent | verified | `tools/migration_boundary.py` scans 627 first-party paths and finds 0 violations |
| S010 | Independent Dolphin/decomp/binary oracle evidence can locate first divergence | verified | `extern/dolphin_fork` observation hooks, `tools/oracle/` parsers, `decomp/sms`, the GMSE01 symbol corpus and `docs/re_notes/`; proves named scopes, not whole-game parity |
| S011 | Native audio is complete and integrated with the product | partial | an audible native JAS voice renderer exists on the decomp evidence path with the Zelda-class ucode contract, seven sub-frame cadence, AFC/PCM decode, resampling and L/R mixing; gap: no title-owned owner connected to the new lifecycle, and no oracle comparison of music, effects, streaming or routing |
| S012 | Application lifecycle, typed configuration, Lucent logging and structure boundaries are enforced | partial | `tools/structure_check.py` measures 408 files with 0 violations, `tools/cpp_quality.py` formats 260 files and lints 153 TUs, the launcher parser is typed; gap: no gameplay composition root, no product logging owner, no persisted typed configuration |
| S013 | Zero-argument launcher provisions and runs only the native/dynarec product | blocked | `./run.sh` is a slim locked-Python shim into `tools/launch/run.py`, which refuses by naming the missing shared executor; blocked by S002 |
| S017 | JIT gameplay is qualified independently on x86_64, macOS AArch64 and Android arm64-v8a | missing | nothing is qualified; a fallback-heavy run and one AArch64 OS cannot stand in for the others |
| S018 | Asset-free automation verifies the redistributable native renderer and tooling | partial | `tools/verify.py` runs 13 steps including the decomp host compile, Ninja build, ctest and clang-format/tidy, on Linux/Windows/macOS CI; shader provenance pins shaderc `50f71a74` (`v2026.1-port.1`) with checksum-verified archives and all 13 embedded headers match; gap: proves redistributable components only, never boot or gameplay |
| S014 | Desktop packages provide no-terminal first-run setup and contain no game content | missing | nothing packaged |
| S015 | Widescreen renders additional world coverage through projection/viewport/scissor ownership | partial | native projection, HUD placement and several screen/effect boundaries exist with GMSE01-specific RE notes, and no final-image stretching; gap: not integrated with the single product, and every horizontal culling/scissor boundary is unenumerated |
| S016 | Native input, controls, saves and settings work through one typed product policy | partial | keyboard/controller translation, memory-card work, persisted renderer/frame-rate/effect settings and an in-game settings UI exist on the retired path; gap: no typed immutable product policy, no OS user-data storage, nothing on the dynarec path |

## Comparison baseline

The baseline is the unmodified NTSC-U release on hardware or Dolphin: console execution, GX
rendering, 4:3 framing, 30 Hz presentation. Sunbright currently has no gameplay executable, so every
delta below is intended and unproven:

| Delta from baseline | State |
| --- | --- |
| One native/dynarec executable running the player's own image | missing (S013) |
| PC-native semantic rendering for J3D, J2D, particles, lights, cameras, effects | partial (S004) |
| Widescreen with additional world coverage, no image stretching | partial (S015) |
| Smooth presentation between original simulation ticks, plus separately qualified native-rate modes | partial (S006) |
| Native audio, input, saves, configuration and in-game settings | partial (S011, S016) |
| Asset-free desktop packages with a no-terminal first-run picker | missing (S014) |

# Codemap

Sunbright's ownership and placement map. Capability state belongs in `docs/project-state.md`, work
order in `docs/port/migration.md`, and evidence in `docs/info/` and focused RE notes.

```text
application composition
  -> typed configuration / Lucent logging
  -> gcnport executor / Dolphin JIT
  -> GMSE01 identity + native override owners
  -> semantic adapters / native host services
  -> native-render GPU platform and presentation
```

| Subsystem | Responsibility | Current / target location | Entry point | Deep doc |
| --- | --- | --- | --- | --- |
| Product composition | Construct focused owners and run the application lifecycle | Future application owner after gcnport exists | Future composition root | `docs/architecture.md` |
| GameCube executor | Runtime image identity, Dolphin CPU/thread transitions, hooks, originals, exits, invalidation, JIT/fallback counters | Shared `gcnport` project | Framework executor API | `docs/port/migration.md` |
| Dolphin core | PowerPC decoder/JIT/cache, memory, and unemancipated devices | `extern/dolphin_fork/` | Dolphin Core/System and JIT dispatch | `docs/architecture.md` |
| Title identity | Validate exact GMSE01 and provision the user image | Future configuration and title owners | Immutable application configuration | `docs/port/migration.md` |
| Native overrides | Image-scoped guest-address hooks and one-call originals | Future title override owners | First seam `J3DShape::draw` at `0x802e0390` | `docs/re_notes/j3d_shape_decode.md` |
| Renderer-neutral schema | Scene values, image/J3D/J2D decoding, bounded frame storage | `native-render/include/`, `native-render/src/` | `native-render/include/sunbright/native_render/frame.h` | `docs/graphics/README.md` |
| Semantic GPU renderer | PC-native passes, shaders, resources, targets, presenter | `native-render/src/`, `native-render/shaders/` | `native-render/src/sdl_semantic_frame_client.cpp` | `docs/graphics/README.md` |
| Renderer controls | Production-boundary CPU and watched GPU controls | `native-render/tests/` | Focused test executables | `docs/graphics/README.md` |
| Guest semantic adapters | Copy GMSE01 values through gcnport memory/state interfaces | Future title adapter owners | High-level runtime hooks | `docs/architecture.md` |
| Native-layout evidence | Exercise semantic contracts from recovered source | `sms-boot/runtime/`, `sms-boot/shims/` | Focused evidence adapters only | `docs/decomp/` |
| Native asset evidence | Decode and transform title assets into ordinary values | `sms-boot/assets/` | Focused asset decoders | `docs/decomp/` |
| Native scaffold evidence | Retain reached declarations and behavior seams for analysis | `sms-boot/boot_stubs/` | Evidence-only source units | `docs/decomp/` |
| Decomp source | Recovered behavior, names, formulas, and native-layout controls | `decomp/sms/` | Upstream-compatible source units | `docs/decomp/` |
| Emulator oracle | Independent state/frame/FIFO observations | `extern/dolphin_fork/`, Python parsers in `tools/oracle/` | Focused parser/observation tools | `docs/project-state.md` |
| Audio | Native sequencing, voice, mix, output, oracle comparison | Future audio owner; retained evidence in `sms-boot/runtime/` and `docs/audio/` | Future audio owner | `docs/audio/native_mixer_plan.md` |
| Presentation | Simulation cadence, interpolation provenance, sync, widescreen | Future presentation owner; retained evidence in `docs/60fps/` | Executor frame exits | `docs/60fps/README.md` |
| Input/UI/saves | Device actions, settings UI, persistent player state | Future focused input, UI, and save owners | Typed application policy | `docs/app/settings.md` |
| Configuration | Sole CLI/environment/file ingestion and immutable validation | `tools/launch/config.py`; future product configuration owner | `parse_launch_config` | `docs/app/settings.md` |
| Logging | Sole product sink/filter/format boundary through Lucent | Future logging owner | Injected logger interface | `docs/architecture.md` |
| Verification policy | Asset-free build/quality checks, native compile-database identity and lint selection, checksum-pinned shader tools, explicit self-test requirements, and the separate game-image RE gate | `tools/verification.py`, `tools/cpp_quality.py`, `tools/cpp_quality_test.py`, `tools/render/shader_toolchain.py`, `tools/selftest_all.py`, `tools/verify_re.py` | `tools/verify.py`, `tools/verify_re.py` | `AGENTS.md` |
| Native runtime deployment | Resolve Windows DLLs from CMake imported targets, stage beside executables, and reject missing or stale deployment | `cmake/SunbrightRuntimeDependencies.cmake`, `tools/runtime_dependencies.py`, `tools/runtime_dependencies_test.py`, `tools/fixtures/runtime-dependencies/` | `sunbright_deploy_runtime_dependencies` | `AGENTS.md` |
| Structure policy | Source caps, dependency edges, config/log ownership, deleted-path checks | `tools/structure_check.py`, `tools/migration_boundary.py` | Python verifier entry points | `AGENTS.md` |
| Audio analysis | Parse and compare native audio evidence | `tools/audio/` | Focused Python tools | `docs/audio/` |
| Document validation | Reject dead live-document paths | `tools/docs/` | `tools/docs/doc_paths.py` | `docs/README.md` |
| Graphics census | Validate and query the graphics registry | `tools/gfx/` | `tools/gfx/graphics_db.py` | `docs/graphics/README.md` |
| Ledger support | Validate project claims and registry paths | `tools/info/` | Focused Python tools | `docs/info/` |
| Presentation analysis | Compare retained frame and motion evidence | `tools/interp/` | Focused Python tools | `docs/60fps/README.md` |
| Performance probes | Measure bounded host/runtime behavior | `tools/perf/` | Focused Python probes | `tools/perf/README.md` |
| Renderer diagnostics | Inspect renderer output and GPU incidents | `tools/render/` | Focused Python tools | `docs/graphics/README.md` |
| Reverse engineering | Decompile/disassemble exact GMSE01 and preserve evidence | `tools/re/`, `tools/ghidra_scripts/` | Small focused queries | `docs/re_notes/` |

## Current source tree

```text
native-render/       renderer-neutral core, SDL GPU passes, shaders, and tests
  native-render/tests/ production-boundary CPU and GPU controls
sms-boot/            non-product native/decomp evidence adapters and host experiments
  sms-boot/assets/     native asset-decoding evidence
  sms-boot/boot_stubs/ reached native/decomp scaffolding evidence
decomp/sms/          recovered upstream game source
extern/dolphin_fork/ maintained Dolphin core and independent oracle hooks
extern/aurora/       bounded GX compatibility/oracle library
tools/               Python verification, RE, oracle parsing, and diagnostics
  tools/fixtures/      redistributable build-metadata fixtures
  tools/audio/         audio analysis and comparison data
  tools/docs/          living-document validation
  tools/gfx/           graphics census tooling
  tools/info/          project-ledger support
  tools/interp/        presentation-analysis tooling
  tools/perf/          bounded performance probes
  tools/render/        renderer and GPU diagnostic tooling
docs/                goals, state, ownership, issues, claims, and RE facts
```

## Placement index

- Dolphin JIT integration, runtime hooks, originals, invalidation, fallback telemetry → `gcnport`.
- GMSE01 address or native behavior → smallest title override/service owner.
- Renderer-neutral mesh/material/image/frame semantics → `native-render/`.
- Guest-layout extraction → title adapter over gcnport memory/state interfaces.
- Recovered behavior or names → `decomp/sms/` and one focused RE note when explanation is needed.
- CLI/environment/persisted setting → configuration owner; consumers receive typed values.
- Product diagnostics → Lucent logging owner; no direct product stderr/stdout.
- Cross-platform self-test selection or game-image RE gate policy → `tools/selftest_all.py`,
  `tools/verify.py`, and `tools/verify_re.py`.
- Shader compiler/validator source pins, safe provisioning, and installed-tool resolution →
  `tools/render/shader_toolchain.py`.
- Windows executable DLL deployment and integrity → `cmake/SunbrightRuntimeDependencies.cmake`
  and `tools/runtime_dependencies.py`; dependency paths come from the CMake target graph.

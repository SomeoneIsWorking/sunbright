# Sunbright agent guidance

Sunbright is the Super Mario Sunshine (`GMSE01`, NTSC-U) native/dynarec port. The product combines
maintained native overrides and the PC-native semantic renderer with on-demand execution of every
remaining PowerPC path through Dolphin's runtime dynarec, integrated by the shared `gcnport`
framework.

Read these authorities before non-trivial work:

1. `docs/project-state.md` — factual capability coverage and the one current focus.
2. `docs/project-goals.md` — durable product outcomes.
3. `docs/architecture.md` — product boundaries and dependency direction.
4. `docs/port/migration.md` — migration order and acceptance gates.
5. `docs/codemap.md` — responsibility ownership and placement.
6. `docs/issues/` and `docs/info/` — atomic work, claims, and instrument trust.

The portfolio-wide architecture and order live in the shared `jit-common` repository's
`docs/migration.md`. Sunbright's authorities refine that contract for this title; they do not create
an alternative execution methodology.

## Product execution contract

- There is one gameplay product. Native overrides own deliberately selected game or host behavior.
  Dolphin's JIT dynamically translates every other reached guest instruction from the user's
  authenticated game image.
- Dolphin's JIT is the default gameplay executor. A bounded interpreter fallback may run only after
  the JIT explicitly refuses to compile or safely execute a block. Every entry records a typed
  reason, guest PC, block count, and instruction count, then returns to JIT dispatch. It is never a
  first pass, compile-wait bridge, missing-backend substitute, or unbounded compatibility mode.
- Interpreter-only execution is an explicit test/diagnostic surface. A fallback-heavy or zero-JIT
  run cannot prove gameplay compatibility or performance.
- The retired gameplay executor is absent and must not be reconstructed or run. Static analysis may
  produce symbols and non-executable metadata. Existing captures, claims, decompilation, and binary
  analysis remain evidence; new runtime evidence comes from Dolphin, hardware, the decomp/reference
  path, or a separately built test oracle.
- Removed executor artifacts, tests, and selectors must not return as a migration bridge, oracle, or
  comparison arm.
- A runtime-populated code cache is disposable OS user data keyed by exact image, core, host, and
  configuration. A fresh install never requires a cache.

## First executable discriminator

The first wiring milestone is exact and deliberately narrow:

1. authenticate and boot `GMSE01` through `gcnport` and Dolphin's JIT;
2. prove nonzero JIT-translated block execution;
3. intercept `J3DShape::draw` at guest address `0x802e0390` through a robust runtime hook;
4. submit the existing renderer-neutral J3D operation to `native-render`;
5. execute the original body through the JIT with the current override suppressed for that one call;
6. prove the ordinary cold block compiled before execution and report fallback counters with
   denominators; and
7. prove the gameplay binary contains no retired executor artifacts or alternate selector.

The hook must be correct across block boundaries, direct chaining, cache hits, invalidation,
savestate restore, and supported host architectures. Its key includes every image/module generation
needed to prevent stale selection. Installing or changing it invalidates translated links that could
bypass the decision. Never special-case a compiled block shape or patch one observed host address.

## Runtime ownership

- **`gcnport`** owns title-neutral GameCube execution: Dolphin integration, authenticated runtime
  images, CPU/thread state transitions, guest-address hooks, original calls, bounded executor exits,
  invalidation, and JIT diagnostics. Missing shared behavior is implemented there, not copied here.
- **Dolphin** owns its PowerPC decoder, JIT backends, code cache, memory system, and emulated devices
  until a verified native Sunbright owner replaces a specific boundary. `gcnport` does not exist yet;
  the product must refuse by name until it embeds this owner. Do not wrap or duplicate its
  code cache in `jit-common`.
- **Sunbright** owns `GMSE01` identity, native override registration and implementations, semantic
  extraction, title policy, native host services, UI, configuration, and composition.
- **`native-render/`** owns renderer-neutral scene values, asset decoding, semantic passes, GPU
  resources, and presentation. It must not consume FIFO, BP/XF registers, TEV programs, EFB-copy
  choreography, Dolphin renderer state, or title object layouts.
- **`decomp/sms`** is readable recovered game source and a native-layout evidence adapter. It is not a
  second shipping gameplay engine and is never linked to guest-layout objects.
- **Aurora and the SDL3-GPU GX path** are bounded compatibility/oracle instruments. They do not define
  the product renderer and do not remain as a shipping fallback once their covered semantic owners
  are verified.

Native overrides use `gcnport`'s canonical CPU/memory/service interfaces, reproduce the guest ABI and
stack/register effects, and keep ordinary JIT execution available for controlled A/B until proven.
Calls back to guest code enter the normal JIT dispatcher. An override is never a substitute for a missing
PowerPC instruction semantic or an unknown crash.

## Native renderer contract

USER 2026-08-28: "Native renderer doesn't mean anything if it'll be identical to Aurora meaning still using GameCube rendering"

Intercept above GX at game-semantic owners: J3D meshes/poses/materials, cameras/lights, particles,
J2D/UI, resources, and named effects. Preserve authored content, ordering, visibility, animation, and
appearance without preserving GameCube fixed-function implementation details. The semantic contract
contains ordinary PC values and stable resource identities, never guest/decomp objects or layouts.

`J3DShape::draw` at `0x802e0390` is the first dynarec-integrated seam because its GMSE01 behavior,
guest layout, native-layout counterpart, and renderer path are already evidenced. Extend semantic
coverage by exact high-level family and refuse unsupported families by name; do not approximate a
TEV program or infer policy from output pixels.

Widescreen is deterministic projection/viewport/scissor ownership. Interpolation acts only on
matching source geometry with explicit identity. Neither feature may inspect adjacent frames or
sample pixels to decide what extra world content exists.

## Decomp and evidence

- Preserve verified binary addresses, object layouts, formulas, native renderer inputs, decomp
  findings, and oracle controls. A fact learned from the old executable path remains useful evidence;
  it does not preserve that path as architecture.
- Rebase `decomp/sms` before hand-porting a gap. Adopt equal-or-better upstream header/source units,
  then name established unknowns, then extend remaining gaps from binary evidence.
- `extern/dolphin_fork` is the maintained independent emulator/oracle fork. Its frame/FIFO hooks and
  bounded controls remain useful, but product execution enters Dolphin through `gcnport`, not through
  title-local JIT patches.
- Every comparison instrument must prove a known-positive and known-negative answer, report
  denominators, and state what it does not cover. Boot, a clean log, or a single frame is not parity.
- Consult `docs/info/claims/` before citing prior measurements. A holding claim is unchallenged, not
  automatically current. Fix or falsify stale evidence instead of appending a contradictory note.

## C++ structure and ownership

The target is self-contained and responsibility-driven; no other game's directory or class layout is
a template.

- Stateful owners are focused C++ classes with RAII lifetimes, explicit constructor dependencies,
  narrow APIs, and composition. Pure transformations are free functions or value types.
- The gameplay entry point is a composition root only: parse configuration, construct owners, connect
  interfaces, run, and destroy in reverse dependency order. It owns no renderer, UI, JIT, input,
  audio, or persistence implementation.
- `config` is the sole environment/CLI/file ingestion boundary and produces validated immutable typed
  configuration. Product modules never call `getenv` or parse strings themselves.
- `logging` is the sole sink/filter/format boundary and uses Lucent. Product code never writes
  directly to stdout/stderr, platform debug APIs, or ad-hoc logging macros.
- The runtime executor, override registry, semantic adapters, renderer, audio, input, UI, saves, and
  diagnostics are peer owners connected by narrow interfaces. Avoid service locators, global mutable
  state, singleton registries, catch-all managers, and numbered file fragments.
- A live game instance owns its executor, override state, clocks, services, and renderer clients.
  Process-global title state is a defect unless an upstream dependency proves a singleton constraint
  and composition enforces one instance.
- Split a touched monolith before extending it. Do not copy policy between the old tree and the target
  tree during migration; move one responsibility and its tests atomically.

## Mechanical quality gates

The normal verifier must fail with exact files and counts for:

- first-party source above 1,200 lines; freeze existing larger files at their measured size and ratchet
  downward when extracting them;
- forbidden dependency edges, especially native renderer dependencies on GX/Dolphin/Aurora state;
- direct stderr/stdout/debug writes outside the logger owner;
- environment reads or CLI parsing outside the configuration owner;
- product linkage or selectors containing an interpreter or retired executor artifact;
- growth or reintroduction of any retired execution source, build rule, or launch route.

Use C++20, Clang for agent verification builds, tracked `clang-format` and `clang-tidy`, and Ninja for
large CMake corpora. Build output belongs under `build/`; bounded runtime artifacts belong under
stable gitignored `scratch/` activity directories. Never use raw `rm`; use the scoped cleanup tools.

## Supported host backends

Gameplay release evidence is independent for x86_64, Apple Silicon macOS AArch64, and Android
arm64-v8a. Each must prove executable-memory publication/protection, instruction-cache coherence,
ABI transitions, exceptions/signals, packaging, representative interactive gameplay, and measured
fallback denominators. One AArch64 operating system does not prove the other.

## Launcher and player data

The zero-argument `./run.sh` currently refuses by naming the missing shared `gcnport` Dolphin-JIT
executor. It must never run tests, select an evidence host, or launch a stale binary. Once S013 is
implemented it provisions and launches only the native/dynarec product from the user-supplied image.

Game files never enter Git or packages. Developer discovery order is explicit argument,
environment/`.env`, then repo drop-in; packaged first run uses a native file picker and persists the
validated selection in OS user data. Validate exact `GMSE01` identity before mapping or executing it.

## Working discipline

- Search `docs/issues/` and run the project-information brief before re-deriving a symptom.
- Fix root causes at the owning layer. No magic offsets, failing-input special cases, swallowed
  errors, retry-until-pass, sleeps to hide races, silent fallbacks, or skipped checks.
- Automated runs are headless, muted, bounded, and driven through the control channel. Kill only the
  exact owned PID with the shared safe-kill helper.
- Write durable facts to the nearest authority once. Goals own intent, project state owns factual
  coverage, issues own atomic work, the codemap owns placement, claims own falsifiable evidence, and
  RE notes own binary behavior.
- The active title is Sunbright/`GMSE01`. Do not start another GameCube title until Sunbright passes
  its complete representative-gameplay and host qualification gates.

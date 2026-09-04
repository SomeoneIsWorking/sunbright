# Project goals

This document owns Sunbright's durable product outcomes. Factual coverage lives in
`docs/project-state.md`; architecture and migration order live in `docs/architecture.md` and
`docs/port/migration.md`; atomic work lives in `docs/issues/`.

## G001 — Smooth presentation without changing authored gameplay

**Outcome:** Present the original 30 Hz simulation smoothly and offer separately qualified native
simulation-rate modes without changing game rules or using frame-content heuristics.

**Why it matters:** Sunshine's motion should feel appropriate on current displays while retaining
the original simulation as the faithful baseline.

**Success conditions:**

- Every continuously visible moving source has stable provenance and is explicitly interpolated,
  snapped, or reported missing.
- First sightings, discontinuities, long absences, 2D/exact effects, and mismatched geometry do not
  count as interpolation successes.
- Native-rate modes preserve gameplay timing and meet their frame-time budgets independently of the
  interpolated path.
- Widescreen and interpolation decisions never depend on adjacent-frame pixels or sampled content.

**Constraints and non-goals:** Interpolation may blend only matching source state; it does not
fast-forward simulation, fabricate geometry, or hide a performance failure.

Contributing state items: S006, S008, S015.

## G002 — Maintain readable, evidence-grounded game behavior

**Outcome:** Keep the native decomp useful as readable game source and expand it from upstream and
binary evidence without turning it into a second shipping execution engine.

**Why it matters:** The decomp names retail behavior, supplies native-layout controls, and makes
native replacements reviewable without requiring gameplay code to be rewritten.

**Success conditions:**

- Upstream is integrated before local gap work; equal-or-better header/source units converge.
- Established unknowns are named precisely and remaining reachable gaps are implemented from binary
  or equivalent independent evidence.
- Native-layout adapters and pure formulas exercise the same renderer/service contracts as the
  guest-layout path without sharing game objects or layouts.
- The product continues to execute non-native game behavior from the original image through the JIT;
  decomp completeness is not a prerequisite for playing.

**Constraints and non-goals:** Do not rewrite portable game behavior merely to eliminate guest
execution, and do not link native decomp objects to guest-layout objects.

Contributing state items: S005, S007, S010.

## G003 — Ship one native/dynarec gameplay product

**Outcome:** Run exact `GMSE01` as a single product composed of native overrides and Dolphin's
runtime PowerPC dynarec through `gcnport`.

**Why it matters:** Runtime translation preserves complete title coverage without a generated source
corpus, while native ownership can replace deliberately chosen behavior at stable semantic seams.

**Success conditions:**

- `gcnport` owns the title-neutral Dolphin executor, authenticated image, runtime hooks, original
  calls, bounded exits, invalidation, and diagnostics.
- Ordinary cold guest blocks compile through Dolphin's JIT before execution. A bounded fallback may
  run only after explicit compile/safe-execution refusal, with typed reasons, guest PCs, block and
  instruction counters, denominators, and return to JIT dispatch.
- Interpreter-only execution is diagnostic; first-pass, compile-wait, missing-backend, unbounded,
  fallback-heavy, and zero-JIT execution cannot establish gameplay or performance compatibility.
- Native overrides are keyed by complete runtime image/module identity plus guest address, can call
  the original body through the JIT without recursion, and remain correct across chaining and cache
  invalidation.
- A fresh checkout provisions from the user's original game image without generating or compiling
  guest code offline.
- The offline generator, emitted corpus, static dispatch/runtime glue, static-only tests, and static
  launch paths remain absent from every development and release milestone.
- x86_64, Apple Silicon macOS AArch64, and Android arm64-v8a pass representative gameplay and host
  JIT qualification independently.

**Constraints and non-goals:** The first `GMSE01` boot and `J3DShape::draw` hook at `0x802e0390`
prove wiring only; they do not count as gameplay parity.

Contributing state items: S001–S003, S008, S009, S013, S017.

## G004 — Render through PC-native game semantics

USER 2026-08-28: "Native renderer doesn't mean anything if it'll be identical to Aurora meaning still using GameCube rendering"

**Outcome:** Render the complete game through renderer-neutral J3D, J2D, particle, camera/light,
resource, and effect values rather than through GX/FIFO reproduction.

**Why it matters:** A PC renderer should own understandable meshes, materials, shaders, passes,
resources, and presentation instead of recreating the GameCube fixed-function pipeline.

**Success conditions:**

- Native hooks at verified game-semantic boundaries submit ordinary PC values and stable resource
  identities to `native-render/`.
- The renderer accepts no FIFO commands, BP/XF registers, TEV program representation, EFB-copy
  protocol, Dolphin renderer state, or runtime-specific object layout.
- Representative scenes preserve authored content, ordering, visibility, animation, lighting,
  effects, and intended appearance; unsupported semantic families fail visibly during development.
- The completed product presents without Aurora or the SDL3-GPU GX compatibility renderer.

**Constraints and non-goals:** Aurora and GX compatibility may supply bounded coverage/oracle
evidence, but pixel identity to either is neither necessary nor sufficient for completion.

Contributing state items: S003–S005, S008, S015.

## G005 — Deliver a maintainable player-facing port

**Outcome:** Provide a portable, configurable, packaged PC application whose architecture remains
cohesive and whose player data is owned by the operating system's user-data locations.

**Why it matters:** A correct engine path is not a usable port if setup requires maintainer scripts,
the code grows into monoliths, or diagnostics/configuration are scattered through product modules.

**Success conditions:**

- The composition root wires focused RAII owners; configuration is immutable and typed; Lucent is
  the only product logging boundary.
- Mechanical gates enforce source-size ratchets, dependency direction, configuration/logging
  ownership, formatting, linting, and the absence of static/interpreter product paths.
- Zero-argument `./run.sh` provisions and launches the intended product without running tests.
- Desktop packages contain no game files, provide a native first-run picker, validate exact title
  identity, and persist configuration/saves in OS user data.
- Input, audio, saves, settings, and supported host architectures pass bounded gameplay checks.

Contributing state items: S011–S017.

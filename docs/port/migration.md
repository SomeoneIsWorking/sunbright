# Native/dynarec migration

This is Sunbright's execution migration authority. The destructive boundary has already happened:
the retired gameplay executor, its artifacts, selectors, launchers, and executor-only tests/tools
are absent. Gameplay remains unavailable until the shared
`gcnport` project embeds Dolphin's maintained runtime JIT. No compatibility bridge replaces it.

## Completion boundary

A fresh checkout must accept the user's exact `GMSE01` image, build the runtime locally,
compile ordinary cold guest blocks before execution, reach representative interactive gameplay,
run the semantic renderer and native owners, and qualify every released host. Boot, FMV, menus, one
hook hit, or nonzero JIT counters are wiring evidence only.

The default executor is always the JIT. A bounded interpreter fallback is allowed only after an
explicit JIT compile or safe-execution refusal. Every entry carries a typed reason and guest PC,
counts blocks and instructions, and returns to JIT dispatch. Interpreter-only mode is diagnostic.
First-pass interpretation, interpreting while compilation is pending, substituting for a missing
host backend, and unbounded fallback are forbidden. Fallback-heavy and zero-JIT runs cannot prove
gameplay or performance.

## Phase 0 — destructive boundary (complete)

The former generator, emitted corpus, dispatcher/runtime substrate, build and launch selectors,
static-only diagnostics, and generation tools were deleted before replacement work. The stable
`./run.sh` now refuses by naming the missing `gcnport` Dolphin-JIT boundary. It cannot discover or
launch a stale executable. `tools/migration_boundary.py` prevents those surfaces returning.

Independently useful facts remain: exact title identity, addresses and layouts, decomp source,
native-render value contracts, native evidence adapters, binary behavior notes, and the maintained
Dolphin oracle fork. These are evidence and components, not another gameplay executor.

## Phase 1 — establish gcnport ownership

Create the shared framework around Dolphin, not a title-local CPU engine. It owns authenticated
image/module generations, CPU and thread transitions, guest-address hooks, one-call originals,
bounded exits, invalidation, and execution telemetry. Dolphin retains its decoder, JIT backends,
code cache, memory system, and emulated devices.

Required controls force both answers for hook hit/miss, enabled/disabled override, ordinary/original
call, cache hit/miss, direct-chain invalidation, executable mutation/retranslation, and bounded
fallback/refusal. Telemetry includes translated blocks and instructions, cache activity,
invalidations, overrides, original calls, fallback reasons, and denominators.

## Phase 2 — exact title and first semantic seam

1. Resolve and validate exact `GMSE01` through the typed configuration boundary.
2. Boot through gcnport and prove an ordinary cold block compiled before execution.
3. Register `J3DShape::draw` at `0x802e0390` under complete runtime identity.
4. Copy the already-evidenced renderer-neutral mesh, pose, material, texture, camera, light, fog,
   and raster values for an admitted family.
5. Submit through the bounded semantic-frame owner.
6. Execute the original body through one-call override suppression and verify guest ABI state.
7. Exercise cache hit, chaining, hook replacement, and invalidation controls.

The hook must not depend on one block split, patch an observed host address, or survive replacement
inside a stale direct link. A CPU semantic gap belongs in Dolphin, not a title override.

## Phase 3 — migrate native owners by responsibility

Move only independently useful behavior into focused RAII owners: identity/configuration/logging and
composition; frame exits, input, storage, and services; semantic frame lifecycle and high-level
J3D/J2D/particle/effect adapters; audio; then interpolation, widescreen, and native-rate policy.
The composition root wires narrow interfaces and owns no subsystem implementation. Each move keeps
one source of truth and deletes superseded code rather than retaining parallel paths.

## Phase 4 — semantic renderer coverage

Attach `native-render/` through gcnport's CPU/memory interface. For each high-level family, identify
the exact semantic entry and lifetime, copy ordinary values and immutable resource content, prove
the production CPU/GPU path can show both answers, and compare content, ordering, visibility, and
appearance against an independent oracle. Once owned, unsupported semantics fail visibly; they do
not resume GX as a silent product fallback.

## Phase 5 — representative gameplay and hosts

Drive a bounded interactive scenario from normal title flow into Delfino. Compare guest state,
memory, exceptions, timing, interrupts, services, audio, input, and frames at the smallest useful
boundaries. Report JIT work and fallback with denominators, plant an override-disable difference and
an executable mutation, and measure frame-time percentiles, memory, loading, sustained behavior,
and clean shutdown.

Repeat the shipping gate independently on x86_64, Apple Silicon macOS AArch64, and Android
arm64-v8a. Each must prove executable-memory publication/protection, instruction-cache coherence,
ABI transitions, signal/exception behavior, packaging, and representative gameplay. Desktop or one
AArch64 OS is not evidence for another.

## Structure and launcher gates

The normal verifier must enforce the 1,200-line source cap and downward ratchets; native-render's
dependency boundary; one typed configuration owner; Lucent as the sole product logging boundary;
Python tooling except the slim `run.sh`; portable build/scratch paths; and absence of the deleted
executor surfaces. Controlled negative selftests must prove every structural rule can fail.

When gameplay exists, `./run.sh` provisions and launches only the native/dynarec product and never
runs tests. Packages contain no game files, provide a native picker, validate a direct image or one
bounded nested ZIP, preserve a prior valid choice on failure, and store player data and disposable
JIT caches in OS user-data locations.

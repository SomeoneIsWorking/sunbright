---
id: 37
title: Migrate Sunbright execution to gcnport Dolphin dynarec
status: open
symptom: The intended native/dynarec product is not runnable: exact GMSE01 still lacks a complete gcnport/Dolphin-JIT boot path and a robust runtime J3DShape::draw hook at 0x802e0390.
state_items: S001,S002,S003
tags: migration,gcnport,dolphin,jit,override
created: 2026-09-04
updated: 2026-09-04
---

## Root cause

Sunbright's title-owned runtime was built around an offline-generated PowerPC corpus and a
title-local dispatch table. That ownership cannot provide the required product contract: one
maintained platform executor translating the user's live image on demand, robust hooks independent
of JIT block shape, and mechanical absence of interpreter/static fallback. The replacement owner is
the shared `gcnport` framework around Dolphin's runtime JIT.

## What was tried / dead ends

Do not revive the prior mixed executor or use removed executor artifacts as the comparison
leg. Existing evidence from that path remains useful only for exact addresses, layouts, behavior,
and reached scenarios. A title-local wrapper around Dolphin's current block split is also rejected:
direct chaining or invalidation could bypass it and another GameCube title would need to duplicate
the same integration.

## Progress note (2026-09-17)

`shared/gcnport` already exists (it predates this note; it is not being created fresh) at
`~/repo/shared/gcnport`, pinned to a `SomeoneIsWorking/dolphin` fork submodule at
`extern/dolphin` (currently checked out at `818ef9de938b3672880f5ff1468729fdaf643679`, `main`
branch). Investigated this session, with exact evidence:

- **Dolphin's `Core` is already a linkable library target** independent of `DolphinQt`/
  `DolphinNoGUI` (`Source/Core/Core/CMakeLists.txt`); gcnport's own CMake configures the fork with
  `-DENABLE_QT=OFF` and links only `core` + test support, not a GUI frontend.
- **Boot/drive loop**: `Core::Init`/`Core::Run`/`Core::System` own the open-ended CPU loop via
  `CPUCoreBase::Run`; there is still no instance API on the fork that executes one observable JIT
  block and returns a bounded host exit (`BootAuthenticatedImage` / `ExecuteJitBlock` from
  `docs/dolphin-embedding-contract.md` in gcnport are contract, not yet implemented as public
  callable API).
- **Hook mechanism**: the fork's pinned `Source/Core/Core/PowerPC/GcnPortRuntime.h/.cpp` (new,
  title-neutral) emits a hook guard at every hooked guest operation inside Jit64/JitArm64-generated
  blocks (not a title-local JIT block patch and not `PowerPC::HLE`). `RunOriginalOnce` falls through
  that guard for exactly the current entry, satisfying the "superCall" one-shot-original contract
  for a tail replacement; a synchronous native→original→native continuation after the guest body
  returns is still unimplemented.
- **JIT/fallback counters**: `JitBase::Dispatch` block-cache hits, hook calls, originals, and
  invalidations already have typed counters in the pinned adapter and are proven live by
  `GcnPortRuntime.ShippingJitCacheHookOriginalAndInvalidation` (Dolphin's own gtest suite). Dolphin's
  existing `FallBackToInterpreter` calls remain untyped (raw instruction word, no reason enum) —
  this is the acceptance blocker for "no interpreter without a typed reason."

**Re-verified this session** (not reused stale numbers): `gcnport`'s non-runtime gate
(`tools/verify.py`, Clang 22.1.8/Ninja) passes; the existing `build/dolphin-runtime` test binary was
re-run directly and `GcnPortRuntime.ShippingJitCacheHookOriginalAndInvalidation` still passes,
proving live nonzero Dolphin-JIT execution (cold compile, cache hit, hook-triggered invalidation and
recompile, one-shot original body, invalidation) on Linux x64 through the pinned fork adapter. This
is a **synthetic redistributable PPC arithmetic/branch test program**, not exact `GMSE01` — the
acceptance bullet "exact GMSE01 boots with nonzero Dolphin JIT blocks" is still open.

Landed this session (commit `1404f6c` in `shared/gcnport`, not pushed): `verify_runtime` now
validates an exact, host-specific required-test inventory (17 POSIX x64 / 16 Windows x64 / 14 POSIX
arm64, via new `tools/gcnport_tools/dolphin_tests.py`) instead of substring-matching one hardcoded
test name, closing a discriminator that could previously go green with fewer tests silently
discovered or executed.

**What remains before this issue's acceptance bullets are met** (per gcnport's own
`docs/project-state.md` S003/S004, 6 of 11 `docs/dolphin-embedding-contract.md` operations still
absent):
- `RuntimeSession`/`BootAuthenticatedImage`/`ExecuteJitBlock`/`ExecuteRefusedBlock`/
  `ExecuteDiagnosticInterpreterBlock` as a public one-block adapter API gcnport's own C++ library
  (not just Dolphin's internal gtest) can call — this is what would let a title actually boot
  `GMSE01`.
- Typed `JitRefusalReason` fallback instrumentation replacing untyped `FallBackToInterpreter(inst)`.
- Synchronous native→original→native call continuation (current guard scheme only covers a tail
  replacement).
- A large (89-file) uncommitted Windows/cross-platform portability batch sits in the Dolphin fork
  submodule's own working tree (not committed to the fork, not pushed) blocked on linting
  `Source/Core/DolphinQt/Debugger/NetworkWidget.cpp` against real Qt6 headers; `qt6-qtbase-devel` is
  not installed on this host. Per project doctrine this needs the operator to run
  `sudo dnf install qt6-qtbase-devel`, not an agent-run privileged install. This blocks landing that
  batch upstream in the fork, not the Linux x86_64 evidence above (gcnport itself never enables Qt).
- Android arm64-v8a has no runtime boundary yet (S006 missing).

Not resolved. Do not treat the passing synthetic-image test or the committed test-inventory fix as
S001/issue-37 acceptance; both still require the public adapter API and an exact `GMSE01` boot.

## Acceptance

- `gcnport` owns image generations, runtime hooks, original calls, bounded exits, invalidation, and
  execution counters without title addresses.
- exact `GMSE01` boots with nonzero Dolphin JIT blocks;
- the runtime dispatcher reaches `J3DShape::draw` at `0x802e0390`, submits the existing semantic
  J3D value, and runs the original body through one-call override suppression;
- controls exercise hook hit/miss, enabled/disabled, cache hit/miss, chaining, and hook-change
  invalidation; and
- link/selector inspection proves the gameplay target includes neither an interpreter nor generated
  guest code.

This resolves the first wiring discriminator only. Representative gameplay is S008 and must pass
before S009 removes the old files.

---
id: 37
title: Migrate Sunbright execution to gcnport Dolphin dynarec
status: open
symptom: The intended native/dynarec product is not runnable: exact GMSE01 still lacks a complete gcnport/Dolphin-JIT boot path and a robust runtime J3DShape::draw hook at 0x802e0390.
state_items: S001,S002,S003
tags: migration,gcnport,dolphin,jit,override
created: 2026-09-04
updated: 2026-09-18
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

## Progress note (2026-09-18)

The public adapter API named above as the acceptance blocker is now real, at `shared/gcnport`
revision `961f0e7` (Dolphin fork revision `5a0d43d42e03f1dfe9bb5377ab90c3532da0e4cd`).
`PowerPC::GcnPort::RuntimeSession`, `BootAuthenticatedImage`/`ShutdownBootedImage`, `ExecuteJitBlock`,
`ExecuteRefusedBlock`, `ExecuteDiagnosticInterpreterBlock`, `InstallNativeHook`/`RemoveNativeHook`,
`ExecuteOriginalOnce`, `InvalidateGuestCode`, and typed `ExecutionCounters` (with
`JitRefusalReason`-classified fallback events, replacing the untyped `FallBackToInterpreter(inst)`
call) all exist as a public C++ facade callable from outside Dolphin's own gtest binary.
`tools/check_dolphin_contract.py` reports 0 of 11 required operations absent (was 6). Proven by
`GcnPortRuntime.PublicAdapterBootExecuteOriginalAndTypedFallback` plus the existing
`ShippingJitCacheHookOriginalAndInvalidation` scenario, both green, alongside the full 1,362-test
Dolphin suite on Linux x64/Clang with `-DENABLE_QT=OFF`.

Still open before this issue's acceptance bullets are met:
- The API has only booted a small synthetic redistributable PPC test image, never exact `GMSE01` —
  that boot attempt is Sunbright's own next step, consuming gcnport's now-real API rather than
  Dolphin internals directly.
- A synchronous native → original → native call continuation after the guest body returns is still
  missing; the current guard scheme only covers a tail replacement (a hook that never resumes native
  code after the guest call). The `J3DShape::draw` override needs this if its native body is meant to
  resume guest execution afterward rather than fully replacing it.
- Android arm64-v8a has no runtime boundary yet (S006 missing in gcnport's own project-state).
- A separate, previously-uncommitted Windows-portability batch is now landed in the same fork commit;
  the earlier note here that it was blocked on `qt6-qtbase-devel` was wrong — `DolphinQt` (including
  the touched file) only builds under `ENABLE_QT`, which gcnport never sets, so no gate this project
  runs was actually blocked by that missing package.

Separately: a prior session investigating issue 11 (statue graffiti stripes) reconstructed the
retired executor's build recipe to get something bootable for diagnosis. That reconstruction has
been deleted and its code changes reverted — this issue's own acceptance text already says not to
revive removed executor artifacts "as a migration bridge, oracle, or comparison arm," and that applies
project-wide, not only to this issue's own scope.

## Progress note (2026-09-18, continuation)

`shared/gcnport`'s synchronous native → original → native gap (the item directly above) is now
closed: `PowerPC::GcnPort::RuntimeSession::CallOriginalSynchronously` lands at fork revision
`8296bbe` (gcnport commit `aa60ab6`, both local/unpushed pending operator review), proven by
`GcnPortRuntimeTest.HookCallsOriginalSynchronouslyThenResumesNativeWork` plus the unchanged
1,362 existing tests (1,363 total). `tools/check_dolphin_contract.py` reports 0/12 absent. gcnport's
own `docs/project-state.md` now marks S003 `verified`. This is the last item the `J3DShape::draw`
override needs from gcnport's side of the contract; the override itself (S003 in *this* project's
state) is still not attempted — that remains separate follow-on work, not done this session.

**First real exact-`GMSE01` boot attempt** (this session, sunbright-only, no changes needed in
gcnport's own repo or tests — it never touches the ROM):

- Added `tools/gcnport_boot/gmse01_boot.cpp`, a standalone maintainer tool (uncommitted, left in the
  working tree for review) that parses a raw GameCube DOL (this session reused the already-extracted,
  gitignored `scratch/bin/sms.dol`, 4,128,928 bytes, matching this repo's real GMSE01 image via the
  `.env` `SUNBRIGHT_ROM` convention), assembles one contiguous flat image from the DOL's own section
  addresses (load address `0x80003100`, entry point `0x8000522c`, span 4,278,016 bytes), and calls
  gcnport's public `PowerPC::GcnPort::BootAuthenticatedImage`/`RuntimeSession::ExecuteJitBlock`
  exactly as a title consumer would. It was built and run standalone (not wired into the main
  `CMakeLists.txt`) by compiling and linking directly against the already-built
  `shared/gcnport/build/dolphin-runtime` static libraries — that build-wiring gap (a proper
  `add_subdirectory`/`FetchContent` integration into Sunbright's own CMake project) is still open;
  this session proved the API boundary works before investing in that wiring.
- **Result: 3 JIT blocks compiled and executed (all cold, 0 cache hits) from real GMSE01 code at
  the real entry point, before a hard fault.** This is nonzero Dolphin JIT block execution against
  the exact retail image, i.e. this issue's second acceptance bullet's literal condition is met, but
  only for 3 blocks before a crash — not a stable or complete boot.
- The fault is a SIGSEGV "invalid permissions for mapped object" inside JIT-generated code executing
  a real GMSE01 store instruction (`stwbrx`-shaped, x86 `movbel` to `(rbx,r13)`), diagnosed live via a
  narrow SIGSEGV handler in the tool that reads `RuntimeSession::GetExecutionCounters()` before
  re-raising (see the tool's `ReportCountersOnFault`). Register state at the fault (`rbx` = fastmem
  physical base, `r13` = the raw effective guest address `0x804277e8`, unmasked) points to a real,
  understood gap rather than a JIT correctness bug: `BootAuthenticatedImage` only calls
  `Memory::Init`/`CoreTiming::Init`/`CPU::Init` and sets PC/NPC (see
  `shared/gcnport/docs/dolphin-embedding-contract.md`, "deliberately scoped to a raw in-memory
  image"). It never runs the BS2/IPL-equivalent OS-init a real apploader performs before jumping to
  a DOL's `__start` — in particular MSR and the PPC BAT (block address translation) registers are
  left at their power-on-reset state, not the values the retail boot process configures. GMSE01's
  own `__start`/OS-init code executes far enough to compile and run 3 real blocks (this session also
  had to set `r1` to a computed top-of-RAM value first, since raw PC/NPC boot leaves GPRs zero and a
  real DOL entry assumes the apploader already gave it a valid stack pointer) before it reaches a
  store whose translated address, under the boot-time MSR/BAT state, lands in fastmem's unbacked
  guard region rather than real RAM.
- This is not a fastmem-arena wiring bug in gcnport (Jit64 owns calling `InitFastmemArena()` during
  its own `Init()`, invoked from `CPU::Init()`, which `BootAuthenticatedImage` already calls) and not
  something to patch around locally: correctly resuming past it needs an actual BS2/IPL-equivalent
  OS-init/apploader adapter, which `docs/dolphin-embedding-contract.md` already names as "a title's
  real GameCube disc boot is a separate, later adapter" — out of scope for this session's two gaps.
- Remaining before this issue's second acceptance bullet is durably true (not just "3 blocks before a
  crash"): (1) a real CMake build-wiring path for Sunbright to link `shared/gcnport`'s built Dolphin
  fork without hand-assembled compiler/linker flags; (2) an OS-init/apploader adapter (BS2-equivalent
  MSR/BAT/stack setup, either through Dolphin's own `BootParameters::Disc`+apploader path exposed
  through a future gcnport adapter, or a narrower hand-authored GMSE01-specific init) so boot survives
  past early hardware/OS bring-up. The `J3DShape::draw` override attempt (this issue's third
  acceptance bullet) is blocked on both of these and was not attempted this session.

## Progress note (2026-09-18, second continuation)

Closed both concrete gaps this issue's previous note left open, in order:

**CMake build wiring** — `extern/gcnport` is now a real pinned git submodule (gcnport `bf6dc3c`,
Dolphin fork `a188e7b0`), wired through `cmake/GcnPortDependency.cmake` (an `EXCLUDE_FROM_ALL`
`add_subdirectory` of the pinned Dolphin fork, mirroring `shared/gcnport`'s own verified build
options) and `tools/gcnport_boot/CMakeLists.txt`. `cmake --build build --target
sunbright_gcnport_boot` now produces a working binary from a fresh submodule checkout with no
hand-assembled compiler/linker flags. Two real integration issues surfaced and were fixed at their
cause rather than special-cased: Dolphin's own CMake selects C++23 and its architecture macros
(`_M_X86_64`/`_ARCH_64`) through directory-scoped `set()`/`add_definitions()`, which do not reach a
sibling directory's target through `target_link_libraries()`, so this directory mirrors that exact
selection (detected by `CMAKE_SYSTEM_PROCESSOR`, not hardcoded); and `core` requires frontend
`Host_*` callback definitions, resolved by compiling in Dolphin's own reusable
`Source/UnitTests/StubHost.cpp` rather than writing a Sunbright-local copy.

**OS-init/apploader adapter (option b: generic gcnport adapter)** — landed in `shared/gcnport`
(commit `bf6dc3c`, Dolphin fork `a188e7b0`, not pushed): `BootAuthenticatedImage` gained an
`apply_gamecube_os_init` parameter (default `false`) that calls a new public
`CBoot::SetupGameCubeBS2Registers`, itself a thin wrapper around Dolphin's own existing
`CBoot::SetupMSR`/`SetupHID`/`SetupBAT` — the exact register setup `CBoot::EmulatedBS2_GC` performs
before jumping to a disc's DOL entry point. This is title-neutral (standard GameCube BS2 behavior,
no GMSE01-specific values) and reuses Dolphin's own maintained implementation rather than
duplicating it. Two new `GcnPortRuntimeTest` cases prove the flag installs the exact retail MSR/BAT
values and that the default leaves every existing caller unchanged; both are now part of the
required regression inventory (which also gained three previously-unlisted-but-passing tests from
earlier sessions, bringing it to 22/21/19 tests on POSIX x64/Windows x64/POSIX arm64). Full
`tools/verify.py --runtime` passes.

`gmse01_boot.cpp` now passes `apply_gamecube_os_init=true` and no longer manually guesses a stack
pointer: `decomp/sms/src/dolphin/os/__start.c`'s `__init_registers` shows GMSE01's own linked
`__start` sets `r1`/`r2`/`r13` itself from the DOL's own linked `_stack_addr`/`_SDA2_BASE_`/
`_SDA_BASE_` immediates before any memory access, so a caller-supplied guess was redundant.

**Result**: 96 real JIT blocks compiled and 7,511 total block executions (7,415 cache hits) from the
real `GMSE01` entry point, up from 3. The previous real-mode/BAT fault is gone. A **new**, precisely
diagnosed fault occurs further into boot: SIGSEGV inside `MMIO::WriteHandler<u32>::Write`
(`Source/Core/Core/HW/MMIO.cpp:379`, gdb backtrace captured), writing to physical address
`0x0C003004` (GameCube `ProcessorInterface` MMIO range) through an uninitialized/garbage handler
function pointer — because `BootAuthenticatedImage` never calls Dolphin's `HW::Init()`, so no
`MMIO::Mapping` handler table exists for any hardware register. This is GMSE01's own
`__init_hardware` performing real hardware bring-up. Next step: either extend the same
`apply_gamecube_os_init`-style option to also invoke `HW::Init()` (broader side effects — video/
audio backend selection, DSP, EXI wiring — deserving its own scoped investigation), or add a native
override for GMSE01's hardware bring-up path before this point. Not attempted this session.

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

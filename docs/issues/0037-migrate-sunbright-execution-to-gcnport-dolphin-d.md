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

## Progress note (2026-09-18, third continuation)

Closed the exact blocker the previous note left open (SIGSEGV inside `MMIO::WriteHandler<u32>::Write`
writing to physical `0x0C003004`, GameCube `ProcessorInterface`, because `BootAuthenticatedImage`
never called Dolphin's `HW::Init()`). Landed in `shared/gcnport` (commit `392f0e8`, Dolphin fork
`fe8183e`, both pushed to origin — verified with the full 1,367-test Dolphin suite and
`tools/verify.py`; see that repo's own `docs/dolphin-embedding-contract.md`, "GameCube hardware
bring-up (MMIO handler table)" for the full investigation): `BootAuthenticatedImage` gained a second,
independent option,
`apply_gamecube_hardware_init`, which calls Dolphin's own maintained `HW::Init`/`HW::Shutdown`
instead of this function's minimal `Memory`/`CoreTiming`/`CPU` bring-up. `HW::Init` builds the
`MMIO::Mapping` handler table (`MemoryManager::InitMMIO`) for every GameCube hardware register, and
constructs no host video/audio/input backend of its own; three real dependencies it does pull in
(a `SoundStream` object for `AudioInterfaceManager::Init`, default EXI/SI device attachment, and
fastmem's SIGSEGV-based MMU slow path for addresses with no backing page) are resolved by forcing
Dolphin's own maintained `NullSound` backend, forcing `EXIDeviceType::None`/`SIDEVICE_NONE` on both
EXI card slots and every SI channel, and installing `EMM::InstallExceptionHandler()` — all ordinary,
real hardware/software states, not a workaround. Proven by two new `GcnPortRuntimeTest` cases,
including an `EXPECT_DEATH` negative control that proves the exact same access crashes without the
flag; full 1,367-test Dolphin suite passes unchanged.

`gmse01_boot.cpp` now passes `apply_gamecube_hardware_init=true` alongside the existing
`apply_gamecube_os_init=true`, and `extern/gcnport`'s pin is bumped to the pushed commit above.

**Result: the ProcessorInterface fault is gone. Boot now reaches 106 real JIT blocks compiled and
7,524 total block executions (7,418 cache hits), up from 3 then 96.** The bounded diagnostic dispatch
loop (`MAX_BLOCKS`, raised from 4096 to 16384 to reach and stably reproduce the new fault instead of
stopping mid-loop) shows GMSE01 spending its first ~6,000 one-block dispatches in two tight polling
loops at `0x80003194` (24 instructions) and `0x8000320c` (5 instructions) — real hardware-register
busy-waits that now resolve and exit on their own once `HW::Init`'s device state is present, rather
than spinning forever or crashing — before reaching genuinely new code around `0x80343774`/
`0x80341eec`.

**A new, precisely diagnosed fault occurs there**: SIGSEGV inside `Jit64::SingleStep`
(`Source/Core/Core/PowerPC/Jit64/Jit.cpp:810`, gdb backtrace captured) with a live register
(`r14 = 0xcc00500a`) holding the exact GameCube uncached-MMIO effective address `0xCC00500A` —
physical `0x0C00500A`, an offset into the DSP interface's own MMIO range (`DSPManager::RegisterMMIO`
registers at physical base `0x0C005000`, see `Source/Core/Core/HW/DSP.cpp`). Unlike the
`ProcessorInterface` fault, the DSP's MMIO handler table entry now exists (`HW::Init` already calls
`system.GetDSP().Init(...)` and registers it); this is not a repeat of the same missing-handler class
of bug. `dolphin-embedding-contract.md`'s own hardware-bring-up section already names DSP LLE/HLE
thread startup (`DSPEmulator::Initialize()`, a separate call Dolphin's own `EmuThread` makes after
`HW::Init`, deliberately not part of this option) as intentionally out of scope; the crash is
consistent with GMSE01's DSP register access reaching a DSP emulator object that exists
(`DSPManager::Init` constructs it) but was never `Initialize()`'d, unlike a from-scratch investigation
this session did not have time to fully confirm against the interpreter frames above `SingleStep` in
the backtrace (unresolved `??` frames, consistent with interpreted/JIT-generated code rather than a
missing debug symbol issue).

Not attempted this session: whether the correct fix is (a) extending `apply_gamecube_hardware_init`
or a new sibling option to also call `DSPEmulator::Initialize()` in DSP-HLE mode (the same kind of
scoped investigation `HW::Init` itself needed — DSP HLE's `Initialize()` may itself assume a
`CoreTiming`/thread context this bare adapter does not fully match), or (b) a narrower native
override for GMSE01's specific DSP register poll before this point. This is `gcnport`'s next scoped
hardware-bring-up gap, tracked there rather than duplicated as a Sunbright-owned fix, matching how the
`ProcessorInterface` gap was resolved.

## Progress note (2026-09-18, fourth continuation)

The previous note's suspected DSP bring-up gap in `gcnport` was investigated and **falsified**: there
is no `gcnport` DSP MMIO gap, and no `shared/gcnport` change was needed or made this session.

Root cause, found by reproducing the exact fault under gdb and reading Dolphin's own fault-handling
source end to end (`Source/Core/Core/MemTools.cpp`, `Source/Core/Core/PowerPC/Jit64/Jit.cpp`): the
crash was entirely a bug in this repository's own diagnostic tool,
`tools/gcnport_boot/gmse01_boot.cpp` (uncommitted, maintainer-owned). `PowerPC::GcnPort::
BootAuthenticatedImage`'s `apply_gamecube_hardware_init=true` path correctly calls Dolphin's
`EMM::InstallExceptionHandler()`, which installs a `sigaction`-based SIGSEGV handler
(`sigsegv_handler`) that is Dolphin's own normal, working mechanism for servicing an unbacked-fastmem
hardware-register access: on fault it calls `JitInterface::HandleFault`, which backpatches the
JIT-generated load/store into a slow C++ MMIO call and resumes; only a genuinely unhandled fault falls
through to the previously-installed handler. `gmse01_boot.cpp`'s own diagnostic crash reporter was
installed via a bare `std::signal(SIGSEGV, ReportCountersOnFault)` call issued *after*
`BootAuthenticatedImage` returned. Because `signal()` and `sigaction()` share one per-process
disposition slot, that call silently discarded Dolphin's already-installed `sigsegv_handler` outright
— every subsequent SIGSEGV, including the ordinary, recoverable fastmem MMIO backpatch faults that
had already served thousands of earlier `ProcessorInterface`/DSP-mailbox polling-loop accesses without
incident, was instead delivered straight to the diagnostic reporter and treated as fatal. The first
access this broke was the DSP_CONTROL register read at physical `0x0C00500A` (`Source/Core/Core/HW/
DSP.cpp`'s `DSP_CONTROL = 0x500A`), simply because it happened to be the first previously-unexecuted
code path to touch a not-yet-backpatched fastmem address after the handler was clobbered — not because
`DSPManager`/`DSPHLE` state was actually missing (`HW::Init` already constructs a live `DSPEmulator`
via `DSPManager::Init`; `DSPHLE::DSP_ReadControlRegister()` needs no separate `Initialize()` call to
answer that read safely).

Fix: reorder `gmse01_boot.cpp` to install its diagnostic `std::signal(SIGSEGV, ReportCountersOnFault)`
handler *before* calling `BootAuthenticatedImage`, so `EMM::InstallExceptionHandler()` saves it as the
chain-to fallback instead of overwriting it. Dolphin's own handler now correctly services every
fastmem MMIO fault, including the DSP_CONTROL access, and the diagnostic reporter only fires for a
truly unhandled fault (exactly its intended purpose). Verified: with the reorder, exact `GMSE01`
boots to this tool's full 16,384-block bound with **zero crashes** — up from crashing at block ~6400 —
compiling 276 unique JIT blocks and executing 17,485 total block dispatches (17,209 cache hits, 91
fallback/interpreted-instruction events), vs. 106 compiled / 7,524 executions before. A second,
temporary run raising the bound to 400,000 blocks (401,101 executions) confirmed this is a genuine
steady state, not a slow crawl toward another crash: execution settles into one stable, unchanging
busy-wait loop at guest PC `0x80343484` (9 PPC instructions) with no new blocks compiled and no faults.

That was read at the time as the honestly-diagnosed next blocker — GMSE01 polling a hardware
condition (DSP mailbox/interrupt, ARAM DMA completion, or VI retrace) that a bare in-memory-image
adapter boot could never satisfy, since it runs no DSP LLE/HLE thread, delivers no hardware
interrupts, and drives no real frame timing. **The fifth-continuation note below falsifies that
reading.** The loop was waiting on an ARAM DMA completion interrupt that `gcnport` had scheduled
correctly and then made unreachable by freezing `CoreTiming`'s global timer; it clears as soon as the
timer advances. The lesson is recorded rather than the conclusion: "the run settles into an unchanging
steady state" was taken as evidence about the *title's* requirements when it was evidence about our
*own* timekeeping, and the cheap falsifier — reading the global timer across dispatches — had not been
run.

## Fifth continuation (2026-09-18): CoreTiming freeze fixed; boot reaches the DVD boundary

`RuntimeSession::ExecuteJitBlock` forced `ppc_state.downcount = 1` before each dispatch, on the
assumption that this was what bounded a call to a single block. `CoreTiming::Advance()` — which
Dolphin's own generated dispatcher calls on entry — derives elapsed guest time from exactly that
field:

    cyclesExecuted = slice_length - DowncountToCycles(downcount)

so on entry `downcount` must still be the previous slice's natural remainder. Once a
one-block-at-a-time caller settled into `slice_length == 1`, the forced sentinel made this
`1 - 1 == 0` and the global timer stopped advancing for good: **measured stuck at 30,891 ticks across
16,384 consecutive dispatches**, with the invariant `ticks + downcount == 30,888` holding throughout.
No scheduled `CoreTiming` event could ever come due, so the ARAM DMA completion interrupt
(`INT_ARAM`, `DSP_CONTROL` bit `0x20`) that `DSPManager::Do_ARAM_DMA` had scheduled just 246 ticks
ahead was never raised, and `__OSInitAudioSystem`'s poll — `decomp/sms/src/dolphin/os/OSAudioSystem.c`
spins on `while (!(__DSPRegs[5] & 0x20))` — could never exit. The sentinel never bounded anything
either: `Advance()` reassigns `downcount` from the event queue before the first block runs.

Fix (Dolphin fork `914365a`, `gcnport` `0975e62`, both pushed): bound the **slice**, which is the
field `CoreTiming` actually sizes a dispatch by. A no-op event kept permanently one cycle in the
future makes `Advance()`'s own `slice_length = min(next_event.time - global_timer, ...)` cap every
slice at one block. `MAIN_ENABLE_DEBUGGING` was tried and rejected: it also produces a per-block
dispatcher exit, but additionally drives `analyzer.SetDebuggingEnabled`, forcing single-instruction
blocks (224 blocks over 16,384 dispatches) and destroying the block granularity this API exposes.
Follow-on: `SerialInterfaceManager`'s periodic poll — only reachable now that the timer advances —
calls `g_controller_interface.UpdateInput()` unconditionally, before and independently of asking any
SI channel for data, and asserts on `m_is_init`; forcing every channel to `SIDEVICE_NONE` does not
prevent the poll, because an SI poll with nothing plugged in is what real hardware does. A headless
`ControllerInterface::Initialize(WindowSystemInfo{})` gives that poll a real owner.

Two required regressions were added, each assertion an independent discriminator: the global timer
must strictly advance per dispatch (fails if `downcount` is overwritten); a block must hold more than
one instruction (fails under `MAIN_ENABLE_DEBUGGING`); and retired work must stay in the guest loop's
fixed ratio to block size, which pins "exactly one block ran" **without** hard-coding an analyzer
decision — the first version of that assertion hard-coded a 4-instruction block and failed when the
analyzer folded three iterations into a 12-instruction one. They were first written, compiled and
registered but **never executed**, because `tools/verify.py` runs an explicit `--gtest_filter`
inventory they were not in; a gate that does not run is not a gate. Verified after adding them:
`tools/verify.py --runtime`, linux/x64, 26 required tests. The timer regression was validated against
the defect by planting the removed `downcount = 1` write back into `ExecuteJitBlock`, where it fails
from dispatch 1 onward.

**Result: exact GMSE01 runs through OS bring-up with zero faults and stops at a correct, precisely
identified boundary — its DVD error screen.** Boot clears `__OSInitAudioSystem`, the RAM clear
(`0x80003194` → `0x81800000`), and `System/Application.cpp`'s two `OSProtectRange` calls. Those two
are worth recording, because they were briefly mistaken for a bogus 64 MB flush on a 24 MB console:
`OSProtectRange(0, nullptr, 0x80000000, 0)` and `OSProtectRange(1, (void*)0x83000000, 0x7d000000, 0)`
are deliberate whole-address-space protection setup, and `DCFlushRange` walks them 32 bytes at a time,
so they alone retire **131,334,144 iterations** of a single four-instruction block — measured within
1% of that figure. The registers that looked like a garbage length (`r3=0x40`, `r4=0x04000000`,
`ctr=0x03fffffe`) were the loop-advanced pointer and the post-`srwi` iteration count sampled two
iterations in, not the call's arguments. `CBoot::SetupGCMemory` (the BS2 low-memory OS globals
`gcnport` does not write) was the leading hypothesis and was **not** the cause; it remains unwritten
and is a separate question.

Boot then renders the SDK's disc-error path: the strings `"An error has occurred. Turn the power OFF
..."`, `"The Disc could not be read."` and `"Reading Disc..."` (all present in the DOL at `0x3a103c`),
drawn through the IPL font. That is what produces the run's "Trying to access Windows-1252 fonts"
notice and the stream of one-byte reads from addresses that are themselves ASCII codes (`0x21` `!`,
`0x4E` `N`, `0x45` `E`, `0x52` `R`, `0x4F` `O`, `0x48` `H`, `0x41` `A`, `0x53` `S`). This is the
expected result, not a defect: `BootAuthenticatedImage` places a flat DOL image in memory and
deliberately exposes no DVD volume, so the title's first disc access fails into the SDK's error path.
The previous 16,384-block bound could never have reached this, which is exactly why it read as a
permanent stall.

`tools/gcnport_boot/gmse01_boot.cpp` now takes an optional `[max-blocks]` argument (refusing a
malformed or zero value rather than silently substituting the default) and reports elapsed time and
block rate, with a default budget large enough to clear the 131.3M-iteration protection flush.

The batched execution entry point this called for landed in the same session
(`gcnport` `c8d4e93`, Dolphin fork `82087cb`). `RuntimeSession::ExecuteJitBlocks(minimum_blocks)`
lifts the one-block slice cap so the dispatcher chains direct-linked blocks natively. The cap is
**removed** for the batch rather than rescheduled further out: `CoreTiming` already sizes a slice from
the next genuinely scheduled event and caps it at its own `MAX_SLICE_LENGTH`, so with our event gone a
batch runs on exactly the slice lengths ordinary Dolphin execution uses, and picking some larger
interval would have invented a second, competing slice policy. Hardware events still bound every
slice, so hardware timing is unaffected. Speed is bought without going dark: blocks report themselves
from inside the generated code (`RecordJitBlockExecutionFromJit`), so the `ExecutionCounters` ledger is
exactly as complete batched as stepped, and the gated regression asserts the counters advance by
precisely the number of blocks the batch claims. It refuses a zero-block request and refuses to run
while a one-shot original ticket is armed — those are consumed by the per-dispatch driver loop, and a
batch has no per-dispatch boundary at which to consume one — and a slice that retires no block stops
the batch with a reported reason instead of spinning. An RAII scope restores the cap on every exit
path, which the test pins by requiring a following `ExecuteJitBlock` to advance the ledger by exactly
one.

Measured against exact `GMSE01`: **~9,900,000 blocks/second batched versus ~180,000 stepped through
the same `DCFlushRange` loop, reaching the disc boundary in ~34 seconds instead of over twelve
minutes.** `gmse01_boot.cpp` now steps the first 32 blocks for block-by-block legibility and batches
the remainder, keeping both paths exercised in one invocation. Throughput falls to ~40,000
blocks/second once boot is inside the disc-error screen — the guest spin-waiting on timers, since its
slices end at the next scheduled hardware event rather than at a block and each font read raises an
invalid-access report — which is a property of that wait, not of the runtime.

Next, in order: (1) a **disc/DVD device adapter**, which Sunbright owns because the game image must
never reach `gcnport`; (2) the `0x802e0390` `J3DShape::draw` runtime override, blocked on (1).

**2026-09-18 (sixth continuation): disc mounting landed; boot now idles in the OS scheduler waiting
for a hardware interrupt that is never raised.** `gcnport` `e9e6304` (Dolphin fork `7b2be56`) added
`GameCubeBootOptions::disc_image_path`, which mounts a disc the *consumer* names — only a path crosses
the API, so no game image enters `gcnport` — and does what `CBoot::EmulatedBS2_GC` does with a disc
before handing over control: read the 0x20-byte header to physical 0 via `CBoot::DVDReadDiscID` (which
also moves the drive out of `DiscIdNotRead`), then leave the volume mounted. The two trailing bools
became a `GameCubeBootOptions` struct in the same change, with every caller migrated.

Mounting the real disc measurably changed GMSE01's behaviour: the repeated one-byte reads from
ASCII-valued addresses — the disc-error text being drawn through the IPL font — **fell from 822 to 3**,
and the title no longer renders that screen.

The remaining stop is **not** an error path. `0x80348814` is inside `SelectThread`, and the SDA global
it spins on (`r13-0x59c0` = `0x8040e800`, `.sbss`) is `__OSRunQueueBits`; the surrounding code writes
`__OSCurrentThread = NULL` at `0x800000E4` and brackets the spin with `OSEnableInterrupts` /
`OSDisableInterrupts`. That is the SDK's **idle loop**: every thread is blocked, and the scheduler is
waiting with interrupts enabled for one to become runnable.

Measured at that point, through the owning device objects: `disc_inside=1`, `msr=0x00009032` (EE set),
`exceptions=0x00000000`, `pi_mask=0x00000ffc` (the title has unmasked DI, SI, EXI, AI, DSP, MI, VI, PE
and CP), and `pi_cause=0x00010000` — bit 16, the reset-button state, and **no pending hardware
interrupt at all**. Guest time is not the constraint: the tick counter advances ~12,000,000,000 ticks
per report, about 1,480 VI retraces' worth each at 486MHz/60, and roughly 8,700 frames in total. So the
title has enabled interrupts, unmasked the sources it needs, and waited the equivalent of minutes of
console time without a single one arriving.

That is the boundary `dolphin-embedding-contract.md` already scopes and this option deliberately does
not provide: `HW::Init` builds the MMIO handler table and `SystemTimers::Init` schedules the periodic
events, but Dolphin's own `EmuThread` makes `g_video_backend->Initialize` and `DSPEmulator::Initialize`
*around* `HW::Init`, and neither is called here. Next: bring those up in their headless forms — the
Null video backend takes a `WindowSystemInfo` exactly as the `ControllerInterface` fix does — so VI
retrace and DSP interrupts exist for the scheduler to wake on.

Tooling note: the first version of this stall reporter read the MMIO addresses through
`Memory::Read_U32`, which does not serve MMIO and answered every field with "Invalid range in
CopyFromEmu" and a zero — indistinguishable from a genuinely quiet interrupt controller. It now reads
the owning device objects, and reports the cumulative tick count alongside the instantaneous cause,
because a handler that has already run leaves cause and exceptions at zero too.

**2026-09-18 (seventh continuation): the idle loop was a missing FIFO consumer, not a missing
interrupt.** The sixth-continuation reading above -- "no hardware interrupt is ever raised" -- was
wrong, and wrong in a way worth recording: `pi_cause` and `Exceptions` are instantaneous samples, so
an interrupt that was raised, handled and cleared leaves both at zero and is indistinguishable from
one that never happened. The falsifier was a cumulative guest-side counter. `VIGetRetraceCount`
(0x803504ec) is a single `lwz r3, -0x58f0(r13)`, which resolves through SDA1 to the SDK's own
`retraceCount` at 0x8040e8d0; it only advances when a VI interrupt is raised AND dispatched into the
title's handler. It advances by 1,480 per report interval -- exactly the number predicted by the
~12,000,000,000 guest ticks each interval covers at 486MHz/60. VI delivery works end to end.

Walking `__OSActiveThreadQueue` (0x800000DC) and each thread's saved stack named the real blocker.
Six threads are WAITING and one is MORIBUND; the main thread's back-chain resolves to:

    OSSleepThread <- GXDrawDone <- THPPlayerDrawDone <- TApplication::mountStageArchive

`GXDrawDone` writes a draw-done token and sleeps until the PixelEngine finish interrupt reports the
GPU has drained past it. Nothing was consuming the GP FIFO, so no such interrupt could ever be
raised. The other five threads (`JKRAram::run`, `JKRAramStream::run`, `JKRDecomp::run`,
`JUTException::run`) are JKernel service threads idling on their own work queues, which is normal.

The fix is `gcnport` `6547c1c` (Dolphin fork `2a69de5`): `GameCubeBootOptions::apply_media_init`
brings up the two consumers Dolphin's own `EmuThread` initializes around `HW::Init` -- the video
backend, pinned to Dolphin's maintained headless Null backend, and the DSP emulator, whose ucode
`DSPManager::Init` constructs but never boots -- followed by `Fifo::Prepare`. It pins single core,
declaring the calling thread as the GPU thread, because `ExecuteJitBlock`'s "exactly one observable
block on the calling thread" contract cannot hold if a separate GPU or DSP thread retires
guest-visible work.

Two instrument defects were found and fixed while getting there, both of the silent-success shape.
The first version of the thread walker read `state` as the low half of the big-endian word at 0x2C8,
which is `attr`, and so reported five threads as READY while the scheduler idled -- a contradiction
that belonged to the instrument, not the guest. And `tools/re/dataref.py` is new: it names the code
that references a guest DATA address (the inverse of `addr2sym.py`) by scanning for the `lis` plus
displacement pairs that form it. It was validated against a known positive -- `__OSCurrentThread` at
0x800000e4, which it finds in exactly the six SDK functions that touch it -- before its zero answers
for the wait-queue addresses were trusted; those queues are fields inside objects, not globals.

**Result of enabling `apply_media_init` against exact GMSE01: boot leaves the idle loop and the next
blocker is the skipped apploader.** The main thread is no longer asleep in `GXDrawDone`; it is
RUNNING, and its stack resolves to

    TApplication::mountStageArchive -> MSound::startSoundSet -> JAIBasic::initInterface
      -> JAIBasic::initInterfaceMain -> JAIBasic::initAllocParameter -> JAIData::initData

which is direct confirmation that the missing FIFO consumer was the exact wait that had stopped it.

What it runs into next is `DVDConvertPathToEntrynum+0x2b0` reading from address 0x00000008 -- a null
file system table. `CBoot::DVDReadDiscID` reads only the 0x20-byte disc header; the FST is loaded by
the **apploader**, which also publishes its low-memory pointers, and a `BootAuthenticatedImage` boot
of a pre-extracted DOL never runs one (`CBoot::EmulatedBS2_GC` ends in `CBoot::RunApploader`, which
gcnport does not call). Every file lookup therefore fails, `JKRArchive::findDirectory` and
`findFsResource` dereference null archives, and the title proceeds on garbage pointers -- which is
what scribbles over the `OSThread` objects and over PI_INTMR, both of which read as float-shaped junk
in later samples. Those are consequences of the null FST, not separate defects.

Next: load the disc's file system the way a console does, so `DVDConvertPathToEntrynum` has an FST to
walk. The apploader is guest code on the user's own disc and Dolphin already drives it through
`CBoot::RunApploader`, so exposing it is the faithful route rather than hand-publishing FST pointers.

Tooling landed with this: the boot tool now registers a non-interactive `MsgAlertHandler`. Dolphin's
default handler prompts on stdin, so an MMIO assertion turned a 250M-block run into a silent hang
waiting for an answer nobody was there to give. Alerts are now printed once each, counted, and
reported in the summary as `dolphin_alerts=N`, because an assertion is exactly the kind of finding
this tool exists to surface and silencing it would be worse than the hang.

# Project state

Factual capability ledger for Sunbright. Epic intent is `docs/project-goals.md`; architecture and
ordering are `docs/architecture.md` and `docs/port/migration.md`; atomic work is `docs/issues/`.

## Comparison baseline

The user-visible baseline is the unmodified NTSC-U GameCube release (`GMSE01`) running on original
hardware or Dolphin with console execution, GX rendering, 4:3 framing, and normally 30 Hz
presentation. The repository's former gameplay executor is absent; Sunbright
currently has no gameplay executable while its shared runtime executor is missing.

## Current focus

S004, the PC-native renderer, is the next focus: the execution path underneath it now runs GMSE01's
whole attract cycle unattended and faultlessly (S008 below has the retrace-timed trajectory), and
the run presents no frame because it uses Dolphin's Null video backend. Nothing further about the
title's behaviour can be seen without a renderer. Its producer seam now reads, decodes and poses the
title's own geometry with the two halves proven to agree; what remains is publishing a `ModelDraw`,
which needs the material and texture state that has not been read yet.

S003 is `partial` rather than `missing`: Sunbright installs its hooks
through `gcnport::DolphinRuntimeAdapter`, and both original-call forms are proven on the real title
-- 1,428 complete native -> original -> native round trips through `TApplication::drawDVDErr`, with
the native caller reading the body's own return value each time. What remains is a native override
that replaces guest behaviour rather than observing it; every hook Sunbright installs today is
diagnostic.

## Capability inventory

| ID | Capability / observable outcome | State | Dependencies | Goals |
| --- | --- | --- | --- | --- |
| S001 | Exact `GMSE01` boots under `gcnport`/Dolphin JIT and reaches the `J3DShape::draw` runtime hook at `0x802e0390` | verified | S002, S003 | G003, G004 |
| S002 | `gcnport` supplies a title-neutral Dolphin dynarec executor with image identity, bounded exits, invalidation, and diagnostics | partial | — | G003 |
| S003 | Sunbright native overrides and original calls use robust image-scoped runtime dispatch | partial | S002 | G003, G004 |
| S004 | The PC-native semantic renderer covers the complete visible J3D/J2D/particle/effect stream | partial | S003 | G004 |
| S005 | Native decomp adapters and recovered source provide independent semantic and behavior evidence | partial | — | G002, G004 |
| S006 | Smooth presentation covers every eligible moving source and keeps native-rate modes separate | partial | S001, S004 | G001 |
| S007 | Reached decomp behavior is upstream-converged, named, and implemented from evidence | partial | — | G002 |
| S008 | The native/dynarec product passes representative interactive gameplay conformance | missing | S001, S004, S011, S016 | G001, G003, G004 |
| S009 | Offline generator, emitted corpus, static dispatcher/runtime glue, tests, and launch paths are absent | verified | — | G003 |
| S010 | Independent Dolphin/decomp/binary oracle evidence can locate first divergence and prove controls | verified | — | G002, G003 |
| S011 | Native audio is complete and integrated with the native/dynarec gameplay product | partial | S001 | G003, G005 |
| S012 | Application lifecycle, typed configuration, Lucent logging, and structure boundaries are mechanically enforced | partial | — | G005 |
| S013 | Zero-argument launcher provisions and runs only the native/dynarec product from a user-supplied image | blocked | S002, S008, S009, S012 | G003, G005 |
| S017 | JIT gameplay is qualified independently on x86_64, Apple Silicon macOS AArch64, and Android arm64-v8a | missing | S008 | G003, G005 |
| S018 | Asset-free automation verifies the redistributable native renderer and tooling on supported hosts | partial | S012 | G005 |
| S014 | Desktop packages provide no-terminal first-run setup and contain no game content | missing | S013 | G005 |
| S015 | Widescreen renders additional world coverage through projection/viewport/scissor ownership | partial | S001, S004 | G001, G004, G005 |
| S016 | Native input, controls, saves, and settings work through one typed product policy | partial | S001, S012 | G003, G005 |

## Capability details

### S001 — first dynamic title discriminator

Partial capability: `tools/gcnport_boot/gmse01_boot.cpp` authenticates the exact retail `GMSE01`
main.dol (gitignored `scratch/bin/sms.dol`, load
address `0x80003100`, entry point `0x8000522c`) through gcnport's public
`BootAuthenticatedImage`/`ExecuteJitBlock` adapter.

Two of this item's three remaining gaps from the previous session are now closed:

1. **Real CMake build wiring.** `extern/gcnport` is now a pinned git submodule (at gcnport
   `185d616`, Dolphin fork `f5e7b38e16`), and `tools/gcnport_boot/CMakeLists.txt` +
   `cmake/GcnPortDependency.cmake` wire it as a real, `EXCLUDE_FROM_ALL` CMake subdirectory: the
   `sunbright_gcnport_boot` target links Dolphin's own `core`/`uicommon` targets and builds
   correctly via plain `cmake --build build --target sunbright_gcnport_boot` (verified end to end
   on Linux x86_64/Clang/Ninja from a fresh submodule checkout). It is excluded from the default
   `all` target so an ordinary product build never drags in the whole Dolphin fork. Two real,
   non-hand-tuned issues had to be fixed along the way, both documented in the CMakeLists' own
   comments: Dolphin's own `CMakeLists.txt`/`Source/CMakeLists.txt` select C++23 and
   `_M_X86_64`/`_ARCH_64` etc. through directory-scoped `set()`/`add_definitions()` calls that do
   not propagate to a sibling directory's target through `target_link_libraries()`, so this
   directory mirrors the same selection explicitly (by architecture detection, not a hardcoded
   single arch); and `core` calls back into frontend `Host_*` functions the tool has no GUI to
   provide, resolved by compiling Dolphin's own reusable, frontend-neutral
   `Source/UnitTests/StubHost.cpp` (the same file Dolphin's own gtest binary uses) into the tool
   instead of writing a bespoke Sunbright copy.
2. **OS-init/apploader gap (partially closed).** `shared/gcnport` gained a title-neutral
   `apply_gamecube_os_init` parameter on `BootAuthenticatedImage` (default `false`) that applies
   the exact retail GameCube MSR/HID/BAT register setup `CBoot::EmulatedBS2_GC` performs before a
   disc boot's DOL entry (`CBoot::SetupGameCubeBS2Registers`, reusing Dolphin's own
   `SetupMSR`/`SetupHID`/`SetupBAT` — see `shared/gcnport/docs/dolphin-embedding-contract.md`).
   `gmse01_boot.cpp` now passes this flag and no longer manually guesses a stack pointer: decomp
   evidence (`decomp/sms/src/dolphin/os/__start.c`'s `__init_registers`) shows GMSE01's own linked
   `__start` sets `r1`/`r2`/`r13` itself from the DOL's own `_stack_addr`/`_SDA2_BASE_`/
   `_SDA_BASE_` immediates before any memory access, so a caller-guessed value was an unnecessary
   bandaid.

   Result: **96 real JIT blocks compiled and 7,511 total block executions (7,415 cache hits, 96
   cold) from the real entry point**, versus 3 blocks before this session — the previous
   real-mode/BAT fault is gone. A **new, distinct, and precisely diagnosed** fault now occurs
   further into boot (gdb backtrace, `scratch/gdb_backtrace.log`): a SIGSEGV inside
   `MMIO::WriteHandler<u32>::Write` (`Source/Core/Core/HW/MMIO.cpp:379`), reached from
   `PowerPC::MMU::WriteToHardware` while writing `val=240` to physical address `0x0C003004`
   (GameCube `ProcessorInterface` register range) — a real hardware MMIO write GMSE01's own
   `__init_hardware` performs, landing on an uninitialized/garbage `m_WriteFunc` function pointer
   because `BootAuthenticatedImage` never calls Dolphin's `HW::Init()` (it only calls
   `Memory::Init`/`CoreTiming::Init`/`CPU::Init`, so no `MMIO::Mapping` handler table for
   VI/PI/MI/DSP/DI/SI/EXI/AI is ever registered). This is the next concrete OS-init gap: either a
   further `apply_gamecube_os_init`-style option that also runs `HW::Init()` (title-neutral, same
   pattern as the register fix), or an explicit decision that MMIO emulation is out of scope for a
   raw in-memory image boot and GMSE01's own hardware bring-up path needs a native override before
   this point. Not attempted this session — a real HW::Init() call has broader side effects
   (video/audio backend selection, DSP, EXI device wiring) that deserve their own scoped
   investigation rather than a same-session follow-on patch.

**2026-09-18 (third continuation): the `HW::Init()` MMIO gap above is closed.** `shared/gcnport`
gained a second, independent `BootAuthenticatedImage` option, `apply_gamecube_hardware_init` (commit
`392f0e8`, Dolphin fork `fe8183e`, both pushed to origin and independently verified (full 1,367-test
Dolphin suite, `tools/verify.py`) — see that repo's own
`docs/dolphin-embedding-contract.md`), which calls Dolphin's own maintained `HW::Init`/`HW::Shutdown`
(building the `MMIO::Mapping` handler table for every GameCube hardware register) while forcing
`NullSound`, "no memory card", "no controller", and installing the fastmem SIGSEGV handler so no host
video/audio/input backend or host-disk side effect is pulled in. `gmse01_boot.cpp` now passes this
flag too. Result: the `ProcessorInterface` fault is gone; boot now reaches **106 real JIT blocks
compiled and 7,524 total block executions (7,418 cache hits)**, up from 96/7,511, and progresses
through two real hardware-register polling loops (`0x80003194`, `0x8000320c`) that now resolve
instead of crashing. A **new, distinct** fault occurs deeper in boot: SIGSEGV inside
`Jit64::SingleStep` with a live register holding effective address `0xCC00500A` (physical
`0x0C00500A`, the DSP interface's own MMIO range) — its handler table entry already exists (`HW::Init`
already registers it), so this is not a repeat of the missing-handler class of bug; it is consistent
with GMSE01 reaching a DSP register access that needs `DSPEmulator::Initialize()` (deliberately not
called by this option, matching the documented DSP LLE/HLE-thread-startup scope boundary) or a
narrower native override before this point. See issue 37's third-continuation note for the full gdb
evidence; not attempted this session.

**2026-09-18 (fourth continuation): the "DSP MMIO gap" above was a misdiagnosis, now falsified and
fixed — no `shared/gcnport` change was needed.** The SIGSEGV at `0xCC00500A` was caused by this
tool's own diagnostic crash reporter (`gmse01_boot.cpp`'s `std::signal(SIGSEGV,
ReportCountersOnFault)`) being installed *after* `BootAuthenticatedImage`, which silently replaced
Dolphin's own working `EMM::InstallExceptionHandler()` SIGSEGV handler (`signal()`/`sigaction()`
share one per-process disposition) — so the ordinary, recoverable fastmem MMIO backpatch fault for
DSP_CONTROL was fatally reported by the tool instead of serviced by Dolphin's normal mechanism.
Reordering the tool to install its handler *before* `BootAuthenticatedImage` (so Dolphin's handler
correctly chains back to it only on a genuinely unhandled fault) fixes it: exact `GMSE01` now boots
to this tool's 16,384-block bound with **zero crashes** (276 compiled blocks, 17,485 executions, 91
fallback events), up from crashing at block ~6400 with 106/7,524. A 400,000-block probe found
execution settling into one stable busy-wait loop at guest PC `0x80343484`. That was read at the time
as a permanent hardware-condition wait no adapter boot could satisfy; the fifth-continuation note
below falsifies that reading — the loop was waiting on an ARAM DMA completion interrupt that a
`gcnport` timekeeping defect prevented from ever being raised, and it clears once the defect is
fixed. See issue 37's fourth- and fifth-continuation notes.

**2026-09-18 (fifth continuation): the `0x80343484` "unsatisfiable hardware wait" was a `gcnport`
timekeeping defect, now fixed; boot clears OS bring-up entirely and stops at the DVD boundary.**
`RuntimeSession::ExecuteJitBlock` forced `ppc_state.downcount` to 1 before each dispatch, believing
that was what bounded a call to one block. `CoreTiming::Advance()` derives elapsed guest time from
exactly that field (`slice_length - DowncountToCycles(downcount)`), so once a one-block-at-a-time
caller settled into a one-cycle slice the sentinel made this `1 - 1 == 0` and the global timer stopped
advancing permanently — measured stuck at 30,891 ticks across 16,384 consecutive dispatches. No
scheduled `CoreTiming` event could come due, so the ARAM DMA completion interrupt
`DSPManager::Do_ARAM_DMA` had scheduled 246 ticks ahead was never raised and `__OSInitAudioSystem`'s
poll at `0x80343484` spun forever. The sentinel never bounded anything either: `Advance()` reassigns
`downcount` from the event queue before the first block runs. Fixed in the Dolphin fork (`914365a`)
by bounding the *slice* instead — a no-op event kept permanently one cycle in the future, so
`Advance()`'s own `slice_length = min(next_event.time - global_timer, ...)` caps every slice at one
block. `MAIN_ENABLE_DEBUGGING` was rejected as the bounding mechanism: it also exits per block, but
additionally drives the analyzer into single-instruction blocks, destroying the block granularity
this API exists to expose. The same revision brings up a headless `ControllerInterface` for
hardware-init boots, because `SerialInterfaceManager`'s periodic poll — only reachable once the timer
advances — calls `g_controller_interface.UpdateInput()` unconditionally and asserts on `m_is_init`.
Landed as `gcnport` `0975e62` with two new required regressions (the timer must strictly advance per
dispatch; a block must hold more than one instruction; retired work and block size must stay in the
guest loop's fixed ratio, which pins "exactly one block ran" without hard-coding an analyzer
decision), both added to the verifier's `--gtest_filter` inventory after the first attempt compiled
and registered them but never ran them. Verified: `tools/verify.py --runtime`, linux/x64, 26 required
tests; the timer regression was validated against the defect by planting the removed `downcount = 1`
write back, where it fails from dispatch 1.

With the timer advancing, exact `GMSE01` runs through OS bring-up **without a single fault** and stops
at a precisely identified, correct boundary. Boot clears `__OSInitAudioSystem`, the RAM clear, and
`System/Application.cpp`'s two `OSProtectRange` calls — which flush `0x80000000` and `0x7d000000`
bytes of address space, retiring 131,334,144 iterations of one four-instruction `DCFlushRange` block,
measured within 1% of that figure — and then renders GMSE01's **DVD error screen**: the SDK strings
`"An error has occurred. Turn the power OFF ..."`, `"The Disc could not be read."` and
`"Reading Disc..."`, drawn through the IPL font (which is what produces the run's "Trying to access
Windows-1252 fonts" notice and the stream of one-byte reads from addresses that are themselves ASCII
codes). This is the expected result, not a defect: `BootAuthenticatedImage` places a flat DOL image in
memory and deliberately exposes no DVD volume, so the title's first disc access fails into the SDK's
disc-error path. The earlier 16,384-block bound could never have reached this, which is exactly why it
read as a permanent stall.

The batched execution entry point that boundary called for has since landed
(`gcnport` `c8d4e93`, Dolphin fork `82087cb`): `RuntimeSession::ExecuteJitBlocks` lifts gcnport's
one-block slice cap so the dispatcher chains direct-linked blocks natively, removing the bounding
event outright rather than rescheduling it, so a batch runs on exactly the slice lengths ordinary
Dolphin execution uses and scheduled hardware events still bound every slice. Per-block accounting is
emitted from inside the generated code, so the counter ledger is unchanged by batching, and the gated
regression asserts the counters advance by precisely the number of blocks a batch claims and that the
one-block cap is restored afterwards. Measured against exact `GMSE01`: **~9,900,000 blocks/second
batched against ~180,000 stepped through the same flush loop, reaching the disc boundary in ~34
seconds instead of over twelve minutes.** `gmse01_boot.cpp` now steps the first 32 blocks for
block-by-block legibility and batches the remainder, exercising both paths in one invocation.
Throughput falls to ~40,000 blocks/second once boot is inside the disc-error screen; that is the guest
spin-waiting on timers (slices end at the next scheduled hardware event, and each font read raises an
invalid-access report), not a runtime defect, and is expected to go once a disc device exists.

`gcnport` `0a48184` (Dolphin fork `d24aef4`) additionally closed a quieter OS-init gap found while
investigating the above: `apply_gamecube_os_init` installed BS2's MSR/HID/BAT registers but not the
low-memory globals `CBoot::EmulatedBS2_GC` writes immediately afterwards. Those are read back by fixed
address — GMSE01's own decomp declares `__OSPhysicalMemSize AT_ADDRESS(0x80000028)` and
`__OSBusClock` at `0x800000F8`, and defines `OS_TIMER_CLOCK` as `__OSBusClock / 4`, so a zero bus
clock silently corrupts every tick and time conversion rather than failing. It now reuses Dolphin's
own `CBoot::SetupGCMemory`. The negative control earned its keep: the pre-existing
`if (apply_gamecube_os_init)` had no braces, so the added call ran unconditionally and the
defaults-off test caught it as "Unable to resolve write address 80000028". Re-measured against exact
`GMSE01`: **5,250 compiled blocks over 140,000,000 executions with zero faults**, stopping at the same
disc boundary — as expected, since correct OS globals do not conjure a disc.

The disc adapter landed as `gcnport` `e9e6304` (Dolphin fork `7b2be56`):
`GameCubeBootOptions::disc_image_path` mounts a disc the consumer names, so only a path crosses the
API and no game image enters `gcnport`. It reads the disc header via `CBoot::DVDReadDiscID` (which
also moves the drive out of `DiscIdNotRead`) and leaves the volume mounted, matching what
`CBoot::EmulatedBS2_GC` does before handing control to a title. Mounting the real disc dropped
GMSE01's disc-error text rendering from 822 one-byte font reads to 3.

Boot now stops in the SDK's **idle loop** rather than an error path: `0x80348814` is inside
`SelectThread` spinning on `__OSRunQueueBits`, with every thread blocked. Measured there:
`disc_inside=1`, MSR.EE set, no pending exception, `pi_mask=0x00000ffc` (DI, SI, EXI, AI, DSP, MI, VI,
PE, CP all unmasked) and `pi_cause=0x00010000` — no hardware interrupt pending — while the guest clock
advances ~12 billion ticks per report, roughly 8,700 VI frames in total. The title has enabled
interrupts and waited minutes of console time without one arriving.

The idle loop turned out not to be an interrupt-delivery failure. The SDK's own `retraceCount`
(0x8040e8d0, reached by `VIGetRetraceCount` at 0x803504ec) advances by 1,480 per report interval,
exactly matching the ~12 billion guest ticks each interval covers, so VI interrupts are raised and
dispatched into the title's handler end to end. Walking `__OSActiveThreadQueue` and each thread's
saved stack named the real blocker: the main thread sits in
`TApplication::mountStageArchive` → `THPPlayerDrawDone` → `GXDrawDone` → `OSSleepThread`, waiting for
a PixelEngine finish interrupt that cannot be raised while nothing consumes the GP FIFO. The other
five waiting threads are JKernel service threads idle on their own queues.

`gcnport` `6547c1c` (Dolphin fork `2a69de5`) adds `GameCubeBootOptions::apply_media_init`, which
brings up the two consumers Dolphin's `EmuThread` initializes around `HW::Init` — the headless Null
video backend and the DSP emulator, whose ucode `DSPManager::Init` constructs but never boots —
followed by `Fifo::Prepare`, pinned to single core so guest execution stays observable on the calling
thread.

With media init enabled, the main thread leaves the idle loop and runs on into
`MSound::startSoundSet` → `JAIBasic::initInterface` → `JAIData::initData`, confirming the missing
FIFO consumer was the exact wait that had stopped it. The next blocker is the skipped **apploader**:
`DVDReadDiscID` reads only the 0x20-byte disc header, while the FST and its low-memory pointers are
published by the apploader (`CBoot::EmulatedBS2_GC` ends in `CBoot::RunApploader`, which gcnport does
not call). `DVDConvertPathToEntrynum` therefore walks a null FST, every file lookup fails, and the
title proceeds on garbage pointers.

Disc file-system provisioning landed as `gcnport` `b084c70` (Dolphin fork `fbdda46`):
`GameCubeBootOptions::run_apploader` runs the mounted disc's own apploader, the code a console runs
between reading the disc header and entering a title, which loads the FST and publishes its
low-memory pointers. A second defect surfaced behind it — Dolphin resolves its IPL font substitutes
and DSP ROM through `File::GetSysDirectory()`, which nothing had pointed at the checkout's `Data/Sys`.
Together they take GMSE01 from **335,405,984 invalid guest accesses to zero** over the same 250M-block
budget (apploader alone: 19, all in the SDK's font path).

Still missing before this item is complete: (1) a host fault inside Dolphin's Jit64 register
allocator (`RegCache::Realize`), deterministic at guest tick 935,443,084 and only with the apploader
enabled — a fault during block compilation, not guest execution, with no invalid guest accesses left;
(2) the `0x802e0390` `J3DShape::draw` runtime override and one-call suppression. Boot alone does not
advance S008.

### S002 — gcnport Dolphin executor

Partial capability (was missing): `shared/gcnport`'s pinned Dolphin fork now implements the complete
embedding contract as a public API — `PowerPC::GcnPort::RuntimeSession`, `BootAuthenticatedImage`,
`ExecuteJitBlock`/`ExecuteRefusedBlock`/`ExecuteDiagnosticInterpreterBlock`, `InstallNativeHook`/
`RemoveNativeHook`, `ExecuteOriginalOnce`, `CallOriginalSynchronously` (the synchronous
native→original→native "superCall" continuation S003 below needs), `InvalidateGuestCode`, and typed
`ExecutionCounters` (0/12 `tools/check_dolphin_contract.py` requirements absent; gcnport's own
`docs/project-state.md` S003 now `verified`). This session also drove that public API against real
`GMSE01` code for the first time (a standalone, uncommitted `tools/gcnport_boot/gmse01_boot.cpp`):
3 real JIT blocks compiled and executed from the retail entry point before a fault, since grown to
106 blocks / 7,524 executions as gcnport's own OS-init and hardware-bring-up options closed
successive real-mode and MMIO gaps. See S001's evidence for the current exact fault and its cause
(a DSP-initialization gap, not a gcnport execution defect).

Gap: gcnport owns no disc/apploader/BS2-equivalent OS-init pipeline (its boot adapter is deliberately
scoped to a raw in-memory image, load address, and entry point), and Sunbright has no CMake build
wiring linking against gcnport yet (this session linked by hand against gcnport's already-built
static libraries to prove the API works, not through the product's own build).

### S003 — native override dispatch

Sunbright links `gcnport::dolphin` and installs every hook through `gcnport::DolphinRuntimeAdapter`,
so nothing here names a Dolphin type or restates Dolphin's build requirements. The adapter's own
test covers the mechanism against a synthetic image -- image/module-scoped keys, a mismatched
generation refused, install and removal revoking the direct link, a single-use original-call ticket
that never enters the callback, and the synchronous `call_original` path.

Evidence on the real title, from one 600,000,573-block run with 0 Dolphin alerts: the first
migrated run reproduced the raw-ABI numbers exactly (13,600 compiled blocks, 161,792 fallbacks,
hook counts 0 and 1,428), proving the adapter changed who owns the boundary and nothing else.
`--super-call` then drove the complete round trip at `TApplication::drawDVDErr` (`0x802a5b50`):
1,428 entries, 1,428 synchronous calls into the original body, 1,977,923 interpreted instructions,
and the native caller read r3 after every one -- `0` on 1,393 frames and `'em_3'` on 35. Both values
are the decomp's own control flow (`src/System/Application.cpp`): `'em_3'` is the "now loading disc"
frame the title draws instead of updating, while `DVDGetDriveStatus()` reports the drive busy, and
it is transient rather than a stuck state. The same run shows the instruction bound is a real
measurement and not a guess: `DVDGetDriveStatus` (`0x8034e144`) cost exactly 60 instructions on
every call, and `drawDVDErr`'s calls ranged 67 to 54,064 -- the short path and the error-drawing
path, exactly as its disassembly predicts.

Gap: every hook Sunbright installs is diagnostic. No native override yet replaces guest behaviour,
so the chaining and cache-invalidation controls that matter to a *replacing* override -- nested
guest calls re-entering the dispatcher, and a disabled override -- are proven only at the adapter's
own level, against a synthetic image, not against a title function Sunbright has taken ownership
of.

### S004 — PC-native semantic rendering

Existing implementation evidence covers a renderer-neutral ordered J2D stream (pictures, gradient
rectangles, windows, immediate draws, and resource-font glyphs), all eleven tiled game image formats
and three palette formats, rigid and multi-matrix J3D meshes, multiple ordinary textured/lit/layered/
masked/effect material families, cull/depth/alpha/blend policy, high-level camera projection, stage
lights, directional specular, linear fog, and standard particle billboards. Watched GPU controls and
bounded title/stage audits recorded nonzero output; C077–C096 contain the detailed scopes and
falsifiers.

The first producer seam for the `gcnport` product exists. `title-adapter` reads GMSE01's own
`J3DShape` objects out of guest memory and fills in exactly the `J3dMeshElementSource` that
`native-render` has always been able to decode from an address (`ByteAddress::guest`, big-endian
indexed arrays). Two of its offsets are confirmed against the shipping binary rather than only
against `decomp/sms`: `J3DShape::loadVtxArray` at `0x802e0320` forms `j3dSys` (`0x804045dc`) and
reads `0x10c`/`0x110`/`0x114` for `GX_VA_POS`/`NRM`/`CLR0`, and tests the NBT flag at `0x30`;
`J3DShape::draw` at `0x802e0390` reads `0x28` (`mGDCommands`) and `0x08` (`mFlags`) from `this`.

Measured on the real title through a diagnostic hook at `J3DShape::draw` (one 1,400,000,124-block
run, 0 Dolphin alerts, every entry still calling the original so the title behaves unchanged):
**51,915 shapes and 60,455 matrix groups read with a zero error rate**, 49,565,856 display-list
bytes accounted for. The distributions are the ones the geometry predicts — 51,061 shapes with one
matrix group and 854 with eleven (skinned), and vertex strides of 4/6/7/8/9/10/11 bytes against
descriptor counts of 2/3/4/5/6, the 7-byte case being a one-byte direct `PNMTXIDX` plus three
`Index16` attributes.

That geometry is now decoded and posed, both through `native-render`'s own owners rather than a
second copy. Over three further 1,400,000,000-block runs (0 Dolphin alerts each, every entry still
calling the original):

- **Every matrix group decodes.** 59,862 of 59,862 display lists ran through
  `decode_j3d_mesh_element` with no failure of any kind, producing 3,811,638 triangles, smallest
  group 6 vertices and largest 1,800. Reading the fields only proves the offsets; a wrong stride or
  attribute type desynchronises the list or yields an out-of-range index, so a clean decode of the
  title's own display lists is what proves the layout those fields describe is the authored one.
- **Every matrix group poses.** `read_guest_shape_pose` resolves each group's palette by the route
  `J3DShape::draw` itself takes — `mDrawMatrices[*mCurrentViewNo]`, bounded by
  `mDrawMtxData->mEntryNum` — and classifies the group by its guest vtable
  (`J3DShapeMtx` `0x803e125c`, `J3DShapeMtxDL` `0x803e123c`, `J3DShapeMtxMulti` `0x803e121c`,
  derived twice over: from `J3DShapeFactory::newShapeMtx` at `0x802e8a84` and from an image scan for
  the three `load` overrides). 38,336 single and 21,526 multi groups, all on the indexed
  position-and-normal pipeline, zero errors.
- **The two halves agree.** Every decoded vertex names a GX matrix slot and the pose says which
  slots hold a matrix: 0 of 11,434,914 named a slot with none. Getting there corrected two real
  defects rather than tuning a number. First, `native-render`'s decoder publishes a slot, not the
  matrix register the display list holds, so an adapter indexing by register was wrong by a factor
  of three (3,184,566 disagreements); the decoder now also refuses a register that is not a multiple
  of three or names a slot past the tenth, instead of folding it onto a neighbouring matrix.
  Second, a matrix group does not have to fill every slot it draws through: `J3DShapeMtxMulti::load`
  skips a `0xffff` entry, and GX keeps what the previous group loaded. Measured: of the 8,540 groups
  declaring ten slots, the gaps fall *between* filled slots and their display lists reference them.
  Modelling that as `GuestMatrixRegisters` — carried in draw order, holding resolved matrices rather
  than indices — closed the remaining 276,696.
- **The inheritance is within a model.** Of those 276,696 vertex slots drawn through a register the
  group did not itself load, **0 came from another shape**. That is the discriminator with two
  possible answers: a wrong reading would have matrices leaking across model palettes.

The two CPU-skinning pipelines are read faithfully rather than approximated — under `PCPU` the
position matrix is `j3dSys.mViewMtx` and the normal matrix stays indexed, and under `NCPU` the
reverse — though GMSE01 used neither in these runs (59,862 of 59,862 groups were `PNGP`), so that
path has unit coverage and no title evidence yet.

That geometry's material is now read too, into the same `native_render::J3dMaterialState` the
decomp-side adapter fills -- deliberately the same struct, so the fifteen material classifiers, the
fog contract and the raster policy stay one implementation that cannot tell which runtime filled
their input. A material is four polymorphic blocks, so `title-adapter` reads each by its guest
vtable: colour (`J3DColorBlockLightOff` `0x803e0d38`, `LightOn` `0x803e0cd4`), texture generation
(`J3DTexGenBlockBasic` `0x803e0c84`), colour stages (`J3DTevBlock1/2/4/16` `0x803e0be8`,
`0x803e0b4c`, `0x803e0ab0`, `0x803e0a14`) and pixel engine (`J3DPEBlockOpa` `0x803e0e64`,
`TexEdge` `0x803e0e00`, `Xlu` `0x803e0d9c`, `Full` `0x803e0968`). Every address is derived at least
twice from the shipping image -- each class's `countDLSize` and `load` overrides appear exactly once
each, at the fixed offsets that class's own virtual list puts them at -- and the two families whose
allocator is reachable are confirmed a third way, by the sizes it allocates: `0x18` and `0x44` for
the two colour blocks, `4` and `0x14` for the pixel-engine blocks.

Measured on the real title through a diagnostic hook at `J3DMatPacket::draw` (`0x802edc38`, where
the title itself resolves the material its shape packets are drawn with), one 1,400,000,000-block
run, 0 Dolphin alerts, every entry still calling the original:

- **44,314 of 44,314 material packets read whole**, 52 distinct materials, and a zero error rate in
  each of the four block readers independently.
- The distributions are non-degenerate where the scene has variety and uniform where it does not:
  tev blocks `TVB2`=29,796 / `TVB4`=5,978 / `TV16`=8,540 with stage counts 1/2/3/5, texture
  coordinate counts 0/1/2/4, colour channel counts 1 and 2, cull modes 0 and 2. All 44,314 colour
  blocks are `CLOF` and all 44,314 pixel-engine blocks are `PEFL`; both are properties of this
  scene rather than of the reader, since the shipping image constructs the other colour class at
  `0x802d6bd8` and the reader answers it in unit coverage.
- **The lookup tables were genuinely read.** J3D stores no alpha-compare or depth-mode
  configuration, only a packed id resolved through a table built at boot (`j3dAlphaCmpTable`
  `0x80407150`, `j3dZModeTable` `0x80407450`, both in `.bss` and therefore zero in the image). The
  encoding that produced each id is known, so every row fetched is re-encoded and compared with the
  id it was fetched for: **44,314 checked, 0 disagreements**. A table never built, read at the wrong
  address, or read at the wrong stride fails this; an all-zero answer from an unbuilt table is
  otherwise indistinguishable from a material that authored `GX_NEVER`.

Each material's textures are resolved and decoded as well, through the same
`native_render::decode_res_timg` the decomp path has always used. `J3DMatPacket` holds the texture
table at `+0x40`; the table is `mResourceCount` (`u16`) at `0x00` and `mResources` at **`0x04`**,
which is the one layout fact the decomp header disagrees with -- it declares a virtual destructor,
implying a vptr, while the shipping image's `loadTexNo__FUlRCUs` (`0x802eea04`) reads
`lwz r4, 4(r4)` and then indexes by `number << 5`. `ResTIMG` is `0x20` bytes. Decoding is per
distinct resource rather than per draw, so the measurement is of the decoder rather than of a cache.
In the same run: **44,314 tables resolved, 78,380 bindings, 92 distinct textures decoded, 0 decode
errors**. Two checks constrain the layout rather than the decoder, and both are the shape a wrong
offset breaks: **0 bindings named a texture past their table's count** (a count read at the wrong
offset answers a wrong bound) and **0 tables carried a non-zero halfword where the count's padding
belongs**. The dimensions are all GameCube-plausible powers of two from `4x32` to `256x256`, and the
dimension histogram and the decoded byte total are derived independently yet agree exactly:
`sum(w * h * 4) = 4,516,864`, the reported total.

Choosing a material family from that state is now one shared rule in
`native-render/src/j3d_material_family.cpp` (`classify_j3d_material`), extracted from the decomp
adapter it used to live inside rather than copied: the decomp path and the guest path call the same
function and differ only in the `J3dTextureSource` they hand it. That extraction dropped
`sms-boot/runtime/native_j3d_material_adapter.cpp` from 452 lines to 289 and is behaviour
preserving, including the two rules that read the whole match set rather than the winning family --
how many textures to decode, and the fact that a program the unlit-textured family accepts names its
first texture through its stage's texture map even when a higher-priority family wins.

Run against the real title with no stage lighting available, all 44,314 materials are refused, and
the shipping classifiers name why:

    raster policy results: success=38430 unsupported pixel-engine block=5884
    unlit colour results:  lighting=37482 texture binding=3416
                           multiple active colour stages=2562 unsupported colour program=854
    unlit textured results: lighting=37482 multiple active colour stages=2562
                           missing texture coordinate=854 unsupported colour program=854
                           missing vertex colour=1708 unsupported raster policy=854

**37,482 of 44,314 -- 84.6% -- are refused for one reason: `lighting`.** GMSE01's materials enable
their colour channel's lighting, and every lit family needs a `ModelLightingContext`. Of the rest,
1,708 `missing vertex colour` are an artefact of this hook: `hasVertexColor` and `hasNormal`
describe the shape, not the material, and `J3DMatPacket::draw` cannot see one.

That lighting context is now read from the guest.
`title-adapter/src/guest_stage_lighting.cpp` reads GMSE01's stage-light owner at
`TLightCommon::setLight` (`0x80229a30`) and its byte-identical `TLightMario` override
(`0x80229610`), at entry, from `this`, the `JDrama::TGraphics*` and the light index. The offsets
were recovered from the shipping image rather than taken from the decomp headers, and in one place
the two disagree: `getAmbColor` (`0x80229cec`) scales the ambient alpha by the f32 at `this + 0x18`
while `getLightColor` (`0x80229d78`) scales the light alpha by the one at `this + 0x1c`. The decomp
declares a single `mAlphaScale` at `0x1c` and uses it for both, so its ambient alpha is scaled by
the wrong field. Other addresses derived here: the view matrix inline at `graphics + 0xb4`,
`gpTLightCommonLightAry` `0x8040e0ac` (entries `+0x10`, count `+0x14`, `0x6c`-byte entries with the
position at `+0x10` and the packed colour at `+0x30`), `gpTLightCommonAmbAry` `0x8040e0a8`
(`0x18`-byte entries, colour at `+0x14`), and `gpLightManager` `0x8040e0b4`.

Measured on the real title in the same run: **15,372 relights, 15,372 published, zero errors**. The
scene's light group holds 15 entries and its ambient group 6; every relight took the group path with
no local override and no effect light; slots 0/2/5/7 and ambient slots 0/1/2/3 are used; two
distinct rigs are published -- white light over mid-grey ambient (9,394) and a dimmer
`0x505050`/`0x282828` pair (4,270); the sun sits at `(200000, 500000, 200000)` with shininess
`50.0`, which is the value the decomp's own RE note predicts.

All 44,314 materials are now classified against a published light, and all 44,314 are still
refused: lighting was a necessary input, not the only missing one. Asked of the lit classifiers
themselves, the remaining gates are specific and the counts close exactly:

    lit colour results:   unsupported colour channels=20496 unsupported colour-stage count=4176
                          texture binding=19642
    lit textured results: unsupported colour channels=20496 unsupported colour-stage count=4176
                          unsupported colour program=10248 missing normal=9394
    channel controls (colour/alpha): 0686/0706=1708 068e/0700=11956 0700/0700=1708 0700/0701=1708
                          0701/0700=854 0701/0701=2562 0706/0700=17840 070e/0700=5124 070f/0701=854

The channel-control histogram is the cross-check: three of the nine pairs GMSE01 authors are in the
lit families' accepted set -- `0706/0700` (17,840), `070e/0700` (5,124) and `070f/0701` (854) --
totalling **23,818**, and 44,314 − 20,496 = 23,818 exactly. So the channel gate is understood rather
than merely counted. Of the six unrecognised pairs, `0700/*` and `0701/*` (8,540) have the lighting
bit clear and are genuinely unlit; `068e/0700` (11,956) differs from the accepted `070e/0700` only
in its attenuation function, which points at a specular family rather than a missing one.

**The largest single actionable item was `missing normal` = 9,394**, which was not GMSE01's doing:
it was the material hook passing `hasNormal = false`, because `J3DMatPacket::draw` has no shape in
hand. That composition now lives at the shape seam, in `tools/gcnport_boot/guest_model_probe.cpp`
on `J3DShape::draw` (`0x802e0390`) -- the same seam the decomp uses
(`sb_native_j3d_shape_submit`), taking both geometry flags from the shape's vertex layout and the
material from `j3dSys.mMatPacket`. Measured on the real title, 0 Dolphin alerts:

    51,124 shape draw(s), 51,124 composed with a material, 25,620 classified,
      52 distinct (shape, material) pair(s)
    0 unreadable shapes / material packets / materials; shape, material and texture-table
      errors all `none`; texture decode results `none=64`
    45,146 shapes carry normals, 16,204 carry vertex colour,
      51,124 classified against a published stage light
    classification: success=25620 unsupported_program=25504
    families: lit_masked_toon=8540 lit_textured=9394 lit_tinted_layered_specular=3416
              unlit_textured=1708 lit_specular_color=854 lit_dual_alpha_effect=854
              lit_alpha_tint=854
    64 distinct textures decoded, 3,797,760 bytes of RGBA
    textures per classified draw: 0=854 1=11956 2=4270 4=8540

**50.1% of GMSE01's shape draws now classify into a real material family**, with their textures
decoded and the stage light in force, through the same shared classifiers the decomp path uses.
`lit_textured = 9,394` is exactly the `missing normal = 9,394` the previous measurement predicted,
which is what says the fix landed where it was aimed rather than somewhere else. The
textures-per-draw histogram agrees independently with the families' own texture counts: 4 for the
8,540 masked-toon draws, 2 for the 4,270 layered ones, 0 for the 854 `lit_specular_color`.

Remaining: ~25,100 draws are still `unsupported_program`, and they are now enumerated rather than
counted. `classify_j3d_material` reports a `J3dFamilyRefusals` set -- why *each* of the fifteen
families turned a state down, in that family's own words -- and the probe records every distinct
material no family accepted. **The whole remaining frontier is 22 materials**, because the scene
holds only tens of distinct materials and each is redrawn once per frame:

| authored channels | distinct materials | draws | shape |
| --- | --- | --- | --- |
| `0706/0700` | 14 | 11,862 | lit, 1 texture; ten of them are one stage and differ from the accepted `lit_textured` family *only* in their TEV program bytes |
| `0686/0706` | 2 | 8,804 | two colour channels with a **lit alpha channel**, 2 stages, vertex colour; no family accepts a lit alpha today |
| `0700/0700`, `0700/0701`, `0701/0700`, `0701/0701` | 6 | 5,124 | genuinely unlit, 1--2 stages |

The gates partition exactly: of the refusals, `lit_color` turns down every one for texture binding
(9,394), colour channels (14,100) or stage count (2,468), and those three sum to the total. The
largest single portable win is the ten single-stage lit materials at `0x80fa7bec`--`0x80fa8f9c`,
8,540 draws, whose channels the `lit_textured` family already accepts.

`lit_specular_ramp`, `alpha_masked_color`, `lit_specular_textured`, `lit_textured_alpha_mask`,
`lit_layered_textured`, `lit_tinted_layered_specular` and `lit_masked_toon` each refuse *every*
remaining draw on colour channels alone, so none of them is close; they are not the next port.

`tools/re/tev_decode.py` decodes a stage program into the expression it computes, and is checked by
`--selftest` against the programs the shipping families already accept. Run over the eleven
`0706/0700` single-texture materials it says what they are:

    c008fe8fc108e670  colour: prev = clamp(konst*tex.rgb)        alpha: prev = clamp(a0*tex.a)
    c008f28fc138e670  colour: prev = clamp(c0.rgb*tex.rgb)       alpha: prev = clamp((a0*tex.a)/2)
    c018f28fc108e670  colour: prev = clamp((c0.rgb*tex.rgb)*2)   alpha: prev = clamp(a0*tex.a)
    c008fe8fc118e670  colour: prev = clamp(konst*tex.rgb)        alpha: prev = clamp((a0*tex.a)*2)
    c008fecfc108e670  colour: prev = clamp(konst)                alpha: prev = clamp(a0*tex.a)
    c008fffec108f0f0  colour: prev = clamp(konst + zero)         alpha: prev = clamp(tex.a*a0)
    c008e28fc108e670  colour: prev = clamp(lerp(konst, c0.rgb, tex.rgb))
    c008ec8fc108e670  colour: prev = clamp(lerp(konst, one, tex.rgb))

**None of them reads `ras.rgb` or `ras.a`.** Their colour channel is authored as lit
(`0x0706` selects the primary light), the channel is computed, and the stage then never selects it.
So these draws are raster-independent: a family may render them without any lighting at all, and
that is provable from the program rather than assumed. That is what makes them the next port --
`tinted texture`, colour = tint x texture and alpha = tint alpha x texture alpha, with the tint
taken from a constant or a TEV register colour, and the shift applied. Six of the eleven materials
(5,124 draws) are exactly `konst|c0.rgb * tex.rgb` with `a0 * tex.a`; the `lerp` and
`konst`-only variants are separate programs and stay refused until they are ported on their own
evidence.

Gap: nothing is drawn yet, and nothing is published. The decoded vertices, poses, materials and
textures are counted and discarded; no `ModelDraw` is built, no material classifies into a family
(measured above), no stage lighting is published from the guest, no semantic frame is produced under
the dynarec, and the run uses Dolphin's Null video backend so no frame is presented. `native_render::ModelDraw` also has no place for the normal matrix, which under the CPU
pipelines genuinely differs from the position matrix; the adapter carries it as
`GuestShapePose::normalViews` and the semantic boundary has yet to accept it. The surviving decomp-side adapters
(`sms-boot/runtime/native_j3d_adapter.cpp` and its peers) remain native/decomp evidence, attached to
the decomp product rather than to `gcnport`. Material families, non-billboard particles, image
producers, screen effects, and full-frame ordering remain incomplete. The new JIT seam must preserve
the same value-only contract; no old body or GX compatibility path may become a silent fallback
after its semantic owner is proven.

### S005 — decomp evidence adapters

The native-layout adapters compile against `decomp/sms` and have exercised the same semantic values
as the guest-layout adapters without sharing objects. Bounded decomp/Aurora runs have reached title,
file-select, Delfino flow, thousands of semantic 2D operations, tens of thousands of J3D submissions,
lights, fog, and representative material families. Claims C079–C096 record exact observed scopes.

Gap: `decomp/sms` still contains upstream divergence, unnamed fields, and reachable incomplete bodies;
some live scene evidence is blocked by issue 30's retained-GX invalid wrap state. The decomp remains
evidence and readable source rather than a second shipping runtime.

### S006 — smooth presentation

The existing interpolation work records stable identities for J3D shapes, matrices, cameras,
billboards, and reached indexed quads. Instrument I038 and C076 provide a controlled camera/sea
comparison showing a slight region-level improvement and a planted forced-snap opposite answer.

Gap: residual palm/sky motion is not joined to a stable draw identity, the graphics census is not
complete, the implementation is not integrated with the new executor/renderer, and native-rate modes
remain performance-limited in heavy Delfino intervals.

### S007 — decomp expansion

The decomp is runnable as an evidence path and contains binary-derived implementations and native
host-safety adaptations. Existing RE notes retain exact GMSE01 addresses, object layouts, formulas,
and behavior for camera, water, J2D, J3D, audio, effects, threading, and game systems.

Gap: upstream convergence debt, established-but-unnamed fields, and reachable incomplete bodies
remain. Each pass must rebase, converge matching ownership units, then extend only from evidence.

### S008 — representative gameplay conformance

The unattended half is measured. In one 4,000,000,558-block run (436.9 s wall, ~9.16M blocks/s,
0 Dolphin alerts, 0 invalid guest accesses, 29,234 blocks compiled and 99.97% of executions served
from the block cache), `--watch-guest 803e9700:8` recorded `gpApplication` walking GMSE01's entire
attract cycle twice, with the transitions timed in guest retraces:

| retrace | `mAppState` | `mMovie` | what the title is doing |
| --- | --- | --- | --- |
| 78 | 2 `BOOT` | 0 | first VI retrace delivered |
| 84 | 3 `NLOGO` | 0 | `mDirector` constructed |
| 295 | 4 `DONE` | 9 | `mNextArea.set(15, 0, 0)`, opening movie queued |
| 6,214 | 5 `GAMEPLAY` | 9 | stage 15 (file-select) running |
| 9,019 | 5 `GAMEPLAY` | 12 | demo movie queued |
| 9,054 | 6 `MOVIE` | 12 | demo movie playing |
| 11,480 | 5 `GAMEPLAY` | 12 | back to file-select |
| 14,279 / 14,316 | 5 → 6 | 9 | opening movie again |
| 20,232 | 5 `GAMEPLAY` | 9 | back to file-select |
| 23,037 / 23,064 | 5 → 6 | 12 | demo movie again — cycle two |

23,064 retraces is ~384 s of guest time, and the cycle lengths are the real ones: movie 9
(`Entrance.thp`, 2,816 frames at 30 Hz) occupies 5,916 retraces against the 5,632 its frame count
predicts, the rest being its fades. The title is not merely surviving; it is keeping console time.

Missing capability: drive a bounded, *interactive* gameplay scenario through the real gameplay
target with native renderer and native owners active. Nothing above involves input, a native
renderer (the run uses Dolphin's Null video backend, so no frame is presented), or native subsystem
owners. Compare guest PC/register state, relevant memory,
timing/interrupt/service events, audio, input, and presented frames against an independent oracle;
report JIT blocks, cache activity, invalidations, overrides, and denominators. Qualify correctness and
frame-time behavior on every released host architecture.

### S009 — retired executor removal

Evidence: the retired executor, its artifacts, selectors, tests, and launch scripts are deleted.
`tools/migration_boundary.py` scans the first-party tree and has planted positive and negative
controls that prove those surfaces cannot return. No replacement executor was fabricated.

### S010 — independent evidence infrastructure

Evidence: `extern/dolphin_fork` retains the maintained independent emulator core and observation
hooks; the Python parsers under `tools/oracle/`,
`decomp/sms`, the GMSE01 symbol/address corpus, RE notes, claims, and controlled native/GPU tests
provide independent behavior and layout evidence. The instrument ledger records trusted and
distrusted tools explicitly. This capability proves mechanisms within named scopes, not whole-game
parity.

### S011 — native audio

The decomp evidence path has an audible native JAS voice renderer. The prior guest-layout path also
proved the Zelda-class ucode contract, seven sub-frame cadence, AFC/PCM decoding, resampling, and L/R
mixing with audible music and effects. `docs/audio/` retains the binary field maps and observed
residuals.

Gap: select one title-owned native audio implementation for the native/dynarec product, connect it to
the new lifecycle and typed configuration, and verify music, effects, streaming/movie audio,
positional/aux routing, teardown, and bounded output against the oracle.

### S012 — application structure and policy

The repository has tracked Clang formatting/tidy configuration, a typed immutable launcher parser,
source-size ratchets, native-render dependency checks, and a migration-boundary scanner. The scanner
rejects retired executor paths/selectors, non-Python scripts except `run.sh`, direct product output
outside the logger owner, and environment reads outside the configuration owner.

Gap: the target gameplay composition root and cohesive RAII owners do not exist. Lucent-backed
product logging and the final typed persisted configuration owner must be implemented with gcnport.

### S013 — default launcher

Blocker: S002, the shared `gcnport` Dolphin-JIT executor exists but its complete embedding adapter is
not implemented yet.

Blocked by S002: `./run.sh` is a slim locked-Python shim and currently refuses by naming the missing
shared gcnport Dolphin-JIT executor. Once the executor exists it must validate exact `GMSE01`, keep
the JIT/native selection invariant, name missing native dependencies with platform commands, and
never run tests.

### S014 — packaged setup

Missing capability: produce asset-free desktop packages whose first launch opens a native picker,
accepts the primary image directly or one bounded nested ZIP, validates the complete exact-title
install, preserves the previous valid choice on failure, and persists data in OS user locations.

### S015 — widescreen

Existing native work owns projection, HUD placement, and several screen/effect boundaries and has
GMSE01-specific RE notes and controls. It does not rely on final-image stretching.

Gap: integrate that policy with the single dynarec/semantic-renderer product, enumerate every
horizontal culling/scissor/screen-effect boundary, and verify additional world coverage and 4:3
faithfulness through deterministic geometry-based controls.

### S016 — input, saves, and settings

Existing paths have keyboard/controller translation, native memory-card work, persisted renderer/
frame-rate/effect settings, and an in-game settings UI with layout controls.

Gap: move their surviving behavior behind one typed immutable product policy and focused RAII owners;
remove execution-engine choices, route physical and future virtual controls through one action model,
store saves/settings in OS user data, and verify them through the native/dynarec gameplay path.

### S017 — host JIT qualification

Missing capability: qualify x86_64, Apple Silicon macOS AArch64, and Android arm64-v8a separately
with the shipping JIT. Evidence must cover executable-memory publication/protection, instruction-
cache coherence, ABI transitions, exceptions/signals, packaging, representative interactive
gameplay, and fallback ratios. A fallback-heavy or zero-JIT run and one AArch64 OS cannot stand in
for another host backend.

### S018 — asset-free automation

The canonical Python verifier checks the migration boundary, source structure, live documentation,
registry paths, deterministic shader regeneration, decomp symbol tooling, Clang formatting/tidy,
the Ninja build, and the native-renderer test suite without game files. The tracked workflow invokes
that same owner on Linux x86_64, Windows x86_64, and the macOS Apple Silicon runner with pinned
actions, Python, uv, and SDL inputs. This proves only redistributable native components and tooling;
it does not claim boot or gameplay.

Shader provenance uses one host-neutral Python provisioner for Linux, Windows, and macOS. It pins
the maintained shaderc v2026.1 fork plus the exact compatible glslang, SPIRV-Tools, and SPIRV-Headers
commits, verifies every downloaded source archive against its recorded SHA-256 before bounded traversal-safe
extraction, builds with Ninja under the locked Python interpreter, and refuses any missing or stale
installed tool instead of consulting `PATH`. All 13 embedded shader headers match this exact
compiler/validator pair locally.

The shaderc dependency is `SomeoneIsWorking/shaderc` at
`50f71a748725b3df267128e519ef6c59881fc33e`, published as `v2026.1-port.1` and based on upstream
`301b4ede53d59b68bf55f95bb26412d9233c8187`. Its bounded string-to-word copy preserves NUL truncation
and zero padding while avoiding the deprecated CRT `strncpy` call rejected by the Windows
Clang/MSVC-target build under `-Werror`. The source change and its regression tests live in the
fork; the provisioner applies no patches. The fork's remote `main` tracks newer upstream code and
is deliberately not the consumed release. The versioned fix passes all 12 copy cases and all 103
compiler tests locally. macOS CI selects AppleClang explicitly; Homebrew LLVM supplies only the
formatting and lint tools.

Self-tests declare game-image and host instrumentation requirements. The asset-free gate reports
the selected, skipped, and discovered denominators, runs Linux kernel/RADV instruments only on
Linux, and omits only the explicitly declared GMSE01-image check. `tools/verify_re.py` owns that
separate check and hard-refuses a missing DOL, so hosted automation cannot turn unavailable game
evidence into a passing empty result.

Android CI is blocked and deliberately has no placeholder job: Sunbright has no Android Gradle/NDK
consumer, package identity, or `gcnport` arm64-v8a executor to build. Add the Android job only when
those real owners exist, and route it through the shared Android build contract. JIT gameplay and
performance remain missing on every host under S017 regardless of these component jobs.

Hosted run `33959423142` confirms Windows builds the pinned shaderc fork and passes the 13-shader
UTF-8 header check. Header I/O explicitly uses UTF-8, writes LF, and accepts Git's CRLF checkout
through universal-newline reading. The shipping self-test covers file round trips and altered-word
rejection, including Python UTF-8 mode disabled under the ASCII locale.

The same Windows run then exposed MSVC STL's vectorized `std::find` instantiation on the image
cache's 16-byte key. The key's default memberwise equality remains unchanged; the unused
current-frame key vector and its search are removed. `CachedImage::lastUsedFrame` remains the
single eviction-use owner. Local Clang 22.1.8 verification passes the real watched semantic GPU
suite, including repeated same-frame image resolution, resident reuse, and changed-revision
readback controls; three focused image/platform CTests and both touched translation units'
format/tidy checks also pass.

Run `33960561447` subsequently compiled all 98 native build steps and passed 25 CPU tests on
Windows, then stalled at the first SDL-linked test, `native_render_sdl_gpu_platform`. GitHub's
check annotation records the 45-minute job execution limit, not runner shortage or concurrency
cancellation. SDL was found for linking but its runtime DLL was neither staged beside the
executables nor exported by setup-sdl into the runtime search path; the log does not expose the
loader's exact blocking UI or exit state.

CMake now resolves transitive Windows runtime DLLs from imported-target metadata (requiring CMake
3.21), stages them beside both SDL test executables, and registers integrity checks that reject
missing or stale DLLs. The fake-SDL platform test and deployment checks have individual 30-second
limits; the real-GPU executable remains outside unguarded CTest. Seven portable controls cover
deployment, refusal, repair, static linkage, and a configure-only Windows dependency graph whose
registered CTest fails before staging and passes afterward. The existing fake-SDL platform test
passes locally with Clang; no Windows execution is inferred from this metadata fixture. There is
no gameplay executable to deploy yet.

The combined Linux Clang landing gate passes all 14 asset-free steps: 20/20 selected
tool self-tests, 27/27 CTests, formatting for 111 files, and clang-tidy for 68 translation
units. Its one excluded self-test requires user-supplied game data.

Hosted run `33964399865` at `8d4731cc411f93994f84fd5b161d0885ea887952` confirms the Windows
deployment change: `native_render_sdl_gpu_platform` passes in 0.02 seconds, both runtime-DLL
integrity checks pass, and all 29 CTests pass in 5.16 seconds. Linux x86_64 and macOS Apple Silicon
complete the whole job successfully. Windows then fails at C++ lint selection: the database index
used native backslash-separated relative strings while the candidate list used forward slashes,
incorrectly reporting all 68 translation units missing.

The quality owner now indexes and selects by the same full native absolute-path identity, resolves
relative command files against their recorded compilation directory, and preserves the original
command and owning build database. Seven controls exercise Windows drive/separator/case handling,
UNC share identity, POSIX case distinctions, wrong-directory/drive rejection, relative-directory
refusal, and actual native file matching; the local database matches 68/68 selected translation
units. No source or lint check is excluded to repair the mismatch.

Hosted run `33965495457` at `1922ef70b1ccbd687ca103ab80e6ab0e0266497f` passes on Linux x86_64,
macOS Apple Silicon, and Windows x86_64. Native Windows passes 16/16 selected tool self-tests,
29/29 CTests (including the SDL platform test and both runtime-DLL integrity checks), formatting
for 111 files, and real clang-tidy for all 68 translation units; the canonical verifier completes
all 14 asset-free steps. This confirms the corrected native compile-database matching and DLL
deployment on Windows, not merely the portable metadata fixtures.

Gap: Android remains missing until its real application and executor boundaries exist. Gameplay,
JIT host qualification, and performance remain missing under S017 on every host; these successful
asset-free component jobs do not supply game-conformance evidence.

**2026-09-18 (ninth continuation, S001): GMSE01 boots to its opening movie with zero invalid guest
accesses.** Two owner gaps closed in `shared/gcnport` (gcnport `e0b7e1e`, Dolphin fork `a514f624`).

First, gcnport never called `Common::Log::LogManager::Init()`. Dolphin reaches that singleton
through an unchecked raw pointer, and `FileMonitor::FileLogger::Log` -- which
`DVDThread::ProcessReadRequest` calls on every disc FILE read -- dereferenced it, faulting the DVD
thread at guest tick 935,443,084 the first time a read went through the file system rather than the
raw disc header. The boot now owns the log manager, over an empty Base config layer because
`LogManager`'s constructor writes its settings there.

Second, and the reason the title then crashed on its first frame: **the disc's region was never
published to `SConfig`**, so `CBoot::SetupGCMemory` wrote a PAL video format at 0x800000CC for a US
disc. The title built a PAL render mode (`xfbHeight` 530) against its NTSC-sized framebuffer
allocation (0xa5000 = 640x528x2), so its display copy ran two lines past that `JKRExpHeap` block and
zeroed the `JDrama::TDisplay` immediately after it; `TApplication::gameLoop` then branched through
the resulting null vtable. gcnport now takes the region from the volume, as
`SConfig::SetPathsAndGameMetadata` does, and applies Dolphin's shipped `Sys/GameSettings` layer for
the title at the same point -- global layer only, never the user's own per-title INI.

Measured on the pinned tree, retail disc, 400M blocks: 0 invalid guest accesses, 1,496 VI retraces
delivered, 13,595 JIT blocks compiled (7,823 before), a 640x448 NTSC render mode, and the guest
executing `__THPHuffDecodeDCTCompY` -- decoding its opening movie. `tools/verify.py --runtime` in
gcnport passes 30 required tests, including the new
`GcnPortRuntime.DiscRegionAndShippedSettingsConfigureTheConsole`, which boots two synthetic
asset-free disc headers differing only in country code.

Remaining under S001: interpreter fallbacks rose from 141 to 79,952 events over the same budget once
the THP decoder was reached, which needs reporting by reason with denominators before it can be
called understood; and the `J3DShape::draw` runtime override at `0x802e0390` is still not reached.
Both were settled in the tenth continuation below, and S001 is now verified.

**2026-09-18 (tenth continuation, S001 verified): the `J3DShape::draw` hook is entered 245,874 times
on the real title, and every interpreter fallback is a Gekko SPR access.**

`--count-calls <hex-addr>` installs a native hook that counts entries to a guest function and returns
`HookAction::RunOriginalOnce`, so the translated body still runs and the title behaves exactly as it
did. One 4-billion-block run of the retail disc, four addresses:

| address | function | entries |
| --- | --- | --- |
| `0x802e0390` | `J3DShape::draw` | 245,874 |
| `0x802dedc8` | `J3DModel::entry` | 106,615 |
| `0x802a5f5c` | `TApplication::gameLoop` | 9 |
| `0x802a5b50` | `TApplication::drawDVDErr` | 12,538 |

Zero Dolphin alerts, zero invalid guest accesses, 29,243 JIT blocks compiled, and the run retired its
whole budget. That is S001's success condition measured rather than argued: a native callback at a
real guest address in the real title, entered a quarter of a million times, each returning through
the one-call original-body path, with the title drawing 3D geometry throughout.

`gameLoop` at 9 entries is the reading that makes the rest legible: it is the per-app-state loop
called from `TApplication::proc`, not a per-frame one, so 9 is nine director/area transitions.
`drawDVDErr` is the per-frame one -- `gameLoop` calls it every iteration as a drive-status check that
returns 0 and draws nothing when the drive is healthy (`decomp/sms/src/System/Application.cpp:857`).
A 600M-block run counted 1,428 entries to it and 0 to `J3DShape::draw`, which reads as a DVD error
screen until the decomp says otherwise; it is simply the opening, which is `J2D` and THP with no J3D
geometry in it at all.

`gpApplication` at `0x803e9700` confirms the same progression from the other side. At 600M blocks
`mAppState` is 4 (`APP_STATE_DONE`) with all three areas zero; that case sets `mMovie = 9` --
`Entrance.thp` -- and `mNextArea = (15,0,0)` before falling through to `APP_STATE_MOVIE`. At 4B
blocks `mAppState` is 6 (`APP_STATE_MOVIE`), `mMovie` is 12 and all three areas are 15. The title is
running its real attract loop, and `mDisplay` is still the intact 640x448 `JDrama::TDisplay` at
`0x8056dd90`.

The open fallback question is closed, and the earlier guess was wrong. gcnport now records WHERE it
falls back, not just how often (`GetFallbackSites()`, bounded at 256 sites with its own truncation
counter). All 1,109,684 events across the 4B-block run fall in 22 sites, none untracked, and every
one is an `mfspr`/`mtspr` on a Gekko-specific SPR that Jit64 routes to the interpreter by design:
`0x803438f8` (`mtspr DMAL`, the locked-cache DMA kick) is 1,099,470 of them and `0x80341ab0`
(`mtspr DEC`) another 10,093. The suspected paired-single opcode gap does not exist. 99.97% of block
executions are compiled code.

One instrument was lying and is fixed. The stall reporter fired whenever two consecutive batches
ended at the same guest PC -- the shape of every stall back when the boot died in its first seconds,
and meaningless once the title runs a main loop, because a million-block batch ends inside a
cache-flush or soft-divide leaf often enough that one run printed 204 "stalled" reports, each with a
full thread walk, while its retrace count climbed past 20,000. The repeated PC is still the cheap
trigger for taking a sample; what the sample is called now comes from whether any VI retrace was
delivered since the last one, and only the genuine no-retrace case pays for the state dump.

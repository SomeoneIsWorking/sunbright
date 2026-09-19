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
whole attract cycle unattended and faultlessly (S008 below has the retrace-timed trajectory). Its
producer seam reads, decodes and poses the title's own geometry, classifies every material it draws,
resolves each material's textures from the display list it baked rather than from its packet's stale
table, publishes the title's `J2DPicture` panes as the 2D pass, and rasterises all of it through the
shipping passes on a real device. Frame 3600 is the title screen: sky, sea, logo, shine, palm tree,
rainbow, copyright line and fly-in letters. What remains is a per-region diff against the console --
the one named difference today is retail's additive sun glow (its object 27 of 127), which is a 3D
draw on the model path.

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

Wiring in the `textured_effect` family took classification from 25,650 draws to **35,868 of 51,314
(69.9%)**. That family -- a texture modulated by an authored constant or register colour -- was
already implemented, already tested and already drawn by the renderer, and was reached by no
runtime: nothing called `classify_j3d_effect_material`. Its nine programs are exactly the nine the
TEV decoder found among the refused materials. It is asked last, so it cannot take a draw from a
family that already had one, and the measurement confirms it took none: every other family's count
is unchanged across the wiring.

    classification: success=35868 unsupported_program=15446
    families: textured_effect=10248 lit_textured=9394 lit_masked_toon=8540
              lit_tinted_layered_specular=3416 unlit_textured=1708 lit_specular_color=854
              lit_dual_alpha_effect=854 lit_alpha_tint=854
    76 distinct textures decoded, 3,957,504 bytes
    textures per classified draw: 0=854 1=22204 2=4270 4=8540

Porting the `lit_masked_specular` family took classification to **44,512 of 51,250 (86.9%)**. Its two
materials are one program: a detail image added to a signed-diffuse layer offset by one half, then
chosen per channel against the directional highlight by a mask image's own RGB. They differ only in
whether the mask's alpha gates opacity, which the family carries as a flag rather than as a second
near-copy of the program. Its alpha channel is lit in its own right (`0x0706`), which nothing in the
renderer computed: `diffuse_lighting` passed alpha through unlit, so the illumination is now
accumulated once and `lit_alpha` multiplies by it.

    classification: success=44512 unsupported_program=6738
    families: textured_effect=10248 lit_textured=9394 lit_masked_specular=8644
              lit_masked_toon=8540 lit_tinted_layered_specular=3416 unlit_textured=1708
              lit_specular_color=854 lit_dual_alpha_effect=854 lit_alpha_tint=854
    80 distinct textures decoded, 3,984,128 bytes
    textures per classified draw: 0=854 1=22204 2=12914 4=8540

Every other family's count is unchanged across the port, so the new one took no draw from a family
that already had it. The two-texture bucket rose by exactly 8,644 -- the new family's own count,
from a histogram computed independently of it.

Reading each refused material's own refusal set -- which family stops it, and at which gate -- is
what the probe now prints, and it named a gate that was not a material question at all. The pixel
policy is matched against an enumeration of exact authored combinations, and every one of them
required a less-or-equal depth comparison. `ModelRasterPolicy` has carried a full `depthCompare`
since it was written, and `semantic_3d_pass` reads it into both the pipeline key and the depth
state; only the classifier declined to fill it. GMSE01's material at `0x80e85660` compares with
LESS, so it was refused for a value the boundary already had somewhere to put. The comparison is
now read as data, and the title's additive combination that still tests depth is admitted beside
the one that does not.

That took classification to **45,730 of 51,614 (88.6%)**. `unlit_textured` gained exactly the 854
draws of `0x80e85660`; `lit_masked_specular` gained draws too, because every family asks the same
policy classifier. No family lost a draw, and 54,270 draws now reach the sink, all accepted.

Two single-stage unlit materials followed, taking classification to **46,322 of 51,352 (90.2%)**.
`0x80d3a7e8` takes its colour from a register and its alpha from the raster, which is the same
semantic material the lit effect family publishes -- a texture times an authored colour -- reached
through authored state that shares none of that family's gates, so it is a separate rule publishing
the same type. `0x80e85db4` writes the channel colour straight into the working register instead of
accumulating it into a colour register; the unlit colour family knew only the second spelling.

    classification: success=46322 unsupported_program=5030   (90.2% of 51,352)
    unlit_textured_effect=854, every other family's count unchanged
    54,862 draws composed and submitted, 9,790,794 vertices, 0 rejected

Six copies of the colour-register conversion had accumulated across the material families, three of
them in a variant that discards alpha. It lives once beside `color_from_rgba8` now; the seventh copy
was what prompted looking.

The refusal set is indexed by family, and appending a family past the enumerator its size is
derived from writes one slot beyond it. That happened here, on the first family added after the
check for it was written -- and the check failed the build, naming the family that no longer fit.
The measurement was retaken with the corrected array rather than reported from the run that had it.

`0x80e85db4`'s program was accepted after that, but its policy blends the source whole against one
minus its own colour, and `ModelBlendMode` had no value for that combination. It has one now, and
`semantic_3d_pass` maps every mode through one exhaustive switch rather than a ternary chain that
had to be extended in step. Classification reached **47,042 of 51,218 (91.8%)** and `unlit_color`
appeared for the first time -- but the publisher then reported **854 draws refused by the sink**,
one for each draw of that material.

The cause was a hand-written range check, `raster.blend <= ModelBlendMode::DestinationAlpha`, which
named the enumerator that had been last until this change. It is the same defect as the family-count
array two commits earlier, in a different file, so the check for it is now general: `structure_check`
reads every scoped enumeration in `native-render` and fails any `<= Enum::Member` bound whose member
is not that enumeration's final one. It found the blend bound immediately, and the first draft of
the rule found nothing because a prose comma inside an enumeration's comment read as an enumerator
separator -- so it strips comments now, and its selftest asserts exactly that shape. `model_test`
carries a control that walks every blend mode through `valid`, and fails on the old bound.

    classification: success=47460 unsupported_program=4176   (91.9% of 51,636)
    56,000 matrix group(s), 56,000 composed, 56,000 submitted, 11,372,298 vertex(es)
    0 rejected by the sink, 0 dolphin alerts

Two of the five then turned out to be one material. `0x80e85ac0` and `0x80fa4c00` both multiply two
textures together, tint the product by one authored colour and double it -- the console idiom for a
surface carrying a baked light map -- and they differ only in where the tint comes from and what
decides opacity. `0x80e85ac0` rasterises a channel for the tint, taking its colour from the material
register and its alpha from the vertex, and carries both textures' alpha through doubled;
`0x80fa4c00` takes the tint from a colour constant its stages name by selection, and its opacity
from that constant's alpha times a colour register's. No stage of the second reads a raster input at
all, which is why its lit channel can be admitted: nothing consumes the channel's output. The first
spelling's channel *is* consumed, so lighting it is refused rather than ignored, and a test asserts
that asymmetry from both sides.

    classification: success=48517 unsupported_program=2515   (95.1% of 51,032)
    doubled_texture_pair=1661 (854 + 807), every other family's count unchanged
    57,057 draws composed and submitted, 0 rejected by the sink, 0 dolphin alerts

`0x80e8817c` followed: two texture layers, each scaled by its own authored colour constant, the
second added to the first rather than modulating it. Each stage names its constant by selection, so
the two layers read different constants rather than one fixed slot, and opacity comes from a colour
register's alpha through both images -- it belongs to neither tint, which is why it is published on
the first one's alpha channel and the second's is zero. Neither stage reads a rasterised channel, so
this rule gates on the program alone.

    classification: success=49653 unsupported_program=1661   (96.8% of 51,314)
    tinted_texture_sum=854, every other family's count unchanged
    58,193 draws composed and submitted, 0 rejected by the sink, 0 dolphin alerts

The last two were ported together. `0x80ed7738` doubles its second image against the rasterised
channel and overwrites the colour its first stage computed, keeping only that stage's alpha -- so
the first image's colour is authored to be discarded, which is what separates it from the doubled
pair, whose second stage multiplies what the first produced. `0x80fa490c` lets one image choose per
channel between two authored colour registers, offsets the choice by a colour constant's alpha,
then adds a five-eighths-weighted second image, biases by a half and halves the result; its alpha
comes out scaled by one plus three eighths because the stage adds a fraction of what it already has
rather than replacing it. Both fractions are read from their selections through
`J3dKonstFraction`, so a different authored fraction is a different material rather than the same
one with a transcribed constant.

**Every material GMSE01 draws is now classified.** Measured on the real title, 0 Dolphin alerts:

    classification: success=51236   (51,236 of 51,236; the refusal line is gone)
    families: unlit_color=854 lit_specular_color=854 unlit_textured=2562 lit_textured=9394
      lit_dual_alpha_effect=854 lit_alpha_tint=854 lit_tinted_layered_specular=3416
      lit_masked_toon=8540 lit_masked_specular=8630 textured_effect=10248
      unlit_textured_effect=854 doubled_texture_pair=1661 tinted_texture_sum=854
      masked_doubled_texture=854 interpolated_registers=807
    0 distinct material(s) no family accepted
    59,776 draws composed and submitted, 11,421,498 vertices, 0 rejected by the sink

This is coverage of the material *programs* the title runs, not proof that each one's output matches
the console: GMSE01's own draws are not rasterised yet, so each family's colour maths is checked
against its decoded program, its unit controls, and the GPU controls below rather than against the
console's pixels.

The five families added here had never been drawn on a device -- their shaders had never been
compiled by a driver. The 3D GPU controls lived inside `semantic_2d_pass_gpu_test.cpp`, a 1,049-line
file named for the other pass, so they were extracted to `semantic_3d_pass_gpu_test.cpp` before
being extended, and the sRGB conversions both files predict pixels through were given one owner in
`semantic_gpu_test_support`. Each new family now renders offscreen and is read back:

- **Doubled texture pair** -- a mid-grey base under a white detail comes out at twice the base;
  darkening the detail to the same grey halves it again, so the second image provably reaches the
  product. Its two opacity answers are checked separately: the doubled product keeps both images'
  half alpha, the constant spelling publishes the tint's own and ignores the images.
- **Tinted texture sum** -- red and green tints over white images sum to yellow; blanking the second
  image leaves red alone, which no single-layer program would do.
- **Masked doubled texture** -- turning the first image blue changes nothing (its colour is authored
  to be discarded) while changing its alpha changes the output. Both halves of that claim are
  checked, since only one of them is what the shader would get wrong.
- **Interpolated registers** -- a black chooser gives the lower register's half-biased half and a
  white one the upper register's, the two ends where the choice is exact whatever the colour space.
- **Inverse source colour** -- over mid grey, a red source leaves red at full and grey's own half in
  the other two channels, which ordinary alpha compositing cannot produce.

The controls are live: replacing the doubled pair's `* 2.0` with `* 1.0` in its shader fails the
first of them by name. They run under `tools/render/gpu_watch.py` and stay outside unguarded ctest.

**GMSE01's own draws are now rasterised.** `--render-frames <hex-addr>` turns the diagnostic run
from counting the title's draws into drawing them: `tools/gcnport_boot/guest_frame_renderer.cpp`
opens an offscreen SDL GPU device, hands the process frame bridge to `SdlSemanticFrameClient` at
GMSE01's own external-framebuffer size (640x448), and a hook at the title's frame seam
(`JDrama::TVideo::waitForRetrace`, `0x802fc9a4`) seals each frame and encodes it through the
shipping `Semantic3dPass`/`Semantic2dPass` before opening the next. Nothing about the composition
changed: the publisher submits the same `ModelDraw` through the same `submit_model`, and the only
edit it needed was to stop claiming the process's one semantic sink when the bridge already holds it
-- it says so in its report rather than printing the zero its own counting sink would now record.

Measured on the real title, one 1,400,001,968-block run, 0 Dolphin alerts:

    4,004 frame seam(s) entered, 4,004 submitted, 4,004 completed, 853 non-empty
    59,864 model(s), 52,792 mesh(es), 10,329,555 vertex(es), 78,288 image(s) reached the passes
    1 frame(s) sampled; first non-clear frame 3152 with 286,720 non-clear pixel(s)
    0 seal failure(s), 0 encode failure(s), 0 begin failure(s)
    the client validated its own output

Three of those numbers are cross-checks rather than restatements. **286,720 is 640 x 448** -- the
sampled frame's every pixel differs from the controlled black clear, which is what a scene whose
background is itself drawn geometry produces and an empty frame cannot. The publisher submitted
59,934 draws and 59,864 reached the passes: the 70 missing are the ones submitted into the final
frame, which the run ended before the title sealed. And 52,792 meshes against 59,934 models is the
collector coalescing identical geometry, which only agrees if the resource identity and revision the
publisher derives are stable across draws.

The negative is a reading from the same instrument rather than an argument. At 300,000,000 blocks
the title has not begun drawing J3D geometry: that run sealed 560 frames, all of them empty, put 0
models through the passes, and the client **refused** to validate -- *"semantic output never
observed pixels distinct from the controlled clear"*. The same binary, the same flag, the other
answer.

**The frames can now be looked at.** `SdlSemanticFrameClient` takes an optional sample observer --
it is handed the sampled frame's own pixels while the readback is still mapped, and answers whether
it kept them; a refusal fails the encode rather than losing the frame quietly. The boot tool's
`--dump-frame <path>` writes one frame as a P6 PPM, the format this repository's own comparison
tools (`tools/render/ab_diff.py`, `tools/render/sb_oracle_diff.py`) already read, so a rendered
frame can be diffed against a Dolphin capture with no converter in between. Without
`--dump-frame-index`, the frame written is the first one whose pixels differ from the clear and only
that one is ever downloaded; with it, a named sealed frame is written and every frame up to it is
downloaded, because the client cannot know a frame is the one wanted without reading it back.

The observer is controlled on a device in `semantic_2d_pass_gpu_test`: it must be able to reproduce
the client's own non-clear count from the bytes it was handed (otherwise the two describe different
frames), and a refusing observer must fail the encode. Replacing the client's `return observed` with
`return true` fails the second by name.

Two frames of the title's attract cycle, from 1,400,000,000-block runs, 0 Dolphin alerts:

- **Frame 3152**, the first with any content, written by the default path. Its hash is the same
  `10633304561967121689` two independent runs reported, so the render is reproducible rather than
  timing dependent.
- **Frame 3800**, named explicitly, 4,004 frames sampled to reach it.

Both show recognisable GMSE01 geometry -- the sky dome, its cloud billboards and Delfino's seagulls
in the correct places -- and both are **grossly over-bright**: 43.7% of frame 3800 is pure white, and
the island geometry that should sit under the sky cannot be told from it. So the title's draws reach
the passes, compile on a real driver, and put its own geometry on the target in the right shape,
while their shading does not yet match the console.

**Two diagnostic draw modes narrow where that brightness enters**, because a rendered frame cannot
answer it alone. `--draw-mode family-map` replaces every material with a flat colour naming its
family, drawn opaque, which attributes coverage; `--draw-mode opaque` keeps each material and its
textures and turns blending and the alpha test off, which separates a surface that computes the
wrong colour from one whose colour is right and is combined wrongly. Both say in their own report
that they are not rendering results. Measured on frame 3800:

- **Coverage is five families**, and the attribution is clean -- the opaque map holds exactly five
  colours, all of them legend entries: `unlit_color` 129,628 px (45.2%), `textured_effect` 101,357
  (35.4%), `interpolated_registers` 40,036 (14.0%), `doubled_texture_pair` 15,270 (5.3%) and
  `lit_masked_specular` 429 (0.1%, the seagulls).
- **No family owns the white.** The saturated pixels fall across all four large families in similar
  proportion -- 51%, 37%, 45% and 29% of each one's own area -- so it is not one family's colour
  maths.
- **Turning blending off removes it.** With `--draw-mode opaque` the pure white falls from 43.7% to
  86 pixels (0.03%), and the frame becomes legible: sky, cloud billboards, a light shaft, a seagull.
  The over-brightness is therefore in how the title's draws are combined, not in what any one
  surface computes. (This mode shows only the last draw at each pixel, so it attributes the visible
  colour, not the whole stack that produced it.)

The publisher now reports the policy each family's draws carry, which names the combinations that
could be at fault and their sizes. Over one 1,400,000,000-block run, 854 frames, ~70 draws per frame:

    unlit_color/inverse_source_color/pass_all/0=854   lit_specular_color/premultiplied_alpha/pass_all/0=854
    unlit_textured: replace/pass_all/1=854  premultiplied_alpha/pass_all/0=854  additive/pass_all/0=854
    lit_textured: replace/pass_all/1=8540  replace/greater_or_equal_half/1=854
    lit_dual_alpha_effect/source_alpha_source_color/pass_all/1=854
    lit_alpha_tint/source_alpha/pass_all/0=854   lit_tinted_layered_specular/source_alpha/pass_all/1=3416
    lit_masked_toon/source_alpha/pass_all/1=17080
    lit_masked_specular: replace/greater_or_equal_half/1=4271  source_alpha/pass_all/1=4271
    textured_effect: source_alpha/pass_all/0=854  additive/pass_all/0=8540  additive/greater_than_64/0=854
    unlit_textured_effect/replace/pass_all/1=854   doubled_texture_pair/additive/pass_all/0=1661
    tinted_texture_sum/additive/pass_all/0=854    masked_doubled_texture/additive/pass_all/0=854
    interpolated_registers/additive/pass_all/0=807

**About sixteen of each frame's seventy draws blend additively**, and additive layers are what
saturate. Each of those policies is read from the title's own pixel-engine block rather than chosen,
so the next question is not whether the blend is right but what alpha and colour the shaders hand
it: an additive source whose alpha should be small and is 1 blows out exactly like this.

**`--draw-limit <n>` bounds how many of each frame's draws reach the sink**, which is what
attributes a defect inside a stack of blended draws to the draw that introduces it -- the finished
frame cannot, because every draw contributed to the pixel. The renderer owns when a frame begins and
the publisher owns what a draw is, so the count they share is its own object rather than a field one
reaches into. At `--draw-limit 35`, frame 3800 is **43.7% pure white -- the same as the unbounded
frame**, so the whole defect is introduced within the first thirty-five of that frame's seventy
draws. Bisecting that range named the draw:

| bound | 4 | 9 | 17 | 26 | 27 | 29 | 30 | 31 | 33 | 35 | unbounded |
|---|---|---|---|---|---|---|---|---|---|---|---|
| pure white | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.7% | **37.0%** | **43.7%** | 43.7% | 43.7% | 43.7% |

**`--draw-skip <n>` withholds each frame's leading draws**, which is what shows a named draw rather
than only naming it: with `--draw-limit 1` it leaves exactly one draw on an empty frame, the only
view in which that draw's own geometry and colour can be read instead of inferred from what it did
to what was already there. Draw 30 alone covers 82.9% of the frame and is 104,878 pixels of pure
white; 99.3% of the pixels it whitens were black before it, so it paints white rather than
saturating something.

That draw is not a renderer defect. `--draw-log-frame <n>` prints every draw of one frame with its
family, policy, mesh, texture and coordinate range, and it says draw 30 is `unlit_textured`,
premultiplied-alpha, drawing an **8x8 clamped texture whose channels run the whole 0-255 range**
over coordinates spanning `u=-0.04..1, v=-0.01..1` -- one of the sky's cloud layers, magnified over
the surface exactly as authored, with a texture that does contain white. The over-brightness
survives into the finished frame because the sky that follows it (draw 32, `unlit_color`,
`GX_BL_ONE`/`GX_BL_INVSRCCLR`, 1,800 vertices, full screen) cannot darken a white destination: that
blend leaves any destination it meets at one. 
`--draw-log-frame` also prints the colour each draw resolves to, read back out of the submitted
draw through the shipping `transform_vertex` rather than restated per material family. Draw 30
resolves to `r1 g1 b1, a0.2-1`: an intensity texture whose channels are equal, multiplied by a white
vertex colour with an authored alpha fade, composited with `GX_BL_ONE`. The console computes the
same thing from the same inputs, so the draw is faithful and the frame is still wrong -- which
places the defect in what the draw is composited *into*.

The sky that follows it (draw 32, `unlit_color`, `GX_BL_ONE`/`GX_BL_INVSRCCLR`, 1,800 vertices,
resolving to a blue gradient over the whole screen) is the other half. That blend leaves any
destination it meets at one, so it can only be authored to run over a dark buffer -- and it runs
after the cloud layers, over the white they left. **The remaining candidate is composition into the
wrong target**: GMSE01's title sequence makes EFB copies this renderer has no counterpart for, and a
pass meant for an offscreen sky texture, drawn into the main frame instead, looks precisely like
this. Measuring where the title copies the EFB within a frame is the next scope.

One real omission was found on the way and fixed. `UnlitTexturedMaterial` carried no authored
colour: its stage multiplies the texture by the raster colour, which is the vertex colour when the
channel takes one and the material's own register when it does not, and the pass substituted white
for that register. Its untextured sibling had carried `materialColorRgba8` all along, so the two
classifiers disagreed about the same input. The material now carries a `baseColor` and the pass
multiplies by it. It is not the cloud defect -- those materials resolve to white because their
vertex colour is white -- but every unlit textured surface GMSE01 authors a colour for was being
drawn at full strength.

Frame indices are **not** comparable across runs. Two runs of the same image reached different draws
at frame 3800; a bisection is only valid within one run's own numbering, and a frame named by index
must be re-listed in the run that produced it.

#### Texture-coordinate generation

Chasing that draw found a real and unrelated gap: **J3D texture matrices were not implemented at
all**. `title-adapter` read a material's texture-generation block for its coordinate count only, so
every generated coordinate was drawn with the values the vertices carried. GX never samples with
those: each generator names a source and usually a matrix, and a title puts every scale and scroll
in the matrix. Measured in the same frame, draw 28 scales its coordinates by two, draw 29 by four
with a -1.5/+0.475 offset, and draw 31 scales one coordinate by two and another by a half, each
with an offset that moves between frames -- drifting cloud layers, all drawn unscaled and unmoving.

`native_render::apply_j3d_tex_coord_generation` now produces them, and the matrix is supplied rather
than derived: J3D computes it every frame in `J3DTexMtx::calc` and leaves it in `mTotalMtx`, so
`title-adapter` hands that over instead of composing the SRT a second time.
`J3DTexGenBlockBasic::mTexCoord` (`0x08`, stride 4) and `mTexMtx` (`0x28`) are read from the guest,
and a `GX_IDENTITY` generator is kept distinct from an empty matrix slot so a slot the title never
filled cannot be read as an identity it never authored.

Measured over one 1,400,000,000-block run, 0 Dolphin alerts, every generator the title issued:

    generators by type/source: 1/4=53,690  1/5=9,394  1/6=8,540  10/19=20,572

All 71,624 matrix generators are `GX_TG_MTX2x4` over authored coordinate sets 0-2; there is no
`MTX3x4`, and no position- or normal-sourced generator, in this scene. The remaining 20,572 are
`GX_TG_SRTG` from `GX_TG_COLOR0` -- the toon ramp lookup, whose coordinate is a lighting result that
does not exist when a mesh is decoded and which the toon families already resolve in their own
fragment programs. Those are counted as deferred and their authored values left alone; refusing the
draw instead removed half the scene, which is how the distinction was found.

#### Where the title copies the embedded framebuffer

The console holds one framebuffer and a title may finish with it several times in a frame: render a
pass, copy the result into a texture, clear, and render the visible scene over the top. This
renderer composed every draw of a frame into one image, so an offscreen pass landed in the frame the
player sees -- which looks exactly like a blending defect and is not one.

`--read-efb <kind>:<hex-addr>[:<reports>]` measures those boundaries. `tools/gcnport_boot/
guest_efb_copy_probe.cpp` hooks the four GX entries that read the framebuffer -- `GXCopyTex`
(`0x8035ee5c`), `GXCopyDisp` (`0x8035ecec`), `GXSetTexCopySrc` (`0x8035e388`) and `GXSetCopyClear`
(`0x8035ea40`) -- names the kind on the command line rather than inferring it from the address, and
records for each copy how many of the frame's draws preceded it, what region it read, and whether it
cleared. Every entry still calls the original, so the title copies exactly as it did.

Measured over one 1,400,000,000-block run of the attract cycle, 0 Dolphin alerts:

    copy to texture: 1,708 entries; 854 cleared the buffer afterwards
      draws offered before the copy: 26=854  49..61=854
    copy to display: 4,005 entries; 4,005 cleared
    source regions: 0,0 256x256=854   0,0 640x448=854

**Every rendered frame is three passes, and the first boundary falls at exactly 26 draws.** The
256x256 copy that clears is the mirror stage: `TMarDirector::initECTMir`
(`decomp/sms/src/System/MarDirectorInitECT.cpp`) hands the mirror camera's own texture object to the
`JDrama::TEfbCtrlTex` named `鏡描画ステージ`, and `TEfbCtrlTex::perform` is the single site in the
title that issues `GXSetTexCopySrc`/`GXCopyTex`. The 640x448 copy that does not clear is
`通常シーン描画ステージ`, which fills `スクリーンテクスチャ`. The two independent readings agree:
the guest's own region sizes match the source the decomp names for each stage, and the clear flag
matches which of the two has to leave the buffer empty.

`GuestFrameRenderer::end_offscreen_pass` now honours that boundary. A copy that clears ends the
pass: the collected frame is sealed and dropped, and the next one starts empty, which is what the
console's buffer does. A copy that does not clear leaves the collection alone. The copy to the
display is deliberately not a boundary here -- the frame seam already seals and reopens the frame,
and ending a pass there would drop the visible image before anything encoded it.

**Over half of every frame's geometry was an offscreen pass.** The same run, with and without the
boundary: 59,530 models and 10,341,666 vertices reached the passes before, 37,784 models and
4,669,047 after.

**It is not the over-bright cause, and that is a measurement rather than an expectation.** Frame
3600 rendered with the boundary and without it is **byte-identical** -- 0 differing pixels of
286,720, 40.43% pure white either way. The mirror pass was entirely overwritten by the visible scene
that followed it, which is consistent with the earlier bisection finding 0.0% pure white at a bound
of 26 draws. The pass boundary is faithful and halves the work; the brightness enters after it.

What the boundary does **not** yet do is produce the texture. A dropped pass is not rendered into a
target a later draw can sample, so a material that reads the mirror or screen texture still reads
whatever it was bound to; the renderer's report counts the dropped and kept passes so that gap
cannot be mistaken for faithfulness.

The measurement named the next owner. A 256x256 copy out of a 640x448 framebuffer means the title
draws into a region smaller than the target, and this renderer has no viewport: every draw is
rasterised against the full 640x448.

#### Which part of the framebuffer each draw may reach

`--read-viewport <kind>:<hex-addr>[:<reports>]` reads both authored regions.
`tools/gcnport_boot/guest_viewport_probe.cpp` hooks `GXSetViewportJitter` (`0x80362fac`, the single
owner that `GXSetViewport` at `0x803630c8` delegates to, so one hook sees both routes exactly once)
and `GXSetScissor` (`0x80363138`), and records each rectangle, how many of the frame's draws
preceded it, whether the viewport's floats were whole pixels, and its depth range.

Reading the viewport needed a capability gcnport did not have. Its `GuestContext` exposed only the
general register file, and the ABI passes every floating argument in `f1` upwards -- so a probe on
`GXSetViewport` could read four registers the caller never wrote and nothing else.
`floating_register`/`set_floating_register` were added there (gcnport `f6f3625`), as doubles because
that is what the register holds whatever the callee declared. Its adapter test drives them against
the live `PowerPCState` with values chosen to be unrepresentable as singles, so an accessor that
narrowed through a float fails by name -- checked by making it do so.

Measured over one 1,400,000,000-block run of the attract cycle, 0 Dolphin alerts:

    viewport: 10740 entr(ies)
      rectangles, as left,top width x height = times set: 0,0 256x256=854 0,0 640x448=9616 0,0 640x480=270
      draws offered before the set: 0=7326 26=854 49=2 51=6 53=23 55=123 57=335 59=289 60=4 61=76
                                    62=12 64=46 66=32 68=214 70=668 72=578 74=152
      0 rectangle(s) were not whole pixels; depth range 0.000000..1.000000
    scissor: 15093 entr(ies)
      rectangles, as left,top width x height = times set: 0,0 0x0=1 0,0 256x256=854 0,0 640x447=3803 0,0 640x448=10434 0,0 640x480=1
      draws offered before the set: 0=9971 26=854 49=2 51=6 53=23 55=123 57=335 59=289 60=8 61=76
                                    62=24 64=92 66=64 68=428 70=1338 72=1156 74=304

**The only region smaller than the framebuffer is the mirror pass's, and it is the one already being
dropped.** Its 256x256 viewport and scissor are set 854 times each -- once per rendered frame,
before that frame's first draw -- and restored to 640x448 after exactly 26 draws, which is the same
boundary the framebuffer copy falls on, measured by a different instrument reading different
registers. Everything the visible scene draws is full-screen.

So the viewport is not the over-bright cause either. Both candidates the copy measurement raised are
now closed by measurement rather than by argument, and the brightness is in the shading or the order
of the visible pass. The probe is worth keeping: the 640x447 scissor (3,803 sets, one row short of
the buffer) and the single 0x0 scissor are authored details a renderer that ignores the scissor
cannot honour, and the depth range says the title never narrows it, which is what a renderer
assuming 0..1 needs to be true.

#### The console's clip depth is not this renderer's

`tools/gcnport_boot/draw_listing.cpp` gained two describers to answer where a draw's geometry
actually goes: `describe_clip_coverage` runs the shipping `transform_vertex` and reports the NDC
bounds, how many vertices fall behind the eye, each triangle's winding in the pipeline's own
clockwise-front convention, and the cull mode; `describe_transforms` prints the draw's model-view
and projection verbatim, because a model-view sitting at the eye and a projection with a wrong depth
row produce the same unusable clip position and no description of the result separates them.

What they reported, per 3D draw in one frame, is that **every vertex in the scene has a negative NDC
depth** -- the sky at -0.018..-0.00015, the clouds at -0.0033..-0.00006, the trees at -8.03..-0.044,
the sand at -0.121..-0.0004 -- and that several draws have every vertex behind the eye. The
projection read back is

    2.04163 0 0 0; 0 2.74748 0 0; 0 0 -3.33344e-05 -10.0003; 0 0 -1 0

which is exactly what `MTXFrustum` writes for a near plane of 10 and a far plane of 300000:
`-n/(f-n)` is -3.3334e-05 and `-(f*n)/(f-n)` is -10.0003. Both of GX's builders map the near plane
to clip z = -w and the far plane to 0. This renderer's pipelines clip and test depth over [0, w].

`native_render::with_zero_to_one_clip_depth` converts between them by adding the w row to the depth
row: the two ranges are the same size and one unit of w apart, so that is exact -- near becomes 0,
far becomes 1, with no scale and nothing chosen, and x, y and w untouched.
`native-render/tests/j3d_projection_test.cpp` builds the console's own frustum and orthographic
matrices from the SDK formulas at the title's own near and far planes and asserts both conventions
by evaluating the depth at each plane, that the conversion is a pure shift at the midpoint, and that
only the depth row changes. `title_adapter::read_guest_projection` applies it, so the conversion
happens at the single boundary where a console matrix becomes a renderer one, after the
canonical-entry check has run on the matrix as the title authored it; its test asks the shipping
conversion what to expect and separately asserts the console matrix is *not* what comes back, which
fails by name when the conversion is removed -- checked by removing it.

**This is a real defect fixed and it is not the over-bright cause.** Frame 3800 is 1.45% different
with the conversion than without, and 44.07% of it is still pure white. The reason the wrong
convention was not already fatal is the next finding: the pass leaves `enable_depth_clip` false, so
the pipeline was clamping depth rather than clipping it, and geometry the console would have removed
at the near plane was being drawn at the near plane instead.

#### Attributing a frame to the draws that made it

Two describers and a readback mode, because attributing one frame out of four thousand was costing
thirteen minutes an experiment.

`SemanticReadbackMode::NamedFrame` downloads the one frame a run names instead of every frame up to
it. A readback is a full-target download and a stall, and `EveryFrame` was paying for it on all 3,599
frames before the one wanted; naming the frame cuts a run from about thirteen minutes to about three.
Its control in `semantic_2d_pass_gpu_test.cpp` seals five frames, names the third, and asserts both
halves -- one sample, and that sample being frame three -- because sampling every frame and keeping
the third would report the same single observer call while costing every download. An off-by-one in
the frame it names fails that assertion, checked by making it off by one. A named frame the run never
reaches is its own failure message, separate from the one about pixels, so a run that stopped short
is not read as a renderer that drew nothing.

What that buys, on frame 3600 of the attract cycle: **the frame is 72 draws, and 26 of them are
Mario and FLUDD in a pass whose every vertex is behind its camera.** Draws 1-20 are
`lit_masked_toon` with a 32x32 skin texture, 21-24 are `lit_tinted_layered_specular`, and all of
them, plus the two after, project behind the eye under the 1.52357/2.0503 frustum -- which is a
different projection from the 2.04163/2.74748 one every later draw uses, and 26 draws is exactly
where the framebuffer copy and the 256x256 viewport both fall. So those 26 are the mirror pass,
measured a third way.

The remaining 46 draws are the sky, a handful of effect quads, part of the island, and the
seagulls. Mapping the family-map image's colours back through `family_color_map.cpp` says what
covers the frame: `UnlitColor` 45.2%, `TexturedEffect` 35.4%, `InterpolatedRegisters` 14.0%,
`DoubledTexturePair` 5.3%, `LitMaskedSpecular` 0.15% -- the last being the seagulls. The island's
own families cover nothing, and the sand's depth range says why it is not a transform defect: draw
38's NDC depth of 0.878..0.9996 is a ground plane running from 82 to 23,256 units ahead of the
camera, which is what a ground plane does. It is below the frustum in this frame, not misplaced.

#### The white is six draws, and what is missing is the scene

Isolating the frame by draw range answers it. Frame 3600 rendered whole is 40.45% pure white; draws
27-32 rendered alone are 40.35% of it, and draws 60-72 alone are a black frame. So six draws own
essentially all of the white, and the effect families that cover the frame are not the ones making
it bright.

Two candidates died here. Fog is off on every one of those six draws -- the draw listing now says so
per draw, printed for a draw whose fog is disabled as well as one whose fog is on, because a listing
silent about fog cannot be told from one that never read it. And depth clipping, switched on in the
same change, changes this frame by exactly zero pixels: the pipeline had been left on SDL's default
of clamping, which is not what the console does, so the field is now set and has its own control in
`semantic_3d_pass_gpu_test.cpp` -- a triangle behind the near plane and one past the far plane, both
of which have to be absent. Clamping draws them flattened onto the nearest plane, which is how that
control fails when the rasterizer state regresses. It is a correctness fix with no visible effect
here, and saying so is the point.

What the frame actually shows, looked at rather than measured: a cyan sky, four enormous white
glare quads, and five seagulls. **No island, no water, no logo, no Mario.** The scene is not
missing from the publisher -- draws 33-57 are `lit_textured`, `lit_masked_specular`,
`lit_alpha_tint` and `lit_dual_alpha_effect`, which is the island and its buildings. They are
missing from the *frame*, because of where their model-views put them: eleven of those draws have
every vertex behind the eye, and the ones in front land at NDC x up to 1820 or entirely below
y = -1. The view those matrices carry has a third row of `0.563 -0.809 -0.171`, which is a camera
tilted about 54 degrees upward -- so the sky fills the frame and the island sits below it.

Two things follow that are worth separating. The mirror pass's own matrices put its geometry 17,236
units behind its eye, which no camera does to the subject it is rendering, so the pose the reader
reconstructs for that pass is wrong rather than merely unused. And the glare quads carry model-views
with no rotation at all -- pure scale and translation, `60 0 0 79454; 0 60 0 81050; 0 0 80 -222832`
-- which is a draw matrix that never had a view concatenated into it. Both are questions about
`read_guest_shape_pose` and the draw-matrix palette it indexes, not about shading.

#### Asking the console, instead of reasoning about it

`extern/dolphin_fork` already carries a headless oracle, and it settles both. It records the
title's own FIFO at a named VI field, and `tools/oracle/parse_fifo_dff.py` decodes it without
linking Dolphin:

    ./extern/dolphin_fork/build/Binaries/dolphin-emu-nogui -u scratch/dolphin-user-pic \
      -e "$SUNBRIGHT_ROM" -p headless -v OGL -C Dolphin.DSP.Backend="No Audio" \
      -C Dolphin.Movie.DumpFrames=True -C Dolphin.Movie.DumpFramesSilent=True \
      --fifo-record scratch/oracle/pic.dff --fifo-record-after 8100 --fifo-record-frames 1

The recording exits at that field, and the frame dump beside it is an AVI the last frame of which
is the console's own image (`ffmpeg -sseof -2 -i <avi> -frames:v 1 out.png`). Game imagery stays in
`scratch/`; none of it is committed or leaves the machine.

What it says. Retail's title-screen frame is 1,258 GX primitives under exactly the two perspective
projections this renderer sees, in the same order: 653 under 1.52357/2.0503, then 567 under
2.04163/2.74748, then the ortho overlay. And the position matrix it loads for 37 of the second
pass's draws is, to every digit printed, the view this port reads out of `j3dSys`:
`-0.29029 0 -0.95694 305.422; 0.77384 0.58828 -0.23474 -1043.36; 0.56295 -0.80866 -0.17077
-353.411`. **So the camera pitched 54 degrees upward is the title's own, and the pose reader agrees
with the console.** The console's image confirms it from the other side: the title screen is sky,
with the sun in the upper right and gulls across it.

That reframes what is missing. Against the console's frame, this renderer has the sky's colour, the
gulls and the camera. It does not have the clouds, the logo, the palm tree, the sea, or a sun that
is a disc rather than a white field over two fifths of the frame. Those are the next targets, and
they are separate from each other -- not one transform defect behind all of them, which is what the
matrices had suggested.

Bisecting the white by single draw narrows it further. Of the six draws that own it, draw 27
contributes nothing, draw 28 contributes 15% of the frame at a mean of 3/255, draw 32 is the sky
itself -- a clean cyan gradient, mean 19,184,232, no white at all -- and **draws 29, 30 and 31 alone
are the whole 40.35%**, at a mean of 194 grey. They are the sky dome's own layers: all three share
the dome's model-view, a pure yaw rotation of the camera, and all three sample 8x8 textures. On the
console those layers are the clouds. Here they are white fields with soft edges, which is what an
additive draw of a bright texel over the whole dome looks like. Two things about them are worth
writing down before the next session touches them: draw 29's generated `v` runs from -1.997 to
-0.02, entirely outside the texture under a clamp wrap, so every sample it takes is the same edge
row; and an 8x8 image is a ramp, not a cloud, so which texture the layer resolves to is as
suspect as how it is sampled.

The run's own timeline is ahead of the console's, not out of step with it. Retail reaches this
title screen at VI field 8000; this runtime reaches it by field 5687, and at field 10000 both are
back to a single-quad movie frame. The scene is the same one -- the camera matrix is identical to
the digit -- so the two can be compared, as long as neither is indexed by time.


**GMSE01's geometry now reaches the renderer's own sink as a `native_render::ModelDraw`.** Every
part measured separately -- shape, pose, material, textures, stage light, projection -- is composed
into one draw by `tools/gcnport_boot/guest_draw_publisher.cpp` and submitted through
`submit_model`, the same entry the decomp runtime uses. Measured on the real title, 0 Dolphin
alerts:

    53,052 matrix group(s), 53,052 composed, 53,052 submitted, 9,733,890 vertex(es)
    53,052 reached the sink; of those rejected: 0 invalid draw, 0 invalid mesh, 0 mismatched mesh,
      0 unindexable pose, 0 mismatched images, 0 for no reason this knows
    0 rejected by the sink, 0 had no published projection,
      0 named a matrix slot the pose never filled
    element errors: none=44408 | mesh errors: none=44408 | pose errors: none=44408

Nothing is drawn yet -- the sink counts rather than rasterises -- but every value a renderer needs
is now present, consistent, and accepted by the boundary's own checks.

Three owners were extracted so the two runtimes share them instead of keeping parallel copies:
`title_adapter::read_guest_shape_geometry` (a matrix group's element, mesh and pose read together,
because the pose reader carries state across groups and a second caller in a different order would
silently lose the inherited slots), `native_render::build_j3d_mesh_vertices` (decoded vertices to
mesh vertices, with the matrix-slot remapping both runtimes must refuse rather than clamp), and
`native_render::j3d_material_image_views`. `sb::CapturedNativeJ3dMaterial` was a second structure
with `ClassifiedJ3dMaterial`'s fields, filled by copying them across one at a time; it is now an
alias.

A `ModelDraw` also carries a projection, and nothing supplied one.
`title-adapter/src/guest_projection.cpp` reads the matrix GMSE01 hands to `GXSetProjection`
(`0x80362c34`, matrix in r3, type in r4) and publishes it through
`native_render::publish_j3d_projection`. The entry point keeps only six of the sixteen values and
takes its offsets from column 2 for a perspective projection and column 3 for an orthographic one,
so the reader checks the ten the hardware discards against what it would have supplied and refuses a
matrix that disagrees rather than carrying values the console never used.

Measured on the real title: **19,283 projection sets, zero errors**, 6,835 perspective and 12,448
orthographic, 9 distinct. Every matrix the title authored is canonical, which is the evidence that
the column rule was read correctly -- a wrong reading would have refused one of the two types
wholesale. The orthographic scales are an independent check of the same thing: `0.003125` is exactly
`2/640` and `-0.00446429` exactly `-2/448`, the console's own framebuffer, and the reader was not
told either number.

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

**2026-09-19: the title screen's 2D composite is published, and the title now renders as a title
screen.** Everything a player would call the title screen -- the logo, the letters that fly into it,
the shine, "PRESS START!", the copyright line -- is not geometry. Each is one textured quad drawn by
a `J2DPicture`, through a path no model hook is on, so a run that published only models was not
rendering a partly-wrong title screen but a complete one with a whole pass missing.

Three owners, each tested on its own:

* `title_adapter::read_guest_picture` reads a pane (`guest_j2d_picture.{h,cpp}`) -- bounds, clip,
  both transforms, inherited opacity, binding/mirror/flip/wrap, black/white remap, four corner
  colours, the two packed blend constants, and each layer's `JUTTexture` with its `JUTPalette`.
* `title_adapter::read_guest_ortho_graph` reads the screen a pane is laid out in
  (`guest_j2d_graf_context.{h,cpp}`). It identifies the class by the recovered `J2DOrthoGraph`
  vtable `0x803e14b0`, **not** by the `unk4 == 1` discriminator `J2DPane::draw` itself uses: the
  base constructor never writes that field in retail, so a plain `J2DGrafContext` carries whatever
  its storage held.
* `native_render::plan_jut_texture` / `decode_jut_texture` (`jut_texture.{h,cpp}`) own what a
  `JUTTexture`'s fields mean -- which formats are owned, which samplers are legal, when a palette is
  required -- so the byte source is the only thing that differs between runtimes.

The boot tool consumes them through `--read-j2d-screen <addr>` (`J2DGrafContext::setup2D`,
`0x802eb6bc`) and `--read-pictures <addr>` (`J2DPicture::drawSelf(int, int, Mtx*)`, `0x802cc7c0`,
entered once `J2DPane::draw` has finalised the pane's transform, clip and opacity). Measured on the
real title, one 1,400,000,000-block run, 0 Dolphin alerts:

```
guest J2D context probe: 3400 setup(s), 3400 accepted, 1 distinct screen(s)
  graf context errors: none=3400
  J2D screen 640x448 at (0, 0) into viewport 640x448 at (0, 0)
guest picture probe: 13981 pane(s) drawn, 13981 submitted, 13981 accepted by the sink
  picture errors: none=13981
  not published: no_canvas=0 unreadable_transform=0 withheld_by_budget=0
                 unresolved_layout=0 invalid_blend_factor=0 no_sink=0
  textures: 31 decoded, 31 distinct, 1084364 byte(s); decode results: none=31
  0 pane(s) clip to less than their bounds
```

Frame 3600 (`scratch/render/j2d1.png`) draws the logo, the shine, the sun character, the palm tree,
the rainbow, `(C)2002 NINTENDO` and the fly-in letters over the sky and sea. That last counter is
the size of one deliberate gap: `J2DScreen::draw` is what decides whether a subtree clips to its
parent and this probe is not on it, so no clip is applied -- and at the title no pane asks for one.

Gap: the sun is missing. Retail's object 27 of 127 is a large additive glow in the upper right
(`scratch/oracle/objiso/obj27.png`); our frame has clear sky there. It is a 3D draw in the sky pass,
so it is on the model path and not this one. `native_render::ModelDraw` also has no place for the
normal matrix, which under the CPU pipelines genuinely differs from the position matrix; the adapter
carries it as `GuestShapePose::normalViews` and the semantic boundary has yet to accept it. Offscreen
passes are dropped rather than rendered into a target a later draw could sample, so a material that
reads one still reads whatever it was bound to. Non-billboard particles, image producers, screen
effects and full-frame ordering remain incomplete. The surviving decomp-side adapters
(`sms-boot/runtime/native_j3d_adapter.cpp` and its peers) remain native/decomp evidence attached to
the decomp product rather than to `gcnport`; they are not built. The JIT seam must preserve the same
value-only contract; no old body or GX compatibility path may become a silent fallback after its
semantic owner is proven.

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

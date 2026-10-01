---
id: 37
title: Migrate Sunbright execution to gcnport Dolphin dynarec
status: open
symptom: The intended native/dynarec product is not runnable: the boot and JIT paths now work, but no gameplay target consumes gcnport's executor, and J3DShape::draw at 0x802e0390 is still only observed, never replaced by a native override.
state_items: S001,S002,S003
tags: migration,gcnport,dolphin,jit,override
created: 2026-09-04
updated: 2026-10-01
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

## Current state

`extern/gcnport` is a pinned submodule (gcnport `185d616`, Dolphin fork `f5e7b38e16`) wired as an
`EXCLUDE_FROM_ALL` CMake subdirectory by `tools/gcnport_boot/CMakeLists.txt` and
`cmake/GcnPortDependency.cmake`, linking Dolphin's own `core`/`uicommon` targets and compiling
Dolphin's reusable `Source/UnitTests/StubHost.cpp` rather than a bespoke Sunbright copy. Exact
`GMSE01` (gitignored `scratch/bin/sms.dol`, load `0x80003100`, entry `0x8000522c`) boots and runs
its attract cycle: 13,595 JIT blocks compiled, 0 invalid guest accesses, 1,496 VI retraces
delivered, decoding its opening movie.

Verified on the real title, in the order the gaps were closed:

- **Boot registers.** `apply_gamecube_os_init` applies `CBoot::SetupGameCubeBS2Registers` (MSR/HID/
  BAT). GMSE01's own `__start` sets `r1`/`r2`/`r13` from the DOL's immediates, so no caller may
  guess a stack pointer.
- **MMIO and devices.** `BootAuthenticatedImage` never called `HW::Init()`, so no `MMIO::Mapping`
  handler table existed and the title's own `__init_hardware` write to PI `0x0C003004` faulted on a
  null `m_WriteFunc`. Dolphin's frontend brings up the video backend and DSP emulator *around*
  `HW::Init`; `apply_media_init` does both in their headless forms plus `Fifo::Prepare`, pinned to a
  single core with the calling thread as GPU thread so the one-block contract holds.
- **Guest timer.** `ExecuteJitBlock` forced `ppc_state.downcount = 1`. `CoreTiming::Advance()`
  derives elapsed time as `slice_length - DowncountToCycles(downcount)`, so the sentinel froze the
  global timer at 30,891 ticks across 16,384 dispatches and `__OSInitAudioSystem`'s
  `while (!(__DSPRegs[5] & 0x20))` never exited. The bound belongs on the *slice*; a permanently
  future no-op event caps `slice_length` at one block. `MAIN_ENABLE_DEBUGGING` was rejected: it
  forces single-instruction blocks. `ExecuteJitBlocks(minimum_blocks)` lifts the cap for a batch
  instead — hardware events still size every slice, and blocks report themselves from inside
  generated code so the ledger stays complete. ~9.9M blocks/s batched vs ~180k stepped.
- **Disc.** `GameCubeBootOptions::disc_image_path` mounts a disc the *consumer* names (only a path
  crosses the API, so no game image enters gcnport) and runs `CBoot::DVDReadDiscID`;
  `run_apploader` runs the mounted disc's own apploader through `CBoot::LoadGameCubeDiscViaApploader`.
  Without the apploader the FST is null, `DVDConvertPathToEntrynum` reads `0x00000008`, and every
  file lookup walks garbage: 335,405,984 invalid guest accesses, against 19 with the apploader and 0
  once Dolphin's `Data/Sys` (IPL fonts, DSP ROM) is visible to `File::GetSysDirectory()`.
- **Region.** `CBoot::SetupGCMemory` writes the guest video format at `0x800000CC` from `SConfig`'s
  region. Left `Unknown`, `DiscIO::IsNTSC` reads PAL, so `TDisplay` (which sits in the block right
  after the title's `0x804c8d80..0x8056dd80` framebuffer) was built with `xfbHeight = 530` against a
  640x448 allocation and its display copy scribbled over the live object. gcnport now takes the
  region from the volume exactly as `SConfig::SetPathsAndGameMetadata` does, and applies only
  Dolphin's shipped global `Sys/GameSettings` layer — never the user's per-title INI. The disc is
  opened before any global state is touched, because `Config::AddLayer`'s notification reaches
  VideoCommon through a frontend CPU thread this adapter does not have.
- **Logging.** `Common::Log::LogManager::IsEnabled` faulted with a null `this` on Dolphin's DVD
  thread at the first FST-backed file read; gcnport now owns the log manager with an empty Base
  config layer, because `ConfigLoaders::GenerateBaseConfigLoader()` reads and writes the user's
  `Dolphin.ini`.
- **Fallbacks.** A 4-billion-block run recorded 1,109,684 fallback events across 22 sites, none
  untracked, and every one is an `mfspr`/`mtspr` on a Gekko SPR Jit64 hands to the interpreter by
  design: `0x803438f8` `mtspr DMAL` (1,099,470), `0x80341ab0` `mtspr DEC` (10,093), and two single
  events in the OS exception path with translation off. 99.97% of block executions are compiled code;
  there is no unimplemented-instruction gap behind the number.
- **First seam.** `--count-calls` installs a native hook that counts entries and returns
  `RunOriginalOnce`, so the translated body still runs. In one 4B-block run: `J3DShape::draw`
  (`0x802e0390`) 245,874, `J3DModel::entry` (`0x802dedc8`) 106,615, `TApplication::drawDVDErr`
  (`0x802a5b50`) 12,538, `TApplication::gameLoop` (`0x802a5f5c`) 9. `gameLoop` is the per-app-state
  loop, so 9 is nine director transitions; `drawDVDErr` is a per-frame drive-status check that
  returns 0 and draws nothing on a healthy drive
  (`decomp/sms/src/System/Application.cpp:857`), which is why a shorter run counting 1,428 entries
  to it and 0 to `J3DShape::draw` is the opening movie, not a stalled game. `gpApplication`
  (`0x803e9700`) agrees from the other side: `mAppState` 4 (`APP_STATE_DONE`, `mMovie = 9`
  `Entrance.thp`) at 600M blocks, 6 (`APP_STATE_MOVIE`, `mMovie 12`, all areas 15) at 4B.

Native overrides still reach the guest: 1,428 complete native → original → native round trips
through `TApplication::drawDVDErr`, each native caller reading the body's own return value.

## Acceptance

- `gcnport` owns image generations, runtime hooks, original calls, bounded exits, invalidation, and
  execution counters without title addresses;
- the runtime dispatcher reaches `J3DShape::draw` at `0x802e0390`, submits the existing semantic
  J3D value, and runs the original body through one-call override suppression;
- controls exercise hook hit/miss, enabled/disabled, cache hit/miss, chaining, and hook-change
  invalidation; and
- link/selector inspection proves the gameplay target includes neither an interpreter nor generated
  guest code.

This resolves the first wiring discriminator only. Representative gameplay is S008.

## Next

1. A gameplay composition target that consumes gcnport's executor, replacing the refusal in
   `tools/launch/run.py`.
2. A native override that *replaces* guest behaviour at `0x802e0390` rather than observing it.

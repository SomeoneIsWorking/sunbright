# Path C step 4 — Dolphin video backend needs Core::System, not standalone

**Date:** 2026-07-04
**Context:** Path C = dual render sinks in sms-boot (NATIVE_PC = SDL3 GPU, GX_ORACLE
= Dolphin videovulkan). Steps 1-3 shipped (commits `5c96977`, `0031b93`): Engine
runtime toggle, dispatch in `present_hook`, oracle sink stub. Step 4 was to make
the oracle sink actually route to Dolphin's Vulkan backend.

**Finding:** Dolphin's `VideoBackend::Initialize` CAN be called from sms-boot —
gets as far as bringing up the Vulkan instance / physical device / logical device
— but its downstream `InitializeShared` (VideoBackendBase.cpp:302) hard-depends
on `Core::System` subsystems being alive:

```cpp
auto& system = Core::System::GetInstance();
auto& command_processor = system.GetCommandProcessor();
command_processor.Init();
system.GetFifo().Init();
system.GetPixelEngine().Init();
BPInit();
VertexLoaderManager::Init();
system.GetVertexShaderManager().Init();
system.GetGeometryShaderManager().Init();
system.GetPixelShaderManager().Init();
system.GetXFStateManager().Init();
TMEM::Init();
```

These are normally brought up by `BootManager::BootCore` (Core/BootManager.cpp)
as part of booting an emulator. There is no standalone-video init path.

**Concrete crash reached during probe:** SEGV inside `TextureCacheBase()` ctor —
called from `VideoBackendBase::InitializeShared` — before `Core::System::GetInstance()
.GetCommandProcessor().Init()` etc. run. Backtrace (addr2line):

```
TextureCacheBase::TextureCacheBase()   ← crashes here
VideoBackendBase::InitializeShared(...)
Vulkan::VideoBackend::Initialize(WindowSystemInfo const&)
sb_oracle_present_frame                ← our sink entry
```

**Preconditions that DID work (documented for the next attempt):**
1. Link `videovulkan videocommon core uicommon common` into sms-boot (root CMake
   build gets access to Dolphin targets via `add_subdirectory(native)`).
2. Provide no-op `Host_*` / `Discord::*` symbols (`native/src/dolphin_host_shims.cpp`).
3. `Common::SetEnableAlert(false)` to prevent PanicAlert's Y/N stdin prompts from
   blocking on assertions.
4. `UICommon::SetUserDirectory(<scratch-dir>)` before `UICommon::Init` — otherwise
   `IOS::FS::BuildFilename` asserts on empty `m_root_path`.
5. `UICommon::Init()` then `UICommon::CreateDirectories()` — brings up Config
   layer stack, SConfig, g_Config, LogManager. Runs cleanly.
6. `Config::SetBase(Config::MAIN_GFX_BACKEND, "Vulkan")` — otherwise
   `PopulateBackendInfo` (VideoBackendBase.cpp:265) reads the config default
   ("OpenGL") and overwrites the backend selection.
7. `VideoBackendBase::PopulateBackendInfo(wsi)` — activates the Vulkan backend
   properly, prints `[oracle] backend activated: Vulkan`.
8. `g_video_backend->Initialize(wsi)` — enters Vulkan-side init, brings up the
   VkInstance + VkDevice, then crashes in `InitializeShared`.

Compile-time requirement: the two Dolphin-touching TUs (`oracle_present.cpp`,
`dolphin_host_shims.cpp`) must build at `-std=c++23` — Dolphin's
`Common/BitUtils.h` uses `std::to_underlying`. Rest of sms-boot stays at C++17.
See `set_source_files_properties(... COMPILE_OPTIONS "-std=c++23")` in
`native/CMakeLists.txt`.

**Strategic decision required from user before step 4 continues.** The clean
path to a real oracle sink is one of:

- **A. Bring up Core::System without booting PPC.** Init the subsystems `BootCore`
  inits, but skip the JIT and PPC execution. Multi-day arc, requires reading
  Core/BootManager.cpp end-to-end. Highest fidelity — same pipeline as
  `build/sunbright`.
- **B. Actually call `BootManager::BootCore` with a dummy ROM.** Boot Dolphin
  fully, then hijack the video pipeline to draw sms-boot's game state instead
  of the JIT'd guest's. Heaviest, drags in DSP/audio/pad emulation too.
- **C. Shell out to `build/sunbright`.** Serialize sms-boot's captured GX state,
  spawn `build/sunbright` in a helper mode that consumes it, get pixels back
  via file. Fastest to ship but violates the "same binary" spirit of Path C.
- **D. Park the oracle sink as a stub.** The scaffolding is done; NATIVE_PC is
  the ship path anyway. Pivot back to the b3/b4 near-clip triangle fix.

Recommendation: D for now (pivot back to actual parity work), revisit later
with A when there's a specific defect the parity harness can't isolate.

**What ships from this arc even without a working oracle sink:**
- `sb::engine::mode()` runtime toggle (`SB_RENDER=native|oracle`).
- Root-cmake unified build producing both `sunbright` and `sms-boot`.
- Weak-symbol dispatch in `present_hook` — oracle stub swappable without
  touching the ship path.
- Documented Dolphin init sequence for step 4 continuation (this file).

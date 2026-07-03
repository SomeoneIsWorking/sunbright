# Path C step 4 — Dolphin video backend brings up cleanly in sms-boot

**Date:** 2026-07-04
**Superseded my earlier claim in this same file that "Dolphin video needs Core::System"** —
that was a wrong diagnosis. The real block was a 4-byte-aligned `operator new` overriding
libc's 16-byte-aligned malloc in sms-boot, which SIGSEGV'd Dolphin's SSE-aligned members
during `TextureCacheBase` zero-initialisation. Fixing the override let
`Vulkan::VideoBackend::Initialize` (and everything downstream through `InitializeShared`)
succeed on the very next run — no Core::System boot needed.

Keeping the note so future me does not re-derive the wrong architectural conclusion.

## What actually happened

`operator new(size_t)` in `reference/sms/src/JSystem/JKernel/JKRHeap.cpp` was:
```cpp
void* operator new(size_t byteCount) { return sb_plain_new(byteCount, 4); }
```
A 4-byte default alignment is NON-CONFORMANT for x86_64 (C++ spec requires
`alignof(std::max_align_t)` = 16). The game's own C++ objects survived because they don't
use SSE-aligned fields. Dolphin's `TextureCacheBase` has `alignas(16) u8* m_temp` and gets
zero-initialised by the compiler using `movaps` (16-byte-aligned SSE store). If `this` is
4-byte-aligned instead of 16, `movaps %xmm1, 0xb0(%rdi)` hits an unaligned address and
SIGSEGVs mid-ctor.

gdb symptom:
```
Thread 3 "sms-boot" received signal SIGSEGV, Segmentation fault.
0x00000000007e74aa in TextureCacheBase::TextureCacheBase() ()
=> 0x7e74aa <_ZN16TextureCacheBaseC2Ev+186>:  movaps %xmm1,0xb0(%rdi)
rdi = 0x7fffdd1af854   ← 4-byte-aligned this pointer
```

## Fix

`reference/sms/src/JSystem/JKernel/JKRHeap.cpp`:
```cpp
void* operator new(size_t byteCount)   { return sb_plain_new(byteCount, 16); }
void* operator new[](size_t byteCount) { return sb_plain_new(byteCount, 16); }
void* operator new(size_t bc, int a)   { return sb_plain_new(bc, a < 16 ? 16 : a); }
void* operator new[](size_t bc, int a) { return sb_plain_new(bc, a < 16 ? 16 : a); }
```
Default is now 16, matching libc. `alignas(N)`-annotated types with N > 16 still take the
explicit-alignment overload with `a` == their own alignment.

## Confirmed working end-to-end (2026-07-04)

```
[oracle] sink active
[oracle] attempting standalone Dolphin video backend init...
[oracle] backend activated: Vulkan
[oracle] calling VideoBackend::Initialize...
[oracle] VideoBackend::Initialize returned 1
[oracle] Dolphin video backend UP (headless). Real rendering wires in step 4b.
```

Stable across SB_WATCHDOG_SECS=0 runs — no crash after init. `Core::System::GetInstance()`
returns a singleton whose sub-managers (`CommandProcessor`, `Fifo`, `PixelEngine`,
`VertexShaderManager`, etc.) exist as members and get `.Init()`'d successfully inside
`InitializeShared` — they don't require an emulator boot to construct.

## Preconditions (the ones that DID matter, kept from the original entry)

1. Link `videovulkan videocommon core uicommon common` into sms-boot (root CMake build).
2. `Host_*` / `Discord::*` no-op stubs (`native/src/dolphin_host_shims.cpp`).
3. `Common::SetEnableAlert(false)` — otherwise PanicAlert prompts block on stdin.
4. `UICommon::SetUserDirectory(...)` before `UICommon::Init` — FS backend asserts on empty root.
5. `UICommon::Init()` + `UICommon::CreateDirectories()`.
6. `Config::SetBase(Config::MAIN_GFX_BACKEND, "Vulkan")` — `PopulateBackendInfo` re-reads
   the config, so setting the config value is what actually picks Vulkan (not `ActivateBackend`).
7. `VideoBackendBase::PopulateBackendInfo(wsi)`.
8. `g_video_backend->Initialize(wsi)`.

Compile-time: `-std=c++23` on the two Dolphin-touching TUs (`oracle_present.cpp`,
`dolphin_host_shims.cpp`) — Dolphin's `Common/BitUtils.h` uses `std::to_underlying`.

## Next arc (step 4b): feed captured GX state into Dolphin's video pipeline

Backend is up; nothing renders yet because the sink stub doesn't submit anything. The
minimal wire-up is:
- Route sms-boot's captured BP/XF/CP state into Dolphin's `g_main_cp_state` +
  `xfmem` + BPMemory. sms-boot already captures per-material into `NgxTevState`
  and per-batch geometry into `NvkTevBatch` / imm-mode buffers.
- Populate a per-frame `AbstractFramebuffer` (or use the swap chain's) and call
  the equivalent of `Presenter::Present()` at end-of-frame.
- Alternatively (heavier but higher fidelity): serialize each GX call as a Dolphin
  FIFO command byte stream, feed it into `system.GetFifo()`; Dolphin's OpcodeDecoder
  drives the backend the same way it does for a real emulator.

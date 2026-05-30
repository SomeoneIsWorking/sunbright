# Dolphin Integration

## Why Dolphin as a submodule?

We reuse Dolphin's hardware emulation so we don't have to reimplement:
- GX (GPU) rendering — Dolphin's VideoBackend handles GX command streams
- DSP/audio — Dolphin's DSP core + HLE
- EXI (memory card, clock) — Dolphin's EXI/IPL
- SI (serial interface, controllers) — Dolphin's SI
- MemMap — Dolphin's 24MB + ARAM + MMIO address space

We **replace** Dolphin's CPU emulator (JIT or interpreter) with our native code.

## Dolphin libraries we use

| Library | Header path | Purpose |
|---|---|---|
| `discio` | `DiscIO/Volume.h` | Read RVZ/ISO/GCM disc images |
| `core` | `Core/HW/Memmap.h` | Memory access |
| `core` | `Core/PowerPC/PowerPC.h` | PowerPCState struct (for state sync) |
| `videobackends` | (via `core`) | GX rendering |

## CMake configuration

The Dolphin submodule is added with `add_subdirectory(externals/dolphin)`.
We set `ENABLE_QT=OFF`, `ENABLE_WXWIDGETS=OFF` to avoid GUI dependencies.

Key targets from Dolphin:
- `discio` — standalone, no Core dependency
- `core` — the main Core library (big, pulls in VideoBackend, DSP, etc.)

## JIT hook mechanism

The goal: when Dolphin's JIT is about to compile address X, check if we have it
in `g_recomp_map`. If yes, call our native function instead.

### Approach A — Compile-time hook (preferred)
Add a callback point to `Source/Core/PowerPC/JitCommon/JitBase.cpp`:
```cpp
// In JitBase::Compile:
if (auto fn = SunbrightHook::Lookup(address)) {
    fn(SunbrightHook::GetCPUState());
    return;
}
// ... normal JIT compilation
```

This requires a small patch to Dolphin. We maintain it as a patch file:
`externals/dolphin_patches/0001-sunbright-jit-hook.patch`

Apply with: `git -C externals/dolphin apply ../../externals/dolphin_patches/*.patch`

### Approach B — Compiled-in shim (current fallback)
The generated code compiles to a shared library. A Dolphin plugin loads it and
calls `recomp_lookup()` before dispatching. Less efficient, but no Dolphin patch needed.

## State synchronization

Every time we cross between recompiled code and Dolphin's JIT (e.g. calling an
unrecompiled function), we sync CPU state:

```
Recompiled frame:  CPUState (our struct, stack-allocated)
Dolphin frame:     PowerPC::ppcState (Dolphin's global state)
```

`dolphin_state_to_cpu()` and `cpu_to_dolphin_state()` in `dolphin_hook.cpp`
handle the conversion. Cost: ~300 ns per boundary crossing. Minimize by
recompiling as much code as possible.

## Building without Dolphin (standalone mode)

Set `SUNBRIGHT_USE_DOLPHIN_DISCIO=OFF SUNBRIGHT_USE_DOLPHIN_CORE=OFF`:
- Recompiler falls back to raw GCM/ISO parsing (no RVZ support)
- Runtime uses a flat 24MB RAM buffer (no real GX/DSP/audio)
- Good for testing the recompiler output logic without full Dolphin build

Convert RVZ → ISO first if not using Dolphin DiscIO:
```
DolphinTool convert -i "Super Mario Sunshine (USA).rvz" -o sms.iso -f iso
```

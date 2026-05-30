# Sunbright — GameCube Static Recompiler

## What this project is
Static recompiler for Super Mario Sunshine (GameCube/PowerPC) → native PC binary.
ROM: `$SUNBRIGHT_ROM`

Pipeline: RVZ → extract DOL → decode PowerPC → emit C → compile → native .so
Runtime: Dolphin subsystems (GFX/DSP/Memory/Input) drive the native code via hook layer.

## Architecture overview

```
tools/recompiler/    Offline tool: ROM → generated/functions.cpp + jump_table.cpp
runtime/             Dolphin integration layer (JIT hook, memory bridge, OS HLE)
generated/           Output of recompiler — gitignored, regenerate with /recompile
externals/dolphin/   Dolphin git submodule — do NOT modify
docs/                Living docs — update whenever architecture changes
```

## How to self-update this file
When you discover new constraints, add patterns, fix edge cases, or change architecture:
1. Update the relevant section below
2. Update `docs/` accordingly
3. Run `/update-docs` to sync

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON
cmake --build build -j$(nproc)
```

Or use `/build` skill.

## Running the game

```bash
cmake --build build --target sunbright-generated  # build recompiled .so first
./build/sunbright [rom.rvz] [libsms_recomp.so]
# defaults: ROM=$SUNBRIGHT_ROM
#           lib=build/libsms_recomp.so
```

F11 toggles fullscreen. X11 and Wayland both work (SDL2 auto-detects).

## JIT hook (no Dolphin patches required)

`runtime/jit_hook.cpp` intercepts via linker `--wrap` on `_Z13JitTrampolineR7JitBasej`:
```
JitAsm → __wrap_JitTrampoline (our hook)
           ├─ IsRecompiled? → SunbrightBridge::Run() → native .so
           └─ else         → __real_JitTrampoline  → Dolphin JIT
```

## Skills (slash commands)
| Command | What it does |
|---|---|
| `/recompile` | Run recompiler on the ROM → regenerate `generated/` |
| `/analyze-rom` | Dump DOL info, function list, disc structure |
| `/build` | CMake configure + build |
| `/update-docs` | Regenerate docs from current code state |
| `/patch-func ADDR` | Mark PPC address as manually patched (skip recomp) |

## Instruction coverage status
Update this table as ppc_decoder.cpp gains coverage:

| Category | Status | Notes |
|---|---|---|
| Integer (addi, add, sub, etc.) | ✅ | |
| Branches (b, bl, bc, bclr, etc.) | ✅ | |
| Load/Store integer | ✅ | |
| Compare (cmp, cmpi, cmpl, cmpli) | ✅ | |
| Logical (and, or, xor, etc.) | ✅ | |
| Rotate/Shift (rlwinm, slw, srw, etc.) | ✅ | |
| FP single (fadds, fsubs, fmuls, etc.) | ✅ | |
| FP double (fadd, fsub, fmul, etc.) | ✅ | |
| CR ops (crand, cror, etc.) | ✅ | |
| SPR (mtspr, mfspr, mflr, etc.) | ✅ | |
| Paired singles (ps_*, psq_l/st) | 🔄 | GC-specific, critical for SMS |
| System calls (sc) | ✅ | HLE via Dolphin |
| Cache ops (dcbt, icbi, etc.) | ✅ | NOP in recomp is fine |
| lmw / stmw (multi-word load/store) | ❌ | 6398 instances in SMS — implement next |
| mftb (time base read) | ❌ | Return fake counter |
| mffs / mtfsf / mtfsb1 (FPSCR) | ❌ | Low priority |
| psq_lx / ps_cmpo0 (indexed PS) | ❌ | Add to opcode 4 decoder |
| fcmpo | ❌ | Ordered FP compare — trivial |

## Known SMS-specific patterns
- Heavy psq_l/psq_st usage for position/velocity data — do not NOP these
- Display lists at 0x80XXX — compiled via GX bridge
- JIT-modifiable code: none confirmed yet
- OSReport → maps to printf in runtime

## Adding a new instruction
1. Add decode case in `tools/recompiler/ppc_decoder.cpp`
2. Add emit case in `tools/recompiler/c_emitter.cpp`
3. Add intrinsic in `runtime/intrinsics.h` if needed
4. Update table above

## Dolphin integration notes
- We hook `JitInterface` to intercept compiled blocks
- When block address is in `g_recomp_table`, call native function instead of JIT
- Dolphin still handles: VideoBackend, DSP, EXI, SI, MemMap
- Do NOT touch `Source/Core/PowerPC/` — that's what we're replacing

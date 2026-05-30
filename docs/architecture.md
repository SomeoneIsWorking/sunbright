# Sunbright Architecture

## Overview

Sunbright is a static recompiler for Super Mario Sunshine (GameCube/PPC) that produces
a native PC binary by translating PowerPC machine code to C, then compiling it with a
standard C++ compiler.

```
ROM (.rvz)
   │
   ▼ [DiscIO]
DOL executable (PPC binary)
   │
   ▼ [sunbright-recomp]
generated/functions.cpp   — one C++ function per PPC function
generated/jump_table.cpp  — address → function pointer map
   │
   ▼ [g++ / clang++]
libsms_recomp.so          — native shared library
   │
   ▼ [sunbright-runtime + Dolphin]
Running game!
```

## Components

### 1. sunbright-recomp (offline tool)
- Reads the ROM via **Dolphin DiscIO** (handles RVZ, ISO, GCM)
- Parses the **DOL executable** (GameCube's ELF-equivalent)
- Scans for function boundaries (bl targets, linear scan)
- Decodes every PowerPC instruction (`ppc_decoder.cpp`)
- Emits equivalent C++ code (`c_emitter.cpp`)
- Outputs `generated/functions.cpp` + `generated/jump_table.cpp`

### 2. generated/ (auto-generated)
- `functions.cpp`: Each PPC function becomes `extern "C" void func_XXXXXXXX(CPUState&)`
- `jump_table.cpp`: `g_recomp_table[]` — address → function pointer, loaded by runtime
- Do not edit manually; re-run `/recompile` after changing decoder/emitter

### 3. sunbright-runtime (shared library)
- **memory_bridge**: Routes effective addresses to Dolphin's MemMap (or a flat buffer)
- **dolphin_hook**: Installs into Dolphin's JIT interface; dispatches to recompiled code
- **os_hle**: GameCube OS high-level emulation (OSReport, time, etc.)
- **intrinsics**: Inline helpers for psq_dequantize, rotl32, carry flags, etc.

### 4. Dolphin (submodule)
- **DiscIO**: Reads RVZ compressed disc images (used by recompiler)
- **VideoBackend**: Renders GX commands (unchanged — game issues GX commands same way)
- **DSP**: Audio (unchanged)
- **EXI / SI**: Memory card, controller (unchanged)
- **JIT**: Fallback for any unrecompiled code (REL modules before they're recompiled)

## Data flow at runtime

```
Dolphin startup
   │
   ├─ Load ROM via DiscIO
   ├─ Initialize VideoBackend, DSP, EXI, SI
   ├─ Load libsms_recomp.so → populate g_recomp_map
   └─ dolphin_hook_install()

Game runs:
   Dolphin JIT hits address X
      │
      ├─ X in g_recomp_map? → YES → call func_X(cpu_state); return
      └─ NO → Dolphin JIT compiles and runs X normally
```

## Emitted C++ pattern

```cpp
// PPC: addi r3, r0, 0x1234   (li r3, 0x1234)
cpu.gpr[3] = 0x1234;

// PPC: lwz r4, 0x10(r3)
cpu.gpr[4] = MEM_R32(cpu.gpr[3] + 0x10);

// PPC: bl sub_80243B00
cpu.lr = 0x80243ABC + 4;
func_80243B00(cpu);

// PPC: blr
return;

// PPC: ps_madd f1, f2, f3, f4
cpu.fpr[1].ps0 = cpu.fpr[2].ps0 * cpu.fpr[3].ps0 + cpu.fpr[4].ps0;
cpu.fpr[1].ps1 = cpu.fpr[2].ps1 * cpu.fpr[3].ps1 + cpu.fpr[4].ps1;
```

## SMS-specific notes

- **REL modules**: SMS loads `.rel` files for each stage/object. These need separate
  recompilation. The recompiler will handle them once symbol relocation is implemented.
- **Paired singles**: SMS uses `psq_l/psq_st` extensively for position/velocity.
  GQR registers are tracked in CPUState and correctly handled.
- **Display lists**: Compiled via Dolphin's GX backend — no changes needed.
- **JIT-modifiable code**: Not confirmed in SMS. Monitor if issues arise.

## Next steps (in priority order)
1. Get Dolphin DiscIO linking correctly in CMake
2. Verify DOL extraction works on the actual RVZ
3. Run analyze-only mode; check opcode coverage
4. Fix any missing opcodes from the histogram
5. REL module recompilation
6. Dolphin JIT hook implementation

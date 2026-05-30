# Manually patch a function (skip recompilation)

Use when a PPC function can't be cleanly recompiled and needs a hand-written C++ replacement.

## Usage
```
/patch-func 0x80243ABC "short description of what this function does"
```

## Steps

1. Create `runtime/patches/func_$ADDR.cpp`:
   ```cpp
   // Manual patch for PPC function at $ADDR
   // Reason: $DESCRIPTION
   #include "../cpu_state.h"
   #include "../intrinsics.h"
   
   extern "C" void func_$ADDR(CPUState& cpu) {
       // Hand-written replacement
   }
   ```

2. Add the address to `runtime/patches/patch_list.h`:
   ```cpp
   PATCH(0x$ADDR, func_$ADDR)
   ```

3. The recompiler will skip this address when regenerating `generated/`
   (patch_list.h takes precedence over generated jump_table.cpp)

4. Update CLAUDE.md — add the patched function under a "## Manually patched functions" section

5. Document WHY the function needed patching (indirect branch? self-modifying? hardware-specific trick?)

## Common reasons to patch
- Function uses computed indirect branches that aren't resolvable statically
- Function touches hardware registers directly (MMIO) — replace with Dolphin API call
- Function is a memcpy/memset variant — replace with C stdlib version
- Floating-point edge case that our emitter gets wrong

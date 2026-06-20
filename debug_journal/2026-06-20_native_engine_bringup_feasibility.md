# 2026-06-20 — Native PC engine from the decomp: feasibility measured, bring-up started

## Direction (user, 2026-06-20): build a TRUE native PC game engine
Recreate the SMS engine as native PC code from the `reference/sms` decompilation. **No GameCube stuff** —
no PPC interpretation, no Dolphin JIT for gameplay, no GX/EFB. No "flip" (the retired `port/` host-layout
bridge — `docs/DO_NOT_REVISIT_FLIP.md`; do NOT revisit). The model: compile the decomp's C++ to native
structs, byte-swap assets ONCE at load, native subsystems (renderer = existing `ngx`, audio = existing
`native_jas`, + input/OS-shim/filesystem) under the game's own logic.

## Feasibility — MEASURED on real data (not a vibe)
With two tiny shims (`native/shim/dolphin/types.h` fixed-width types, `native/shim/gekko_intrinsics.h`
host Gekko intrinsics), force-included, against `reference/sms/include`:
- **~302 / 571 non-asm .cpp compile natively (syntax) with ZERO further work** (53%).
- Only **19 / 579 files (3%)** contain inline Metrowerks PPC `asm` (need C reimplementation).
- The decomp is ~92% complete by function body (968 empty stubs / ~10900 funcs); `Player/` ~100%.

## Obstacle classes (all bounded, ranked by frequency from `g++ -fsyntax-only` first-errors)
1. **~130 `cast from pointer → u32/int loses precision`** — the decomp assumes ILP32 (32-bit pointers).
   THE architecture fork (see below). Cannot `-fpermissive` past it: that truncates pointers = silent
   corruption.
2. **35 missing header `JNd/.../JNDTevBlocks.hpp`** — a JSystem subtree absent from the decomp include
   set (or path issue). Bounded.
3. **~30 `snprintf/vsnprintf/va_start not declared`** — decomp leaned on MSL transitively including
   stdio/stdarg. NB: a blanket host `<cstdio>` prelude LOWERED the pass count (302→222) by exposing MSL
   conflicts → the fix is to SHADOW the MSL libc (`PowerPC_EABI_Support/`) with host-libc forwarders,
   not augment it.
4. **14 `conflicting return type JKRArchive::getResSize`** + a few abstract-class `new` errors — real
   signature/vtable mismatches, small count.

## THE FORK: 32-bit vs 64-bit native build (gates everything)
- **32-bit (`-m32`)**: pointers = 4 bytes → the ~130 pointer↔u32 casts and struct layouts "just work";
  kills the biggest error class at a stroke. This is what decomp-native ports (sm64ex etc.) do, precisely
  because the game stores pointers in 32-bit-sized fields. COST: needs 32-bit multilib installed
  (`glibc-devel.i686 libstdc++-devel.i686`, +32-bit SDL/Vulkan later) — NOT present on this host
  (no crt1.o/i686 libstdc++), needs `sudo`. A 32-bit native x86 binary is still a GC-free PC engine.
- **64-bit + fix casts**: modern, but each of the ~130 pointer-as-u32 casts must be FIXED (store the full
  pointer), with ongoing vigilance as more files compile. Bigger, riskier (silent truncation if missed).

RECOMMENDATION: 32-bit. The pointer-width hazard count makes it the lower-risk, faithful choice.

## Foundation laid this session (`native/`)
- `native/shim/dolphin/types.h` — fixed-width s8..u64 (overrides the decomp's `long`-based u32 which is
  64-bit under LP64).
- `native/shim/gekko_intrinsics.h` — host `__frsqrte/__fres/__fabs/__fabsf/__cntlzw/__sync/__dcbz` +
  `__declspec` macro, `extern "C"` signatures matching the decomp's MSL `<math.h>`. Exact host math
  (a native port may be MORE precise than HW estimates; Newton-Raphson refiners just converge faster).
- `native/{platform,src}/` — empty, for the SDK-seam native implementations + engine glue.

## Bring-up order (proposed)
0. Decide 32/64 (+ install multilib if 32). 1. Shadow MSL libc + add missing JNd headers → push the
clean-compile fraction toward ~100%. 2. Reimplement the 19 asm files in C (math/low-level). 3. Stand up
the `sms-native` CMake target; get the leaf clusters (JGeometry/MarioUtil/JMath/System utils) into a lib.
4. Native platform seams (the big work): OS/threading, filesystem+FST, GX→ngx, DSP→native_jas, SI→input,
VI→present. 5. Boot the game's main loop (TApplication/TMarDirector) natively against those seams,
Dolphin kept ONLY as the diff-oracle. 6. Fill stubs as reached; verify scene-by-scene vs the oracle.

## PROGRESS (2026-06-20, end of session) — phase-1 at 474/573 (83%) compile-clean
Decision locked: **64-bit, architecture-independent (x86-64 AND arm64), fix the source** (uintptr_t /
widened fields), memory-emulation only as fallback. See memory `native-engine-arch-independent`.

Done (committed; parent `main` + submodule `reference/sms` fork, gitlink bumped each step):
- Shims (`native/shim/`): fixed-width `dolphin/types.h`; portable Gekko intrinsics; 5 case-fix
  forwarders; MSL libc shadow (`printf.h`, `stdio.h` via `#include_next`).
- Pointer-cast campaign COMPLETE: 0 `loses precision` sites in non-asm files (128 mechanical →uintptr_t
  across 47 files; 130 event/SPC-VM sites via a `TSpcSlice` pointer accessor + a `TSpcBinary`
  side-table that preserves the serialized symbol stride; OSRoundUp/Down32B macro).
- 19 inline-PPC-`asm` engine files reimplemented in portable C (MathUtil, J3DTransform 8 matrix
  kernels, J3DAnimation Hermite, JUTException, JKRHeap dispose).
- **The single biggest lever:** `MActorData.hpp` `sortByFileNameRaw((void**)unkC)` — one cast in a
  template fixed ~131 instantiations (388→474).
- Platform-seam architecture + 14 scaffold headers + tiered phase-2 work-breakdown
  (`native/platform/`, `README.md`, `api_surface.md`).

Process that worked: parallel subagents on DISJOINT trees (submodule vs native/shim vs native/platform),
each verifying with `g++ -m64 -fsyntax-only`, the LEAD committing parent + bumping gitlink. Submodule
editors run ONE AT A TIME (shared git index). Progress metric = compile-clean count + cast-site count
(NOT "files clean per fix" — that lags until a file's LAST blocker clears).

## NEXT — phase-1 tail (~99 files), then phase-2 (the real work)
Phase-1 tail, by first-error frequency (all small, high-leverage):
- 15 `getResSize` conflicting return type (JKRArchive vtable override mismatch — one header).
- 8 narrowing `long unsigned`→`int`; 5 abstract-class `new` (TViewport/TLightMap — likely a
  not-yet-overridden pure virtual / decomp incompleteness); 4 `DEG_TO_RAD`/`RAD_TO_DEG` undeclared
  (one macro/const header); 3 `SMSGetGameRenderHeight` ambiguating decl; then a tail of one-off
  `no declaration matches` (per-function .cpp-vs-header signature mismatches).
- The 19-asm grep also matched ~32 GC OS/CRT/HW files (`dolphin/`, `TRK_MINNOW_DOLPHIN/`,
  `PowerPC_EABI_Support/`) — those are the platform's job, NOT compiled as game logic.
RUNTIME landmines noted for phase-2 (compile-fine, semantics-wrong on 64-bit): `J3DMaterial.hpp`
`(ptr < 0xC0000000)` GC-address-range heuristic; `fireStartDemoCamera` u32 userdata (knowing trunc);
`operator new(u32)` should be `size_t` on host.

Phase-2 (months, parallel — see `native/platform/README.md`): implement seams along the critical path
**OS → DVD → VI → GX**, audio + input on parallel tracks. Tier0 OS (blocks all) + MTX (pure); Tier1
DVD (assets: FST+Yaz0/RARC + BE→native swap) + CARD (port `runtime/overrides/native_card.cpp`); Tier2
VI then GX (→ ngx renderer, NOT FIFO emu) + Audio (→ native_jas); Tier3 PAD/THP/EXI. Then link the
`sms-native` target, boot `TApplication`/`TMarDirector`, fill the ~970 stubs scene-by-scene, with
Dolphin kept ONLY as the diff-oracle. On this path the goo/EFB-readback "GameCube stuff" becomes plain
engine code.

To continue: native compile check per file =
`g++ -std=c++17 -m64 -w -fsyntax-only <f> -include native/shim/gekko_intrinsics.h -Inative/shim -Ireference/sms/include`

## Don't re-chase
- `port/` (flip) is dead — do not revive.
- A blanket host-libc prelude — it conflicts with MSL; shadow MSL instead.
- `-fpermissive` to silence pointer-truncation — corrupts pointers under LP64; fix or go 32-bit.

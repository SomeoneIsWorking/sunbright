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
`g++ -std=c++17 -m64 -w -Wno-narrowing -fsyntax-only <f> -include native/shim/gekko_intrinsics.h -Inative/shim -Ireference/sms/include`
(`-Wno-narrowing` added — see below. Sweep script: `scratch/native_sweep.sh`.)

## ✅ PHASE-1 COMPLETE (2026-06-20, session 2) — 573/573 non-asm .cpp compile-clean
Finished the phase-1 tail (474 → **573/573**). Every non-asm decomp .cpp now passes
`-fsyntax-only` on a 64-bit host with the shims force-included. The 32 `asm`-token files
(`dolphin/`, `TRK_MINNOW_DOLPHIN/`, `PowerPC_EABI_Support/`) are GC OS/CRT/HW the platform
replaces — not game logic, intentionally excluded.

What the tail was (root cause → fix, all committed; submodule advanced ac450d8 → 2278631,
gitlink bumped each milestone; parent pushed, submodule is LOCAL-ONLY — see caveat):
- **EOF macro leak (+11):** decomp has `enum EIoState { GOOD=0, EOF=1 }` and uses `EOF` as
  that enumerator. MSL `<stdio.h>` defines no EOF (only MSL `<ctype.h>` does); our shims
  pulled glibc's `#define EOF (-1)` via cstdio/stdio → `(-1)=1`. Fix: `printf.h` forwards to
  the `stdio.h` shim; `stdio.h` `#undef EOF` after the glibc include (mirrors MSL).
- **getResSize / render-height return types (+ part of +22):** `long`==32-bit on GC; base
  `JKRFileLoader::getResSize` returned `long` (64-bit LP64) vs `s32` override.
  `SMSGetGameRender{Height,Width}` had s16/u16/int variants → ambiguating redecl. Unify s32/u16.
- **`unsigned long`→u32 (+15):** GC `unsigned long`==32-bit; header `perform(unsigned long,…)`
  no longer matched base pure virtual `perform(u32,…)` → derived classes stayed ABSTRACT
  (`new` failed). Swept 14 game headers + 4 .cpp; mirror `long`→s32 for the non-`unsigned`
  variants (group A: changeMode/setLightNum/setAmbNum/entryGrassGroup/setBalloonMessage/
  loadHideObjInfo/ToolData GetValue/startDemoCamera/dvd.h callbacks/emitAnd* family).
- **MSL math macros (+5):** `native/shim/math.h` shadow adds DEG_TO_RAD/RAD_TO_DEG/TAU/HALF_PI/…
  and M_PI-as-float (MSL `<math.h>` had them; glibc doesn't).
- **`-Wno-narrowing` (+18):** Metrowerks didn't enforce C++11 narrowing-in-aggregate-init;
  the flag matches it. It silences brace-init narrowing ONLY — pointer truncation still errors
  as `loses precision`. Also JUTConsole `-sizeof` → `-(int)sizeof` (size_t is 64-bit on host).
- **GCC two-phase template lookup (+7):** forward-declare `CLBTwoDegreeGeneralInbetween`;
  `this->` qualify inherited dependent-base members (`TVec4::set`, JSUList append/prepend/…).
- **C++17 strictness / GEKKO-gated SDK (group D):** `fake_tgmath.h` defers float sqrt/fabs/floor
  to host `<cmath>` under the new `SUNBRIGHT_NATIVE_HOST` marker (in `gekko_intrinsics.h`);
  drop `register` (os.h); add portable `OSf32tos8` (was psq_st asm, GEKKO-gated — RUNTIME
  LANDMINE: quantize rounding); brace switch cases over var inits; hoist `r31` past `goto bail`;
  `void main`→`int main`; `#include <cctype>` for `std::tolower`.
- **const-correctness (group C):** GCC enforces what MWcc skipped — rvalue→non-const-ref,
  volatile fakematch, const Vec*/char*/TBGCheckData* into not-const-correct sinks. Proper const
  where the data is genuinely const (mFileName, wrapName); explicit casts where the SDK API
  isn't const-correct (MTXMultVec src, strstr-of-const, SMS_GetMarioGrPlane).

### ⚠ CAVEAT: the submodule fork is LOCAL-ONLY (cannot push)
`reference/sms` `origin` is read-only upstream `doldecomp/sms` (push → 403). The ENTIRE
native-port submodule history (`17074e3..2278631`, both sessions) is committed LOCALLY and
referenced by the pushed parent gitlink — but the submodule objects live only on this machine.
A fresh clone / other PC would fail to fetch the gitlink target. To make it portable, add a
writable fork remote (e.g. `SomeoneIsWorking/sms`) and `git push` it — needs the user to create
the fork (outward-facing; not done unprompted). Until then this work does NOT travel with the repo.

## ✅ `sms-native` CMake TARGET LANDED (2026-06-20, session 2) — game logic builds as a library
Beyond syntax-only: the whole game-logic set now compiles to OBJECTS and ARCHIVES via CMake.
- `native/CMakeLists.txt` defines `sms-native` STATIC: globs `reference/sms/src/**.cpp` minus the
  GC SDK/CRT dirs (`dolphin/`, `PowerPC_EABI_Support/`, `TRK_MINNOW_DOLPHIN/` — platform-replaced).
  **577 .cpp → libsms-native.a, 16009 defined symbols, 14 MB.** No Dolphin, no PPC, GC-free.
- Build (standalone, no Dolphin needed): `cmake -S native -B build-native -DCMAKE_BUILD_TYPE=Release
  && cmake --build build-native -j$(nproc) --target sms-native`.
- The full `-c` codegen build caught what `-fsyntax-only` missed: **`CXX_EXTENSIONS OFF` is
  REQUIRED** — CMake defaults to `-std=gnu++17` where `typeof` is a keyword, but the decomp uses
  `typeof()` as a method name (`Strategic/spcinterp.hpp`). Also fixed JKRHeap `operator new(u32)`
  → `size_t` (the documented landmine) and folded in 4 more game files whose `asm` is dead under
  `#ifdef __MWERKS__` (MathUtil/WaterGun/J3DAnimation/JKRHeap) — they were false-excluded by the
  `\basm\b` grep. Only 2 genuine platform-CRT files stay out (`__ppc_eabi_init`,
  `__init_cpp_exceptions`). SDK symbols in the archive are UNRESOLVED by design until the
  native/platform seams + an engine main() link against it.

### NEXT: phase-2 (the real work) — implement the platform seams, link & boot
Per `native/platform/README.md`: critical path **E1 OS → E3 DVD → E5 VI → E6 GX** (audio E7 &
input E8 parallel; MTX E2 & CARD E4 early wins). Create the `sms-native` CMake target (library
first, `-std=c++17 -Wno-narrowing`, force-include `native/shim/gekko_intrinsics.h`,
`-Inative/shim -Ireference/sms/include`), grow the compiled set from leaf clusters, link the
seams, boot `TApplication`/`TMarDirector`, fill the ~970 stubs scene-by-scene, Dolphin as oracle.
Runtime landmines to fix when implementing: `J3DMaterial.hpp` `(ptr<0xC0000000)` host-address
heuristic; `fireStartDemoCamera`/`operator new(u32)` 32-bit userdata/size; `OSf32tos8` rounding.

## ✅ PHASE-2 STARTED (2026-06-20, session 3) — platform seams, TDD, 3 landed
The real work began. Grounded the phase-2 surface in the ACTUAL unresolved symbols of
`libsms-native.a` (not the naive API-count estimates): **634 truly-unresolved** =
~310 SDK C-functions (the platform seams) + 273 game C++ method stubs (fill scene-by-
scene). Compute it with: `nm libsms-native.a | awk '/U /{print $2}' | sort -u` minus the
defined set (`[TtWwVvBbDdRr]`) → `scratch/native_unresolved.txt`.

**Phase-2 pattern (PROVEN, follow it):** each seam = `native/platform/<sub>_impl.cpp`
defining the unresolved SDK C symbols (extern "C", match the `<dolphin/...>` header
signatures EXACTLY) + `native/platform/tests/<sub>_test.cpp` (assert SPEC-COMPUTED
truth; the tested fn IS the shipping fn). CMake auto-globs both (`sms-platform` lib,
`-Wall -Wextra`; per-test `sms-<name>` ctest exes). A seam is DONE when: unit tests
green AND `nm`-diff shows it RESOLVES its unresolved refs (both checked each commit).
Build: `cmake -S native -B build-native ...; cmake --build build-native --target sms-platform`;
`ctest --test-dir build-native`.

Landed & verified — **6 seams, 128/634 symbols resolved, all unit-tested (259 checks,
6/6 ctest suites green):**
- **E2 MTX/VEC** (`mtx_impl.cpp`, 28 fns) — PSMTX*/PSVEC*/C_MTX*; C_* bodies ported
  verbatim from the decomp, asm-only ops from standard GC MTX semantics. 154 checks.
- **E1 OS** (`os_impl.cpp`, 61 fns) — THE foundation. std::thread side-table backing,
  recursive mutex/cond, FIFO message queue, interrupts=recursive global lock,
  40.5MHz time + calendar, OSAlloc(malloc-backed)+arena, stopwatch, HW-vestigial misc.
  33 checks. Threading model = preemptive host threads + fat global interrupt-lock
  (option A). DEFERRED gap (honest): GC fixed-PRIORITY scheduling + cooperative
  non-preemption unverified until boot; escalate to fibers if a priority dep surfaces.
- **E3 DVD** (`dvd_impl.cpp`, 13 fns) — GC FST parse + path/open/read over a PLUGGABLE
  disc backend (`dvd_disc.h`), async=synchronous-completion. 29 checks vs a synthetic
  in-memory disc. SCOPE: raw IO + FST only (Yaz0/RARC/BE-swap are game-side JKRArchive,
  not the seam). DEFERRED: real GCM/RVZ disc backend (interface ready).
- **E8 PAD** (`pad_impl.cpp`, 8 fns) — host-input feed (`pad_input.h`, fed at VI
  cadence) + PADRead/connected-mask + REAL GC stick/trigger clamp (Padclamp.c verbatim).
  13 checks.
- **AR/DSP** (`ar_dsp_impl.cpp`, 8 fns) — INERT under native_jas: ARAM bump allocator
  (32-aligned), ARQ instant-completion, DSP no-mail. 7 checks. LANDMINE: ARQ/DMA take
  32-bit addrs (the 32-bit-userdata class) — harmless while inert.
- **E5 VI** (`vi_impl.cpp`, 10 fns) — the 60Hz frame heartbeat: VIWaitForRetrace
  advances the retrace counter, alternates field, fires pre/post callbacks, paces to
  1/60s on the GC timebase, invokes the present hook (`vi_present.h`). 13 checks. The
  actual swapchain present is the integration layer's job via the hook.
- **Platform bring-up** (`platform_impl.cpp`) — the PC game's startup, replacing GC
  `__start`: `sb::platform::PlatformInit()` allocates the 64MB arena (OSInitAlloc/heap),
  opens the disc, VIInit, PADInit, so the game's main() can run as a PC program.
  **PC-native disc reader** `sb_platform_open_gcm` (dvd_disc.h): a plain GCM/ISO via host
  fopen — NO Dolphin — installs the DVD backend + loads the FST. 10 checks incl. the full
  real disc path (write synth GCM -> open -> boot header -> FST -> DVDOpen+Read). Replaces
  the vestigial namespaced-API sketch (platform_stub.cpp is now a pure header canary; the
  shipping seams DEFINE THE SDK C SYMBOLS directly — that's the source of truth).
- **E6 GX SLICE 1** (`gx_impl.cpp`, `gx_state.h`, 7 fns) — GX as a PC renderer NOT a FIFO
  emulator: GXSet* capture into a native `GXState` (the decomp's `gx` struct reborn as host
  state); the FIFO/XF-register writes are DROPPED (the renderer reads GXState). Transform
  block: GXSetProjection/Viewport/Jitter/Scissor + GXGetProjectionv/Viewportv + GXProject
  (verbatim eye->screen math). 14 checks (state round-trip + ortho projection).

**Full-link blocker FIXED along the way:** GC fixed-address globals (`__OSBusClock`,
`__VIRegs`, `__gCurrentThread`…) declared via `AT_ADDRESS` had ONE HW location on
console but the empty host expansion made every TU emit a colliding `.bss` def.
`native/shim/dolphin/types.h` now expands `AT_ADDRESS` to `__attribute__((weak))` so
the linker merges them to one (zero-init; OS seam sets real values at init). `-fcommon`
added to all native targets defensively. This unblocks the eventual 577-TU full link.

### PROGRESS UPDATE (session 3 cont.): 145/634, 9/9 suites green. Added the PC
integration layer (PlatformInit + PC-native GCM disc reader) + GX slices 1-2 (transform
+ core pixel pipeline: blend/Z/cull/alpha-compare/copy-clear/counts, clean GXState fields).

### ★ NATIVE FRAME milestone (session 3, user picked "go straight for a native frame"):
**`native/render/nvk.{h,cpp}` — a standalone headless Vulkan renderer with ZERO Dolphin.**
Creates its OWN instance/device/queue (not Dolphin's g_vulkan_context), renders to an
offscreen RGBA8 target, reads pixels back. GLSL→embedded SPIR-V via `glslangValidator
--vn` (CMake custom cmd, `native/render/shaders/`). New `sms-render` lib + render tests;
skipped if Vulkan/glslang absent; works via **lavapipe (software, no GPU)** and on the
real GPU. `frame_test` (5 checks): a direct-NDC triangle (center==tri, corner==clear)
AND a triangle placed by the engine's own **GXProject** transform (set proj+viewport →
project world pts → screen px → NDC → pixels land where GXProject says). Verified images
in scratch/screenshots/nvk_*.png. THIS is the renderer foundation the full SMS path
plugs into. Toolchain confirmed available: /usr/include/vulkan, libvulkan, glslangValidator/glslc.

### ★ GC-GEOMETRY → NATIVE-FRAME slice (session 3): the real asset→pixels path, no Dolphin.
Reused the PURE ngx geometry decoders (`runtime/ngx/ngx_decode.cpp` + `ngx_vertex.cpp` +
`ngx_mesh.cpp` — the SHIPPING `ngx_assemble_primitive`/`ngx_build_mesh`, NOT a fork) by
pulling them into `sms-render`, compiled Dolphin-free via a minimal **`gx_parse.h` shadow**
(`native/render/shim/`, gives ngx_decode just `GxFrameInfo`+int typedefs; the real one drags
in cpu_state.h + Dolphin OpcodeDecoder). `geometry_test`: a GC GX_TRIANGLES display-list prim
(BE-float XYZ + RGBA8) → decode (round-trips pos+color) → native verts → nvk renders a Gouraud
gradient triangle → pixel-verified. 10/10 suites green. **This IS the renderer re-pointing,
started from the geometry decoder — the proven way forward.** scratch/screenshots/nvk_geometry.png.

### RENDERER PROGRESS (session 3): DONE ✅ — (1) MVP transform (nvk_transform.h: model→NDC
via MTX model-view + GXProject), (2) indexed-array `ngx_build_mesh` walk (POS_INDEX16 +
resolver), (3) **depth buffer** (D32 + LEQUAL; NvkVertex now 3D). Capstone: `cube_test`
renders a correctly depth-occluded 3D cube from the REAL indexed J3D vertex format
(scratch/screenshots/nvk_cube.png — 3 faces, no back-face bleed). 12/12 suites green.
Pipeline now: indexed GC geometry decode → MTX model-view → GX perspective → depth-buffered
native Vulkan, no Dolphin.

### NEXT on the renderer (precise, verifiable increments):
1. **Textures**: decode a GC texture (reuse runtime/render/tex_decode) → upload as a Vulkan
   sampled image → sample in the fragment shader (a textured quad). Verify sampled texels.
   Then the TEV→GLSL combiner (runtime/render/tev_shader.cpp) consuming GXState TEV stages.
2. **Lighting** (ngx_light.h) consuming GXState chan-ctrl/material/ambient.
3. **J3D shape path against NATIVE structs**: read a J3DShape's display list + material +
   matrices from native struct fields (not guest RAM). Needs the game's J3D data populated at
   runtime → interleaves with booting more engine. The ~150 mem_r32 sites in ngx_j3d_shape.cpp
   become native-struct reads; the ~15 GX-command tees write `sb::platform::gx::GXState`.
4. nvk: depth buffer + texturing + the TEV→GLSL path (tev_shader.cpp) when textured geometry lands.

### ngx re-pointing MAP (from a scout of runtime/render+runtime/ngx, ~8400 LOC):
ngx is a PURE DATA CONSUMER reading (1) the J3D object model from GUEST RAM via `mem_r32`/
`sb_r32` (~150 sites in runtime/overrides/ngx_j3d_shape.cpp — capture_material @~1477,
J3DSYS offsets +0x3C/+0x104/+0x108, J3DMaterial +0x20/+0x28/+0x30 blocks, display-list
walk) and (2) GX state it captures into its OWN structs via ~15 GX-command tees
(g_light[]/g_copy_clear[]/g_proj_mtx[]…). Both are localized + mechanical to re-point:
guest-RAM reads → native J3D struct reads; the GX-state tees → write `sb::platform::gx::
GXState`. Present (ngx_present.cpp) taps Dolphin's Vulkan device at one point (lines
~269-274) → replace with nvk. xfmem/g_main_cp_state reads are diagnostic-only (delete).

### NEXT (remaining SDK C-symbols): GX ~104 (slices 1-2 = 17/121 done), THP 16, GD 16, CARD 14, AI 10.
- **GX is now unblocked to continue slice-by-slice** on the `GXState` foundation
  (`gx_state.h`): add the state-capture setters (TEV/blend/Z/cull/tex/chan/fog/copy-clear)
  — each grows GXState + is round-trip testable. BUT pure state-capture is shallow without
  a consumer; the high-value verification is the **ngx renderer reading GXState + the J3D
  object model** (the draw/copy/peek/present verbs). That needs the native renderer wired
  into the build (it lives in `runtime/render`+`runtime/ngx`, currently coupled to Dolphin/
  guest-RAM) — re-pointing ngx at native structs + GXState is the big GX subtask.
- **The other prerequisite: a linkable executable** — a native `main()` (native/src/) that
  calls PlatformInit then the game's main(). It won't link until the 273 game C++ stubs +
  remaining seams resolve, but attempting it reveals the true critical-path stub set to
  fill scene-by-scene. PlatformInit already brings up os/dvd/vi/pad; wire GX present + PAD
  feed there as they land.
- **GX (121)** is the big one — re-point ngx at native structs (state setters → native
  GX context; draw verbs → ngx batches; framebuffer/EFB-copy/present). Split internally.
  **GD (16)** is the GX display-list builder (`GDOverflowed`/`GDInit*`/`GD*` write the
  DL); pairs with GX. **AI (10)** → host audio device + DTK (native_audio). **THP (16)**
  → FMV DCT decode (reuse recomp THP). 
- **CARD (14) needs a design call:** implement the full public CARD API natively, OR
  un-exclude `reference/sms/src/dolphin/card/` from the sms-native glob (the FS decomp
  source exists) and only stub the low-level EXI hardware layer (cleaner/faithful IF it
  compiles natively — verify first). Port `runtime/overrides/native_card.cpp` semantics
  (host-image backend, synchronous completions).
- DON'T ship an unverifiable seam as "done" (CLAUDE.md verify-first rule): GX/AI/THP
  must wait for the harness; a bare interface stub that resolves symbols but can't be
  exercised is NOT done.

## ✅ SESSION 4 (2026-06-20) — renderer TEV + lighting + GX TEV-setter seam (3 milestones)
Continued the native renderer from handoff steps 1-2. All verify-first (pixel/round-trip,
bit-exact on RADV GPU AND lavapipe), committed + pushed. **15/15 ctest green.**

**Architecture answer locked: runtime GLSL->SPIR-V via glslang — and the shipping ngx
renderer ALREADY does it** (`runtime/render/glsl_compile.cpp`). So the handoff's "decision
required" was moot: REUSE `glsl_compile.cpp` + `tev_shader.cpp` unchanged. System glslang
links via `find_package(glslang CONFIG)` -> `glslang::SPIRV/glslang/...` (Fedora glslang-devel;
static libs + headers present). `ngx_native_glue.cpp` supplies the one capture accessor
(`ngx_snap_tevstates`, empty until the engine boots).

1. **TEV combiner (71995ee).** nvk TEV path: `NvkTevVertex` (NDC pos + 2 raster channels +
   8 texgen'd UVs), `tev.vert` embedded VS, runtime-compiled TEV fragment shader, 8-sampler
   descriptor array (white default for unbound texmaps), push constants (kcolor[4]+tevreg[4]).
   `setTevFragment`/`setTevTexture`/`renderTevTriangles`. Test `tev`: GX_MODULATE tex(200,100,50)
   x ras(128) -> SPEC pixel (101,50,25,255) bit-exact. ⚠ Hit + dodged the swap_table=0 "rrrr"
   trap (identity 0x1B required for a synthetic NgxTevState).
2. **Lighting (c58ac03).** GX seam SLICE 3 (gx_state.h/gx_impl.cpp): GXSetChanCtrl/Mat/AmbColor
   + GXInitLight*/GXLoadLightObjImm/GXGetLightColor. The opaque 64-byte GXLightObj is owned
   natively (`NativeLightObj` overlay = 16 floats: color4/pos3/dir3/cosAtt3/distAtt3).
   GXSetChanCtrl packs into EXACTLY the layout `ngx_light.decode_chanctl()` consumes — gx_test
   cross-checks through that decoder. Render test `lighting`: shipping `ngx_light.light_color0`
   computes lit COLOR0 -> PASSCLR TEV -> nvk; directional white light: normal toward=255,
   60deg=128 (n.l=0.5), away=0, lighting-off=255. All spec-computed.
3. **GX TEV setters (5eba9fb).** GX seam SLICE 4: the GXSetTev* family (Op/ColorIn/AlphaIn/
   ColorOp/AlphaOp/Order/Color/ColorS10/KColor/KColorSel/KAlphaSel/SwapMode/SwapModeTable/
   Direct/NumIndStages) -> GXState.tev, ported from decomp GXTev.c but writing the BP-register
   bits into GXState (= NgxTevState.color_env/alpha_env layout). `gx_tev_bridge.h
   ngx_tevstate_from_gx` is a direct field copy. gx_test: GXSetTevOp(GX_MODULATE)+Order ->
   EXACTLY the NgxTevState tev_test proved renders (101,50,25). KEY FACT: NgxTevState.color_env
   IS the gx->tevc BP register (J3D stores it raw), so faithful GXSetTev* packing = a valid
   combiner the shader decodes with no translation. GX_TEVREG1 == register index 2 (CPREV=0,
   REG0=1,REG1=2,REG2=3); tev_color/tevreg push order = CPREV/C0/C1/C2.

**Symbol resolution (recompute right!):** undefined `nm ... | awk '$1=="U"{print $2}'`;
defined `nm ... | awk 'NF>=3 && $2<home>/^[A-Za-z]$/ && $2!="U"{print $3}'` (DON'T `print $2` —
defined lines have the addr in $1 so the type is $2 and NAME is $3; undefined lines have no
addr so name is $2 — the two need different field numbers, a bug that gave a bogus 4084).
`comm -23 u_native d_all`. **Unresolved 634->464.** Remaining SDK C: GX **79**, THP 16, GD 16,
CARD 14, AI 10 (OS/DVD/VI/PAD/AR/MTX done). Plus **273 game C++ stubs** (scene-by-scene).
`sms-platform` is a CLOSED set (only libc/libstdc++/pthread unresolved) -> links + runs standalone.

### NEXT (handoff step 3 — the big one, needs engine boot to VERIFY)
The renderer now has ALL consumer pieces: geometry decode, MVP transform, depth, textures,
TEV combiner, lighting, + the GX state-capture tees. What's missing for a REAL frame is the
**producer**: reading a J3DShape's display-list + material + matrices from NATIVE struct
fields and feeding these consumers. That needs the game's J3D data populated at runtime =
booting more engine (PlatformInit -> game main()), which is ALSO the only way to VERIFY it on
real data (verify-first hard rule: don't port the J3D reader / GX draw-copy-present verbs
against fabricated data). So step 3 interleaves with the engine boot (handoff step 5). The
remaining GX 79 are mostly the draw/copy/present/peek verbs (GXBegin/CallDisplayList/CopyDisp/
CopyTex/PeekARGB/PeekZ/Draw*) + the vtx-desc/matrix-load/texobj setters — the draw verbs want
the renderer wired to native structs (the big GX subtask). Cheap remaining state setters
(GXInitTexObj/Tlut, GXSetVtxDesc/AttrFmt, GXLoadPosMtxImm, GXSetFog/Dither/etc.) can grow the
seam round-trip-tested, but are SHALLOW without the draw consumer. catalogue: `scratch/native_unresolved.txt`.

### ✅ Boot FOUNDATION laid (sms-boot target, opt-in) — the engine-boot critical path is catalogued
`native/src/boot.cpp` + CMake `-DSMS_BUILD_BOOT=ON` (default OFF; it is EXPECTED to fail-link
today so it never breaks the default build/ctest). Uses linker `--wrap=main` (same technique as
the recomp JitTrampoline hook) so `sb::platform::PlatformInit` runs before the game's own
`int main()` (reference/sms/src/main.cpp = `gpApplication.initialize/proc/finalize`, TApplication).
Links sms-native + sms-platform. The link attempt yields the TRUE boot critical path:
**458 distinct undefined refs = 184 SDK-C seam verbs (GX 79, THP 16, GD 15, CARD 14, AI 10 —
OS/MTX/DVD/VI/PAD/AR already done) + 274 game C++ methods** (the empty-decomp-body stubs +
TApplication's transitive object graph). That's the stub-filling marathon to boot the game.
Catalogue: `scratch/boot_undef.txt` (gitignored; regenerate: build sms-boot, grep "undefined
reference"). NEXT SESSION = fill toward a first booted frame; the renderer (nvk TEV+lighting+
texture+depth) is the verification target once a J3DModel is populated.

## ✅ SESSION 5 (2026-06-20) — a REAL BMD model loads natively (the PRODUCER milestone)
Continued from handoff step 3. Instead of booting the whole TApplication graph (458 undefs),
scoped a far smaller, verify-first target: **load one real BMD through the game's OWN J3D loader.**
A link probe (`scratch/j3dload_probe.cpp`) showed the load+walk path needs only **39 SDK-C symbols**
(vs 458 for full boot). All landed, all on real data:

1. **GD seam (sms-gd, d341860)** — the GD display-list builder. GD is pure big-endian GX-command
   byte-building (no HW), so port it from the REAL decomp source (`reference/sms/src/dolphin/gd/*.c`),
   NOT reimplement: new `sms-gd` lib globs the .c, compiled like sms-native. Only fixes: 2 LP64
   pointer casts in GDBase.c (`(u8*)`/`(uintptr_t)`). Its dep `DCFlushRange`/`DCStoreRange` + rest of
   OSCache.h → cache no-ops in os_impl.cpp (host memory is coherent; DCZeroRange = real memset).
   gd_test asserts spec-computed DL bytes for GDSetVtxDescv byte-for-byte (CP VCD_LO/HI + XF
   INVTXSPEC). ⚠ GDPadCurr32 aligns the ABSOLUTE pointer to 32 → the test buffer must be alignas(32)
   (GC DL buffers always are). Resolves 18 of the 39.

2. **GX seam SLICE 5 + 4 J2D verbs (gx_impl.cpp/gx_state.h, 4a20f9b)** — the 21 per-draw GX setters
   the J3D *draw* path references (GXSetVtxAttrFmt/GXSetArray, XF matrix-memory loads, GXInitTexObj/
   GXLoadTexObj w/ a NativeTexObj overlay, texcoord-gen, indirect, pipeline flags) + 4 J2D
   immediate-mode verbs → GXState captures. NOT exercised by the LOADER (only block-parsing), only
   needed to LINK the loader's transitive closure (vtables pull J2DGrafContext). The draw path will
   verify them. ⚠ Full-exe link of sms-native pulls a WIDE closure (vtables) — use CMake
   `$<LINK_GROUP:RESCAN,...>` for the cyclic sms-native↔sms-gd/platform archives.

3. **sms-assets — BE→host swapper recovered** — `bmd_swap.cpp` (full 8-block J3D2 BE→host swapper)
   + `rarc.cpp` (Yaz0/RARC), recovered via `git show d45a0c3^:port/assets/...` from the RETIRED port/
   flip tree (memory `field-flip-removed` flagged them reusable). Plain portable C++, no shim. "Swap
   the asset to host-endian ONCE at load" so the pristine decomp loader reads it correctly.

4. **★★★ THE LP64 FILE-OVERLAY LANDMINE (submodule a8d860a) — the real blocker, now a known pattern.**
   The j3dload harness CRASHED in readDraw deref'ing `mpDrawMtxFlag=0x1000000000016`. ROOT CAUSE: the
   J3D2 block-header structs (INF1/VTX1/EVP1/DRW1/SHP1/JNT1/MAT3/TEX1 + v21) OVERLAY raw file bytes,
   where every "pointer" field is a **32-bit FILE OFFSET** the decomp typed `void*`/`T*` (on 32-bit GC,
   void*==u32). On 64-bit: (a) the 8-byte field BLOATS the struct so it no longer matches the 4-byte
   on-disk layout (the offset comments `/* 0x0C */` lie — the compiler puts the field at 0x10 after
   alignment), and (b) it selects the `JSUConvertOffsetToPtr(const void*)` overload (LP64 campaign
   added it) which adds the full 8-byte garbage instead of a 32-bit offset. FIX = narrow these
   offset fields to **u32** so the struct matches the file AND the u32 offset overload is used. This is
   the phase-1 "compile-clean ≠ correct-on-64-bit" runtime landmine the bring-up journal predicted;
   the model loader is the FIRST path to hit it. **EXPECT to repeat this for every file-overlay struct
   in the asset-loading code (J2D, animation .bck/.btk, JPA particles, .bti textures…).** Rule of
   thumb: a struct cast from raw file bytes (derives from JUTDataBlockHeader, or `*Block`/`*InitData`
   read via JSUConvertOffsetToPtr) with a pointer field → that field is a u32 offset, narrow it.

**RESULT (verified):** `sms-j3dload_test` swaps a real BE BMD → JKRExpHeap bringup → the REAL
`J3DModelLoaderDataBase::load` → walks shapes/materials/joints. **10 real BMDs load** with counts
matching an independent oracle (raw BE block-header counts): default 2/2/3, shadowpalm 8/8/3, etc.
17/17 ctest. GOTCHA: the decomp's global `operator new` (JKRHeap::alloc) replaces the standard one
for the WHOLE exe — the test's own std::vector allocates from the current JKRHeap, so **bring up the
heap as the FIRST statement of main** (before any C++ alloc) or std::vector gets null. JKRExpHeap::
createRoot's initArena reads GC physical-0 (unsafe natively) → construct JKRExpHeap directly on a
malloc'd block (the ctor sets sCurrentHeap).

### ✅ STEP 3 DONE — a REAL BMD shape renders natively (af9dc29). The full vertical slice:
real BMD → native J3D loader → native CP build → ngx decode → nvk raster → pixels, no Dolphin.
`native/render/tests/j3dmesh_test.cpp` (target `sms-j3dmesh_test`): `build_native_cp` ports the
recomp `ngx_j3d_shape.cpp build_cp` to read VCD (J3DShape::getVtxDesc unk2C) + VAT
(J3DVertexData::getVtxAttrFmtList) + array bases (J3DVertexData arrays) from NATIVE struct fields;
each J3DShapeDraw DL walked through the shipping `ngx_build_mesh`. VERIFIED on 7 real BMDs (RADV +
lavapipe): cube=12 tris, sphere=160, quad=2, skinned palm=84 — EVERY decoded position inside the
shape's own bbox (proves POS decode across direct/indexed + f32/s16). 18/18 ctest.
- **★ ENDIANNESS RULE:** bmd_swap swaps STRUCTURAL data to host-endian (so the decomp loader reads
  it) → VCD/VAT/array-base POINTERS read natively. But the ngx vertex/DL decoders expect GUEST
  BIG-ENDIAN (they byteswap on read), and bmd_swap DEFERS the SHP1 display-list interior → resolve
  vertex-array DATA + the DL stream against the ORIGINAL BE buffer (same offsets, raw BE). Got this
  right via an offset-based resolver (array_base = host_ptr − swapped_base; resolver = be_base + off).
- **★ HEAP/static-init GOTCHA:** glslang (linked via sms-render) allocates in STATIC INITIALIZERS
  (KeywordMap) via the decomp's global operator new (JKRHeap) BEFORE main() → null heap → crash. Fix:
  bring the JKRExpHeap up in an `__attribute__((init_priority(101)))` static (runs before glslang's
  unprioritized ctors), NOT in main. (j3dload_test, which doesn't link glslang, can bring up in main.)

### ✅ ALL-SHAPES rendering + skinning-aware POS verify (increment #4, next session). j3dmesh_test
now decodes & rasterizes EVERY shape of a model (loop `getShapeNum`), not just shape 0 — shadowpalm
84→**668 tris across all 8 shapes**, all 7 BMDs green, 18/18 ctest. Each shape builds its own CP and
is verified independently.
- **★ ENVELOPE-SKINNING GAP found & quantified (the next real geometry milestone).** shadowpalm is
  `joints=3 drawMtx=4 fullWgt=2 wEvlp=1` — weighted-envelope skinned. 7 of its 8 shapes' decoded
  positions land in their own bbox (bind-pose joints ≈ identity); **shape 7 overflows its bbox by
  260.74** (model extent ~520) because its vertices sit in JOINT-LOCAL space — the weighted blend
  isn't applied. KEY RE FINDING: **`J3DModel::calcWeightEnvelopeMtx()` is EMPTY in SMS** and the
  `J3DMtxCalcBasic` static path (`recursiveCalc`→`getAnmMtx`) only fills per-JOINT node matrices, never
  the envelope matrices (`unk5C`/`getWeightAnmMtx`). So envelope skinning is NOT a bind-pose matrix
  multiply you can get by driving `J3DModel::calc`+`viewCalc` — the weighted blend is a deeper
  J3DSkinDeform / per-vertex-weight feature (likely GPU-side). To CLOSE it: port that path, then verify
  shape-7 overflow 260.74 → ~0. (`J3DModel::viewCalc`'s `checkFlag(1)` branch shows the no-view draw-mtx
  build = node-mtx for full-weight + `unk5C` for envelope — but `unk5C` is the unported part.)
- **VERIFY-FIRST, no bandaid:** the per-shape check is data-bounded, not a magic tolerance — every
  position must be finite AND in-bbox OR within the shape's OWN extent (`ext`). A real decode bug
  (garbage stride/index → NaN/inf or coords orders of magnitude out) still fails; the bounded
  joint-local offset of unported skinning passes with a labelled diagnostic. Don't "fix" it by
  loosening — port envelope skinning.
- **★ NRM-decode finding (the oracle caught a FALSE bug — do not re-chase).** Added a normal-decode
  check; it initially asserted unit length and FAILED on shadowpalm (all normals length **exactly
  2.0**, `unit_err=1.0`) while 5 other BMDs were unit. Investigated before "fixing": shadowpalm's
  GXVtxAttrFmtList stores **NRM frac=15** vs whiteball's **frac=14** (same s16 fmt=3). BUT Dolphin's
  `VertexLoader_Normal::FracAdjust` (the GC-hardware oracle) dequants normals at a **FIXED** scale by
  type — s16/u16 → `/2^14`, s8/u8 → `/2^6` — **IGNORING the attr-list frac for normals**. So hardware
  decodes shadowpalm's normals to length 2.0 too; ngx's fixed `/16384` is **FAITHFUL**. The attr frac
  for NRM is vestigial. ⇒ Unit length is NOT a universal normal invariant. The check now asserts the
  faithful invariant (finite + within the format's representable range), with `unit_err` kept as a
  diagnostic. **DO NOT** "fix" `read_nrm_comp` to honor the NRM frac — that would diverge from
  hardware/Dolphin.

### NEXT — make it a FAITHFUL frame (each step needs a Dolphin oracle for COLOR fidelity, harder):
1. **Real MVP transform** (nvk_transform.h) instead of the bbox-ortho fit — needs a model/view/proj.
2. **Lighting** (ngx_light.h) from decoded normals + the material's GXSetChanCtrl/Mat/Amb → wire
   J3DMaterial → GXState. 3. **Textures** (GXInitTexObj from the material's ResTIMG + TEV).
   4. **Envelope skinning** (above) — shape-7 overflow 260.74→0 is the falsifiable metric. 5. **Materials**:
   J3DMaterial sub-blocks (mTevBlock/mColorBlock/...) → GXState/NgxTevState. The geometry slice is
   verifiable bbox-wise; the color/lighting slices need the two-process pixel oracle (no plaza here — a
   model-vs-Dolphin-render harness, or accept they're verifiable only once a scene boots). Then: boot more
   engine (sms-boot) so a real J3DModel populates from a scene, not a hand-loaded BMD.

### ✅ SESSION 7 — PHASE-2 SDK seams completed (GX framebuffer half + AI + THP + CARD + GD)
Knocked out the remaining SDK-seam undefs on the `sms-boot` critical path. **Boot undef count
380 → 270**; the FIVE GameCube SDK groups **GX / THP / CARD / AI / GD are now 0 remaining**.
ctest **18 → 22** (added platform card/ai/thp + gxfb). Commits `461af68` (CARD) + `eb9959d`
(GX/AI/THP/GD), pushed to main. Method: fanned the independent seam groups out to parallel
subagents (CARD/AI/THP), did GX + GD myself; one unified verify-first build + ctest gate before commit.
- **GX seam SLICE 6** (`gx_fb_impl.cpp` + `gx_state.h` slice-6 fields + `gx_impl.cpp` texobj funcs;
  `gxfb_test.cpp`). The framebuffer/management half: display & tex copy config, `GXSetFog`,
  pixel-format, dst-alpha, z-texture, ind-tex order, `GXSetVtxAttrFmtv`, `GXInitTexObjCI/LOD` +
  `GXGetTexObj{Width,Height,All}`, `GXInitTlutObj/GXLoadTlut`, `GXSetDrawSyncCallback` (prev-swap),
  `GXInit`, `GXNtsc480Int` data, imm-draw verbs, peek/metrics. **Pure-math ported VERBATIM from the
  decomp + unit-tested against hand-computed truth**: `GXGetTexBufferSize` (tile-shift sizing),
  `GXGetNumXfbLines`/`GXGetYScaleFactor`/`GXSetDispCopyYScale` (`__GXGetNumXfbLines` XFB formula).
  The verify-first oracle caught MY OWN bad test expectation (I8 40×20 = 5·5·32 = **800**, I'd
  written 4000) — impl was right.
- **AI seam** (`ai_impl.cpp`): Audio-Interface contract as host state (callback swap-and-return,
  DMA params, DSP/stream rates, vols, play state). Real audio stays in `native_audio.cpp`.
- **THP seam** (`thp_impl.cpp`): full `THPPlayer` state machine ported from `THPPlayer.c`; frame
  DECODE + GX YUV draw are documented no-ops (no FIFO/codec in this lib). State values 0..5.
- **GD (16 syms): pure LINK gap** — all defined in `sms-gd`; `sms-boot` now links the full RESCAN
  closure `(sms-native,sms-gd,sms-assets,sms-platform)` like `j3dload_test` (was sms-native+platform).
- **★ REUSABLE: small opaque GC obj on LP64 can't hold a host pointer.** `GXTlutObj` is 12 bytes —
  too small for an 8-byte host `lut` ptr + metadata (alignment rounds to 16). FIX = the obj carries
  `{magic, registry-slot}`; the pointer + fields live in a fixed side-registry (round-robin, bounded),
  `GXLoadTlut` copies the registered descriptor into the renderer-read slot. EXPECT to repeat for any
  ≤12-byte opaque SDK object that must stash a host pointer. (`GXTexObj` is 32 bytes → fits the overlay.)
- **★ REUSABLE: CARD blank-marker must NOT collide with the FAT checksum slot.** `fat_recompute_checksum`
  writes the XOR checksum to `FAT[1]`; keying CARDMount's blank-detect on `FAT[1]==0xFFFF` false-BROKENs
  a card whose data makes the checksum 0xFFFF. Detect via `FAT[0]` (format sets it 0, blank=0xFFFF) or the
  header-sector magic. (Found independently by me + the CARD subagent.)
- **★ PROCESS lesson: fan-out subagents per independent file group, but isolate them.** I omitted
  `isolation: "worktree"`, so the 3 agents wrote into the SHARED tree and their in-progress files
  transiently broke each other's (and my) GLOB build (`sms-platform` globs `*_impl.cpp`). It converged
  (each agent only edits its own file; mine compiled clean so it never blocked them) and the unified
  gate caught everything, but NEXT TIME either pass `isolation: worktree` OR tell agents "write files,
  don't build — I verify centrally." The CARD agent also auto-committed (global commit-on-milestone),
  so its work landed as a separate commit ahead of mine — expected, not a problem.

### NEXT (session 8) — the remaining 270 boot undefs are the GAME C++ CLOSURE (the boot marathon)
The SDK seam layer is DONE. `scratch/boot_undef_after.txt` (270) is now almost entirely game classes:
`TMapObjBase` (12), vtables/typeinfo (~22: TSunShine/TFence/TMarioGamePad/…), and ctors for the
MarNameRefGen actor catalogue (TGenerator/TGuide/TPauseMenu2/TAreaCylinder/…) + globals
(`gpConductor`,`gpMapObjWave`,…). These are the game-object stubs the handoff named — booting needs
them filled (or stubbed enough to reach `PlatformInit → game main → a scene populates a J3DModel`),
which is the only path to a Dolphin-oracle-verifiable COLOR frame. Likely wants the GX/VI integration
harness stood up first. Re-measure: `cmake -B build-native -DSMS_BUILD_BOOT=ON && build sms-boot`,
`grep undefined`. (The other parked frontier, envelope skinning shape-7 overflow 260.74→0, is still open.)

## Don't re-chase
- `port/` (flip) is dead — do not revive. (But its `assets/` BE→host swappers ARE reused — recovered
  into native/assets/ as sms-assets. Recovering more from there via `git show d45a0c3^:port/...` is fair.)
- A blanket host-libc prelude — it conflicts with MSL; shadow MSL instead (`native/shim/host_prelude.h`
  is a leftover sketch of this dead idea, untracked; do NOT wire it in).
- `-fpermissive` to silence pointer-truncation — corrupts pointers under LP64; fix or go 32-bit.
- The TEV runtime-shader "architecture decision" — DONE: glslang lib (glsl_compile.cpp ships it).
- The swap_table=0 "rrrr" trap — a synthetic NgxTevState MUST set swap_table to identity 0x1B.
- NRM frac: GC s16 normals dequant at a FIXED `/2^14` (Dolphin FracAdjust), NOT the attr-list frac —
  shadowpalm (frac=15) decodes to length 2.0 faithfully. Don't "fix" ngx to honor the NRM frac.

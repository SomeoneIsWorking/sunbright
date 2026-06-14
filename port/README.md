# Super Mario Sunshine — native PC port scaffold (FIRST BRICK)

This directory stands up a **native x86-64 build** of the SMS decompilation
(`doldecomp/sms`, vendored read-only at `../reference/sms`) plus the compat-shim
layer every later port track will build on. It is a **parallel, independent**
build from the sunbright recompiler — it does **not** touch `../runtime`,
`../CMakeLists.txt`, or `../build`, and it does **not** modify the pristine
vendored decomp.

The deliverable is `libsmsport_core.a`: a static library of the portable engine
core (math / util / geometry / particle / 2D-graphics-context). It proves the
build pipeline and the shims work. No `main()` — it is a library.

## Build

```bash
cmake -S port -B port/build/cmake
cmake --build port/build/cmake -j"$(nproc)"
# -> port/build/cmake/libsmsport_core.a
```

Verify:

```bash
ar t port/build/cmake/libsmsport_core.a | wc -l     # 85 object files
```

A standalone probe script, `port/try_compile.sh`, compiles an arbitrary list of
decomp sources with the same shim flags and reports PASS/FAIL + the first error
per file (used to curate `core_sources.txt`):

```bash
port/try_compile.sh reference/sms/src/JSystem/JMath.cpp ...
# add -p as the first arg to also try -fpermissive
```

## Compat-shim design

Everything the native build needs that the GameCube/CodeWarrior toolchain
provided implicitly lives under `port/compat/`. Two shim headers are
**force-included** (`-include`) into every translation unit, ahead of any decomp
source; the rest are **shadow headers** resolved via include-path ordering.

### `compat/intrinsics.h` (force-included)
Host implementations of the CodeWarrior PowerPC **compiler intrinsics** the
decomp uses (g++/clang don't provide the `__name` forms):

- `__frsqrte` / `__fres` — **bit-accurate** Gekko/Broadway reciprocal-sqrt and
  reciprocal *estimates* (~12-bit piecewise lookup table), ported verbatim from
  Dolphin's `Common::ApproximateReciprocal[SquareRoot]`
  (`externals/dolphin/Source/Core/Common/FloatUtils.cpp`) — the same math
  `runtime/intrinsics.h` reuses. **Not** `1/sqrt(x)` / `1/x`: MSL's
  `sqrt`/`sqrtf` and the game math (MathUtil, JPAMath) refine the estimate with
  Newton-Raphson, so the exact estimate bits matter. Self-contained (no Dolphin
  link); the lookup tables are inlined here.
- `__fabs`/`__fabsf`, `__cntlzw` (clz, with `clz(0)==32` per PPC), `__fsel`/
  `__fself` (branchless floating select), `__fnabs`.
- `bit_cast_` — a C++17-portable `std::bit_cast` (the std one is C++20).

### `compat/msl_shim.h` (force-included)
The MetroWerks Standard Library (MSL) libc headers under
`reference/sms/include/PowerPC_EABI_Support/Msl/` are kept **off** the include
path (they re-declare the C standard library and clash with / shadow host
glibc/libstdc++). But the game uses a few **non-standard MSL `<math.h>` macros**.
This shim provides them on top of the real system `<cmath>`:

- `M_PI` redefined as a **float** literal (MSL spelled it `3.14159...f`; glibc
  spells it as a double) — the decomp relies on float-typed `M_PI`.
- `TAU`, `LONG_TAU`, `HALF_PI`, `THIRD_PI`, `QUARTER_PI`, `SIN_2_5`, `M_SQRT3`.
- `DEG_TO_RAD` / `RAD_TO_DEG` (the latter keeps the decomp's `+0.000005f`
  fakematch term for bit-identical results).

It deliberately does **not** pull `<cstdio>` globally — that would drag in the
`EOF` macro, which collides with the decomp's enumerator
`enum EIoState { GOOD, EOF }`. stdio/stdarg are handled by shadow headers
(below) only for the TUs that ask for them.

## Shadow-header strategy (override without editing the decomp)

Hard constraint: `../reference/sms` is pristine and stays upstream-tracking. We
override by **shadowing** — placing a corrected header at the same logical path
under `port/compat/include`, which is **earlier on the `-I` path**, so it wins
header resolution while the original stays unreachable/untouched.

| Shadow header | Overrides | Why |
|---|---|---|
| `compat/include/dolphin/types.h` | `reference/.../dolphin/types.h` | **The key fix.** GC was ILP32 big-endian where `long`==4 bytes; the decomp typedef'd `u32`/`s32` as `signed/unsigned long`. On x86-64 (LP64) `long`==8 bytes, silently doubling every `u32`/`s32` field and corrupting struct layouts/sizeof/masks. The shadow typedefs them as `int` (always 4 bytes). |
| `compat/include/stdio.h` | system `<stdio.h>` (for decomp `#include <stdio.h>`) | Pulls the **real** system stdio via `#include_next`, then `#undef EOF` so the game's `EOF` enumerator survives. Also pulls `<stdarg.h>` *first* (some TUs use `va_start` having included only `<stdio.h>`, as MSL allowed). |
| `compat/include/stdarg.h` | system `<stdarg.h>` | `#include_next` to the compiler builtin (va_list/va_start/...); MSL's is off the path. |
| `compat/include/PowerPC_EABI_Support/Msl/MSL_C/MSL_Common/math.h` | the MSL `<math.h>` some TUs include **by full path** (e.g. `JMath.cpp`) | The real MSL tree is off the path, so that include would fail. This shadow routes to system `<cmath>` + the MSL macros. |
| `compat/include/JSystem/J3d/J3DGraphBase/Blocks/J3DTevBlocks.hpp` | a **case-fold** fix | `J3DMaterial.hpp` includes `JSystem/J3d/...` (lowercase `J3d`); the real file is `JSystem/J3D/...` (uppercase). CodeWarrior on Win/macOS resolved case-insensitively; Linux does not. The shadow forwards to the correctly-cased real header. |
| `compat/include/JSystem/JGadget/std-list.hpp` | `reference/.../JGadget/std-list.hpp` | **CodeWarrior-lax template.** In `TList_pointer<T>::iterator` the inner `typedef Base::iterator Base;` re-declares the name `Base` (already meaning the outer `typedef TList_pointer_void Base;`, used in the base-specifier `class iterator : Base::iterator`) to a different type in the same scope → g++ `-Wchanges-meaning` error ("declaration of 'Base' changes meaning of 'Base'"). Gated every TU pulling `std-list.hpp` via `MActorData.hpp` (the whole MActor/M3DUtil cluster). Fix: rename the inner typedef to `BaseIt` (resolves to the identical type). |
| `compat/include/printf.h` | system stdio (for decomp `#include <printf.h>`) | On GC, `<printf.h>` was the MSL header that declared the printf family incl. `snprintf`. With the MSL tree off the path, `<printf.h>` lands on glibc's *internal* `/usr/include/printf.h`, which lacks `snprintf` → inside a template body (`MActorAnmDataEach::loadAnmPtrArray`) g++ rejects the unqualified `snprintf` (`-Wtemplate-body`, two-phase lookup needs the decl). Routes `<printf.h>` to the shadow `<stdio.h>` (real system decls + EOF fix), as MSL did. |
| `compat/include/M3DUtil/MActorData.hpp` | `reference/.../M3DUtil/MActorData.hpp` | **CodeWarrior-lax conversion.** In `MActorAnmDataEach<T>::loadAnmPtrArray` the call `sortByFileNameRaw(unkC)` passes `unkC` (`J3DAnmBase**`) to a `void**` parameter; C++ does NOT implicitly convert `T**`→`void**`. The function is type-erased (only reorders the pointer array, layout-identical), so the fix is an explicit `(void**)` cast — behavior-preserving. |

## Include-path rules (order matters)

```
-I port/compat/include      # 1. shadows WIN (corrected types.h, shadow libc, case-fold, MSL-math)
-I reference/sms/include     # 2. the pristine decomp headers
# (NOT added) reference/sms/include/PowerPC_EABI_Support/Msl  — shadows host libc, won't compile
-include port/compat/intrinsics.h
-include port/compat/msl_shim.h
-std=c++17 -fno-strict-aliasing -Wno-multichar
```

No `-fpermissive`: the core set compiles **cleanly** without it, so the 64-bit
pointer-truncation diagnostics stay ON and act as the porting to-do signal.

## What's in the core lib (85 files)

Curated in `core_sources.txt` (every file compiles cleanly with the shims under
strict `-std=c++17`, **no `-w`/`-fpermissive` masking**). By area:

| Area | Files | Examples |
|---|---|---|
| `JSystem/JParticle` | 16 | JPAMath, JPABaseShape, JPAEmitter, JPAField, JPADynamicsBlock |
| `MarioUtil` | 13 | LightUtil, ShadowUtil, RumbleMgr, EffectUtil, GDUtil, DrawUtil, MapUtil, ModelUtil, ScreenUtil, TexUtil |
| `JSystem/J3D` | 14 | J3DShape, J3DVertex, J3DModel, J3DMaterial, J3DAnimation, J3DJoint, J3DCluster, J3DPacket, J3DMaterialFactory_v21 |
| `M3DUtil` | 9 | MotionBlendCtrl, MActor, MActorAnm, MActorUtil, LodAnm, M3UModel, M3UJoint, SampleCtrlModel, SDLModel |
| `JSystem/JUtility` | 9 | JUTRect, JUTColor, JUTGamePad, JUTPalette, JUTDirectPrint, JUTFont |
| `JSystem/JStage` | 6 | JSGObject, JSGCamera, JSGLight, JSGActor |
| `JSystem/J2D` | 5 | J2DGrafContext, J2DOrthoGraph, J2DPicture, J2DTextBox, J2DWindow |
| `JSystem/JSupport` | 5 | JSUList, JSUMemoryStream, JSUInputStream, JSUOutputStream |
| `JSystem/JGadget` | 4 | linklist, singlelinklist, std-list, std-vector |
| `JSystem` (root) | 2 | JMath, random |

The 2026-06-14 growth (62 → 85) added 23 files: the std-list `-Wchanges-meaning`
fix + the `<printf.h>`/`MActorData.hpp` shadows unblocked the whole MActor/M3DUtil
cluster (MActor, MActorAnm, MActorUtil, LodAnm, M3UModel, M3UJoint, SampleCtrlModel,
SampleCtrlNode, SDLModel) and several J3D animator/loader files
(J3DModel, J3DMaterial, J3DPacket, J3DAnimation, J3DJoint, J3DCluster,
J3DMaterialAnm, J3DClusterLoader, J3DMaterialFactory_v21) plus MarioUtil
DrawUtil/MapUtil/ModelUtil/ScreenUtil/TexUtil — all already-pointer-clean once the
template/header blockers were removed.

## What's deferred (24 files), by reason

Run `port/try_compile.sh` over the full candidate sweep and inspect
`port/build/probe/classified.txt` to regenerate. Categories (after the
2026-06-14 std-list/printf/MActorData header fixes, 47 → 24 remaining):

- **Pointer→`u32`/`s32` truncation — ~13 files (the real 64-bit-port work).**
  The decomp stashes pointers in `u32`/`s32` fields and casts `void*`→`u32`
  (`cast from 'void*' to 'u32' loses precision` — a hard `-fpermissive`-gated
  error in C++, not just a warning). Sound on the 32-bit GC, lossy on
  x86-64/arm64. Fixing means porting those fields/casts to pointer-width types
  (`uintptr_t`) in the **.cpp bodies** — actual engineering, not a shim. The
  shared-header truncations (J3DPacket/J3DMaterial/J3DTexture/JSUConvertOffset…)
  were already fixed in 03f68ac, which is why most of the model graph now
  compiles; what remains is the per-`.cpp` residue (J3DTevs, J3DDrawBuffer,
  J3DAnmLoader, J3D*Factory, J3DModelLoader, JRenderer, J2DPrint, JUTDirectFile,
  JUTTexture, PacketUtil).
- **Inline PPC `asm` — 2 files** (`MathUtil.cpp` `MsVECMag2`/`MsVECNormalize`,
  `J3DTransform.cpp`). CodeWarrior PPC assembly won't parse under g++; needs a
  C/intrinsic reimplementation of those functions.
- **C++11 brace-init narrowing — 4 files** (`JUTConsole`, `JUTResFont`,
  `JUTRomFont`, `JUTException`). A negative constant brace-initialized into an
  `int` field; was legal under CodeWarrior, now `-Wnarrowing`.
- **Covariant-return width mismatch — 2 files** (`JUTResource`, `J2DScreen`).
  Surfaced by the `s32`→`int` fix: `JKRFileLoader::getResSize` is declared
  returning literal `long` while the `JKRArchive` override returns `s32`. Both
  were 4 bytes on GC; now `long`(8) ≠ `int`(4), so the virtual override is
  rejected. Needs the decomp's stray `long`s ported to `s32`.
- **overload resolution — 1 file** (`ToolData`: `getValue(int&, s32&, long*)`
  no longer matches after the type fix), **template two-phase lookup — 1 file**
  (`J2DPane`: `appendChild` calls unqualified `append` from a dependent base;
  CodeWarrior was lax, g++ wants `this->append`), **`jump to case label` — 1
  file** (`MtxUtil`), **`const char*`→`char*` — 1 file** (`MActorData.cpp`
  body — distinct from the now-fixed `MActorData.hpp` template).

The `snprintf`-in-a-template-body and the `std-list` `-Wchanges-meaning`
blockers (which gated the whole MActor/M3DUtil cluster at `#include` time) are
**fixed** as of 2026-06-14 via the `printf.h` / `std-list.hpp` / `MActorData.hpp`
shadows (see the shadow table above).

Several of the narrowing/two-phase/overload cases compile under `-fpermissive`,
but that also masks the genuine pointer-truncation bugs, so the core build keeps
it off and defers them honestly.

## Endian-safe asset loading (`port/assets/`)

**Principle: GameCube on-disk asset formats are BIG-ENDIAN; read every
multi-byte field with explicit big-endian byte assembly, NEVER by casting raw
file bytes to a struct.** The decomp parses formats via struct overlay (e.g.
`JKRArchive::SArcHeader` in
`reference/sms/include/JSystem/JKernel/JKRArchive.hpp` is a plain
`struct { u32 signature; u32 file_length; ... }` cast over the file bytes). On a
little-endian host (x86-64, arm64) that silently misreads every field — a
`file_length` of `0x00084000` reads back as `0x00400800`, names land at bogus
offsets, sizes come out as byteswapped garbage. This is the #1 correctness risk
of the port.

`port/assets/rarc.{h,cpp}` is the native replacement for the outermost
container — **Yaz0 (SZS) decompression + RARC/ARC archive parsing** — written
to the format spec with `be16()`/`be32()` helpers for every multi-byte read. No
struct overlay, no host-byte-order assumption (portable to x86-64 **and**
arm64). API:

- `yaz0_decompress(src, srclen, out)` — Yaz0 magic + big-endian uncompressed
  size + the standard run-length/back-reference scheme.
- `rarc_parse(buf, len, out)` — returns each regular file's name, size, and a
  pointer/offset into the buffer (directories excluded).

`assets/rarc_test.cpp` (CTest target `rarc_test`) proves it: a synthetic
hand-built big-endian RARC fixture (deterministic), a Yaz0 round-trip, and — if
present — the real `scratch/audiores/data/nintendo.szs`. **Verified** against
that real archive: it decompresses (107 016 -> 543 584 bytes) and parses 8 files
with real SMS asset names (`msound.aaf`, `standard_fontex.bfn`, `*.bti`
textures, `nintendo.blo`) at sane sizes — i.e. NOT byteswapped garbage. Run:
`ctest --test-dir port/build -R rarc_test`. Every later asset reader (BTI, BMD,
BCK, ...) follows this same explicit-big-endian rule.

## Files in this scaffold

```
port/
  CMakeLists.txt          build of libsmsport_core.a from core_sources.txt
  core_sources.txt        the 85 clean core sources (paths relative to repo root)
  try_compile.sh          probe: compile a file list with the shim flags, report PASS/FAIL
  README.md               this file
  compat/
    intrinsics.h          force-included PPC-intrinsic shim (bit-accurate frsqrte/fres)
    msl_shim.h            force-included MSL <math.h> macro shim
    include/
      dolphin/types.h     shadow: int-based u32/s32
      stdio.h, stdarg.h   shadow: forward to system libc, fix EOF macro/enum clash
      PowerPC_EABI_Support/Msl/MSL_C/MSL_Common/math.h   shadow: MSL math by full path
      JSystem/J3d/.../J3DTevBlocks.hpp                    shadow: J3d->J3D case fold
  build/                  throwaway output (gitignored): cmake/, probe/
```

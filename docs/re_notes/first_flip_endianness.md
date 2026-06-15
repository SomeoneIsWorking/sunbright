# First-flip blocker: the port engine reads big-endian assets but has no byteswap layer

**Status (2026-06-15): RESOLVED ✅ — the real port `J3DModelLoaderDataBase::load` now loads a
fully-swapped real BMD into a non-null `J3DModelData` with correct joint/shape/material counts for
all 15 test BMDs (`scratch/bmd/load_gate`). Three layers were needed (all done): (1) the BE→host BMD
swap (`port/assets/bmd_swap`), (2) the LP64 struct-overlay fix (`port/compat` block-struct shadows),
(3) a faithful `GDInitGDLObj`/`GDPadCurr32` (`port/pal/gd/gd_stub.cpp`). See the two UPDATE sections
below. The historical blocker narrative is kept for context.**

## What was tried
The plan (handoff) was: flip `J3DModelData` first because it is loader-managed (0 game `new`
sites, 0 subclasses, 0 sweep gaps) — so it dodges the polymorphic-inlined-ctor construction blocker
that killed JUTTexture. Identity would come from a SINGLE seam: bridge
`J3DModelLoaderDataBase::load` (guest `0x802e6f00`) to the **port's native** loader, return a handle.

The port/ link itself was DERISKED this session (see below) — that part works. The blocker is
upstream of it.

## The blocker: endianness
The port's core JSystem loaders (`port/src/J3DModelLoader.cpp`, the J3D factories, JUTTexture's
`storeTIMG`, …) are **source ports of the CodeWarrior decomp that read multibyte fields directly
off the file image** (`fileHeader->mMagic`, `block->mPacketNum`, ResTIMG width/height, …). The decomp
ran on a **big-endian** PPC; the host is **little-endian** x86-64/arm64. The port's shadow
`dolphin/types.h` fixes only the `long`-width (ILP32→LP64) problem — it does **NOT** byteswap.
The only byteswap-aware readers in port/ are the *standalone* asset readers
(`port/assets/rarc.cpp`, `bti.cpp`, and `JKRDecomp` Yaz0) — written from the format spec, NOT the
core decomp loaders.

GameCube asset data (BMD/BDL/BTI/BCK/… inside the ROM) is big-endian and **stays** big-endian in
guest RAM after the archive is decompressed/extracted (the archive layer doesn't rewrite a file's
internal binary blocks). So a bridged port loader fed `sb_guest_to_host(guest_bmd_addr)` reads
big-endian bytes as if host-endian → garbage.

### Proof (empirical, against the actually-linked port lib)
`J3DModelLoaderDataBase::load` dispatches on the fourcc: `if (magic == 'J3D2' && type == 'bmd3')`.
The multichar constant `'J3D2'` is computed by the compiler from char order = `0x4A334432`, host-
independent. A real BMD's first 4 bytes are `4A 33 44 32`; read as a host (LE) `u32` that is
`0x3244334A`. They never match → `load()` returns `nullptr` immediately.

`scratch/endian_probe.cpp` (synthetic BE `J3D2`/`bmd3` header → the linked port loader):
```
magic as host-u32 = 0x3244334A  ('J3D2' const = 0x4A334432)
load() returned: (nil)   <- fourcc dispatch FAILED on this host
```

## Why this generalizes (not just J3DModelData)
- **JUTTexture** (the other "clean" sweep candidate) reads ResTIMG header fields (width/height/format)
  in `storeTIMG`/`initTexObj` — ResTIMG is big-endian guest data → same failure. (JUTTexture also has
  the polymorphic-inlined-ctor blocker, so it was already out.)
- **JKRArchive**, fonts, animation data, particles — every engine type whose state is *parsed from a
  big-endian asset* is blocked the same way.
- Engine types whose state is set **programmatically** (default ctor + scalar method calls, register
  args that cross the bridge as host-endian values) are NOT affected — but they may be rare/awkward
  and most still have the construction-emission blocker.

## The real fix (port-engine work, NOT a flip-mechanism problem)
The port engine must read big-endian assets — the ROM is big-endian and that is not changing. This is
required "own the engine" work regardless of the flip. Two known approaches:
1. **Swap-at-load (format-aware):** byteswap each asset's multibyte fields ONCE, in place, at load
   time, then the pristine decomp loaders read host-endian directly. Needs a per-format swap
   descriptor (BMD/BDL/BTI/…) — i.e. structural knowledge of each format ≈ a second parse.
2. **Swap-on-read:** wrap every multibyte field read in the loaders with a byteswapping accessor.
   Forks the decomp loaders (loses pristineness / diffability).

Both are substantial. This is the gating work for flipping ANY asset-data engine type.

## Status of the port/ → sunbright LINK (the handoff's "heaviest step") — DERISKED ✅
Independent of the endianness blocker, the link integration is proven cheap:
- `add_subdirectory(port …)` with `SMSPORT_BUILD_MAIN=OFF`, link `smsport_core` + `smsport_pal`
  (start-group) into the sunbright target. **Builds clean.**
- The `J3DModelData` loader closure is only **73 undefined symbols**, all PAL (GX/GD stubs, OS,
  PSMTX/PSVEC, DC cache, JKRDisposer); `operator new` resolves from libstdc++.
- **Symbol-conflict cross-check: ZERO real conflicts.** pal's 252 defs vs sunbright's 46596 → 0
  overlap; core's 8282 defs vs sunbright → only `__static_initialization_and_destruction_0` (a per-TU
  LOCAL symbol, never collides). pal defines NO global `operator new`.
- The AT_ADDRESS hardware-global multiply-def (handoff's feared blocker) only bites under
  `--whole-archive` (smsport_main). Normal archive GC (pull-on-demand) does not trigger it for the
  J3DModelData closure — the seam links clean against core+pal alone.
Probes kept: `scratch/flip_link_probe.cpp`, `scratch/endian_probe.cpp`, `scratch/flip_undef.txt`.

## UPDATE (2026-06-15, later): BE asset layer DONE — and a SECOND blocker surfaced (LP64 struct overlay)
The big-endian→host BMD swap layer (Path A) is **complete**: `port/assets/bmd_swap.{h,cpp}` swaps
all 8 J3D2 blocks (INF1/VTX1/EVP1/DRW1/JNT1/SHP1/MAT3/TEX1), verified on 15 real BMDs
(`scratch/bmd/verify_real`) + synthetic `bmd_swap_test`. Stride/landmine notes resolved empirically
(J3DJointInitData stride is **0x40** not the header's 0x30; JNT1 jointNum feeds EVP1's inv-bind count
via a pre-pass; SHP1 display-list interior + TEX1 texel data stay GC-native by design — decoded at
render time).

**But feeding a fully-swapped BMD to the real port loader (`J3DModelLoaderDataBase::load`) SEGVs in
`readDraw` — a SECOND, independent blocker: LP64 struct-overlay layout.** The decomp's file-overlay
block structs (`J3DDrawBlock`, `J3DModelInfoBlock`, `J3DVertexBlock`, `J3DEnvelopBlock`,
`J3DTextureBlock`, `J3DJointBlock`, `J3DShapeBlock`, `J3DMaterialBlock`) declare their
offset members as `void*` / typed pointers. On the GameCube `sizeof(void*)==4` == the 4-byte file
offset, so the overlay works. On a 64-bit host `sizeof(void*)==8`, so (a) every pointer member is
8-byte aligned → field offsets SHIFT off the file layout, and (b) reading one over-reads 8 bytes =
a wild pointer. `readDraw` is the first site that DEREFERENCES a converted offset
(`mDrawMtxFlag[i]`), so it's where the crash lands; the bug is in EVERY block struct.
Evidence: `scratch/bmd/load_gate` (heap bringup → swap → `load`) → SIGSEGV in `readDraw`; the BMD's
`mpDrawMtxFlag` (a 4-byte file offset 0x14) is read as an 8-byte host pointer 0x00540000_00000016.

This is **separate from endianness** and is required "own the engine for a 64-bit host" work. The
fix: shadow the block-struct-declaring headers in `port/compat`, changing the file-offset members
from `void*`/typed-pointer to `u32` (matching the 4-byte file layout AND the decomp's GC semantics).
The loader passes these members to `JSUConvertOffsetToPtr(base, offset)` (which has a `u32` overload),
so a `u32` member yields the correct host pointer. The factories' *internal* pointer members are real
host pointers set at runtime (8 bytes, fine) — only the FILE-OVERLAY `*Block` structs need the change.
(Port/compat already shadows `J3DShapeFactory.hpp` for the `unsigned long` vs `u32` mangling issue —
same shadow-the-header pattern, different LP64 problem.)

## UPDATE (2026-06-15, latest): LOADER GATE GREEN — both blockers fixed + a GD-init bug + a ccache trap
After the LP64 block-struct shadows (above), the loader crashed deeper, in two more spots — both fixed:

1. **GD display-list init was a no-op (memory corruption).** `J3DShape::makeVcdVatCmd` does
   `GDInitGDLObj(&list, mGDCommands, 0xC0)` then calls the header's INLINE `GDWrite_*`/`__GDWrite`
   helpers (via `makeVtxArrayCmd`/`J3DSetVtxAttrFmtv`), which append bytes through
   `__GDCurrentDL->ptr`. `port/pal/gd/gd_stub.cpp` had `GDInitGDLObj` as a NO-OP, leaving the stack
   `GDLObj` uninitialized → the inline writers wrote through a garbage `ptr` → smashed the
   `makeHierarchy` stack → SEGV. Fix: implement `GDInitGDLObj` + `GDPadCurr32` faithfully (per
   `GDBase.c`) so the bytes land in the shape's real `mGDCommands` buffer. `GDFlushCurrToMem` stays a
   no-op (host memory is coherent; the native renderer reads `mGDCommands` directly).

2. **⚠ CCACHE TRAP (cost a long debug detour) — newly-added higher-priority shadow headers don't
   invalidate ccache.** The build uses `/usr/lib64/ccache/c++`. When a `port/compat` shadow header is
   CREATED for a path that previously resolved to `reference/sms`, the source+flags hash is unchanged,
   so ccache returns a STALE object compiled against the OLD (reference) header — the factory ctors
   kept reading the old 8-byte-pointer block layout (disasm showed `mov 0x10(%rax)` instead of
   `0xc`). A fresh build dir did NOT help (same ccache). Symptom: `-H`/manual compile is correct but
   the CMake object is wrong; the `.o.d` depfile lists the reference header. FIX: rebuild affected TUs
   with `CCACHE_DISABLE=1` (or `ccache -C`) whenever a NEW shadow header is added on top of an existing
   include path. (Editing an EXISTING file invalidates ccache normally; only NEWLY-APPEARING
   higher-priority headers slip through.)

The loader gate harness is `scratch/bmd/load_gate.cpp` (heap bringup -> bmd_swap -> `load` -> assert
non-null + sane counts), linked against `port/build-flip2/lib{assets,core,pal}.a`. Build the libs
with `CCACHE_DISABLE=1 cmake --build port/build-flip2 --target smsport_core smsport_pal smsport_assets`.

### Remaining toward the actual flip (loader works; now wire it)
The port loader producing a valid host `J3DModelData` is the gate the handoff's NEXT step 2 wanted.
Still to do (handoff steps 3-4): re-add the `port/` -> sunbright link, `SB_ENGINE_TYPE(J3DModelData)`
+ bridge `J3DModelLoaderDataBase::load @0x802e6f00` (swap in the bridge: copy guest BMD ->
`bmd_swap_to_host` -> port `load` -> handle) + free-fn wrappers for the out-of-line J3DModelData
methods, then `SUNBRIGHT_ENGINE_TYPES=J3DModelData` recompile + oracle-verify. NOTE: the J3DModelData
methods that touch GX/render still run against the GD/GX stubs — field/method-correctness verification
is the realistic near-term check (a textured frame needs the renderer owned in `port/`).

## Next-session decision (open)
Pick the forward path:
- (A) Build the big-endian asset layer for the port loaders (principled, unblocks the whole
  loader-managed category, large).
- (B) Find a programmatic-only engine type (no file parse) for a first flip to validate the full
  pipeline end-to-end now (link + bridge + handle + flipped field access + recompile + oracle).
- (C) Solve the polymorphic-inlined-ctor host-construction (re-enables JUTTexture — but JUTTexture is
  ALSO endianness-blocked via ResTIMG, so (C) alone is insufficient).

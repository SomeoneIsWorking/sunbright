# First-flip blocker: the port engine reads big-endian assets but has no byteswap layer

**Status (2026-06-15): CONFIRMED blocker. Invalidates the handoff's J3DModelData first-flip plan.
Affects the ENTIRE loader-managed / asset-data engine-type category.**

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

## Next-session decision (open)
Pick the forward path:
- (A) Build the big-endian asset layer for the port loaders (principled, unblocks the whole
  loader-managed category, large).
- (B) Find a programmatic-only engine type (no file parse) for a first flip to validate the full
  pipeline end-to-end now (link + bridge + handle + flipped field access + recompile + oracle).
- (C) Solve the polymorphic-inlined-ctor host-construction (re-enables JUTTexture — but JUTTexture is
  ALSO endianness-blocked via ResTIMG, so (C) alone is insufficient).

# GameCube CodeWarrior ABI — findings for the tailored-recomp boundary

RE'd 2026-06-14 while building the decomp header parser (`tools/recompiler/decomp_parse.*`)
and validating `abi_layout` against the SMS decomp's annotated offsets at scale (712 headers,
1032 annotated types, 304 fully-sizable structs cross-checked). These are load-bearing for the
data/object-field half of the game↔engine boundary (`docs/ARCHITECTURE_TARGET.md`).

## Member layout (confirmed correct — matches the decomp annotations)
CodeWarrior (MWCC) for GameCube uses standard PPC-EABI scalar placement, which `abi_layout`
implements and which reproduces the decomp's `/* 0xXX */` / `// _XX` annotations exactly on
every clean struct:
- each scalar at the next offset that is a multiple of its natural alignment
  (i8=1, i16=2, i32/f32/ptr=4, f64/i64=8);
- arrays = N contiguous elements, alignment = element alignment;
- struct align = max member align, size rounded up to it.
The ONLY guest↔host divergence in member placement is **pointer size** (guest 4, host LP64 8)
and the alignment shifts that causes — exactly what makes a host-native engine object's field
offsets differ from the guest's.

## ⚠ Vtable placement is CLASS-DEPENDENT (the surprise — do not assume vtable@0)
For a polymorphic class the vtable pointer is NOT always at offset 0:
- **TNameRef** (root with virtuals): members start at 0x4 → **vtable at offset 0**.
- **TGraphWeb**, **TSolidStack**: members start at 0x0 and the decomp annotates the vtable at
  the END (`/* 0x18 */ // vt`, `/* 0xC */ // vt`) → **vtable appended after the data members**.

So the guest offset of a field in a polymorphic class depends on where that class's vtable
sits, which varies. Two consequences:
1. The earlier de-risk slice's assumption "polymorphic ⇒ vtable@0 ⇒ first member@4" (EngineCam,
   abi_layout_test case 3) is only one of the cases. It is fine as a *modelled* example, but
   the REAL guest offset for a given class must come from the decomp, not be assumed.
2. **The type DB reads each field's absolute guest offset straight from the decomp annotation**
   (`to_engine_layout`), so vtable placement never affects field resolution — we never compute
   it. `abi_layout`'s `polymorphic` flag (vtable@0) is kept only for the cases that match and
   for the host side; it is NOT used to derive guest field offsets for the DB.
3. The corpus ABI cross-check (`decomp_parse_test`) is therefore vtable-agnostic: it validates
   **absolute alignment + non-overlap** of the annotated offsets (the rules `abi_layout`
   encodes), tolerating the unannotated vtable slot / padding holes as gaps.

TODO when scaling: resolve each polymorphic engine type's vtable position from the decomp
(the `// vt` annotation when present; else TNameRef-style vtable@0) — needed for *host*-side
construction and virtual dispatch, not for reading guest fields.

## Decomp-data quirks that are NOT ABI/parser bugs (verified, allowlisted in the test)
- **JAISeqUpdateData / TDSPChannel** — decomp annotates 4 sub-byte fields all at `/* 0x0 */`
  (a WIP placeholder for a 4-byte/bitfield region); overlapping by the decomp's own offsets.
- **HeaderData** (CardManager) — `mComment[0x20]@0x24` overlaps `mBanner@0x40` by 4 bytes; the
  header is internally inconsistent.
- **OSContext** — HW thread context: `f64 psf[32]` at the 4-aligned offset `0x1C4` (paired-
  single registers, deliberately non-8-aligned). An OS struct, not an engine type.

## Parser scope (decomp_parse)
Focused line-oriented parser for the regular decomp member shapes (one annotated member per
line). Handles: both annotation styles, scalars/typedefs→FKind, pointers (incl. function
pointers) → 4/8-byte, arrays with hex/dec/`A - B` dimension expressions, `__attribute__((...))`
stripping, base-class capture, `virtual`→polymorphic, and skips methods/statics/typedefs.
HONEST about the rest: embedded value types (TVec3/Mtx/TParamRT…), enums, and unknown types are
recorded but marked **not sizable** (so they're excluded from the ABI cross-check rather than
guessed). Embedded value types are the next coverage item for the DB (handoff residual risk).

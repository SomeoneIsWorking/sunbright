# Object identity at the tailored boundary — engine-object CONSTRUCTION

RE'd 2026-06-14 (continuation of the field-access boundary, HEAD 1fca757). This is the
"OBJECT-CREATION / IDENTITY flip" the handoff flagged as the hard, design-first piece. The
field-access boundary (read/write an engine object's members host-natively) is DONE; what is
NOT yet handled is where engine objects come from. This doc grounds the design in the REAL
SMS construction patterns (disassembled from `scratch/bin/sms.dol`) and specifies the
mechanism. Tools used: `scratch/dis` (disassembler), `scratch/callers` (BL caller finder),
`scratch/flip_dump` (per-type field recovery dump).

## The problem (restated precisely)
The boundary makes a recompiled `lwz/stw rD, off(rA)` translate to a host-struct member access
**iff** the pass knows `rA`'s static type is an engine type, and `rA` holds a 32-bit HANDLE
(`sb_eng_host(handle) -> host object*`). Type recovery (`recover_eng_fields`) seeds types from
the function's decomp signature and propagates them FORWARD. That works for a method operating
on an already-existing object (its `this`/params are seeded). It does NOT cover the site where
an engine object is first MADE — because at construction the type is not yet attached to any
seeded register; it becomes known only through the constructor/first-method call. Until
construction is handled, a freshly-made object's register is untyped → its inlined ctor field
writes emit guest MEM against raw memory, and the value stored into a container field is a raw
guest pointer, not a handle → downstream `sb_eng_host()` faults.

## The four real construction patterns (all confirmed in the DOL)
Type used as the worked example: **JUTTexture** (out-of-line ctor `__ct__10JUTTextureFii9_GXTexFmt`
@0x802ca4ec; methods storeTIMG @0x802ca640, load @0x802caa3c). The global allocator
`operator new(size)` is **0x802c3ba4** (1949 call sites — reads the current JKRHeap from SDA
`r13-0x5f2c`, virtual-dispatches `alloc(this,size,4)`). A second allocator-ish helper is
0x802fa69c (66 sites; not yet characterized — likely a typed/array helper).

### Pattern A — heap `new`, OUT-OF-LINE ctor
`load__14TScreenTexture` @0x8022d4xx (the ONLY direct caller of the out-of-line JUTTexture ctor):
```
li    r3, 0x54                 ; sizeof(JUTTexture) (guest)
bl    0x802c3ba4               ; r3 = operator new(0x54)  -> raw guest buffer
mr.   r29, r3                  ; save the new ptr; null-check
beq   skip
...                            ; compute w (r4), h (r5); li r6,4 (fmt)
mr    r3, r29                  ; this = saved buffer
bl    0x802ca4ec               ; __ct__JUTTexture(this=r29, w, h, fmt)   (ctor returns this in r3)
stw   r29, 0x10(r30)           ; container.field = r29   <-- the SAVED ptr, NOT the ctor return
```
**Crux:** the value stored into the container field is `r29` (the `operator new` result), not the
ctor's return r3. So "bridge the ctor as a factory that returns a handle" does NOT match this
codegen — the stored token must come from the ALLOCATION. Identity must attach at `operator new`.

### Pattern B — heap `new`, INLINED ctor (the handoff CRUX)
`new JUTTexture(timg)` with the `(const ResTIMG*)` ctor inlined:
```
r3 = operator new(0x54)
stw  rZero, 0x28(r3)           ; inlined ctor: mEmbPalette = 0
bl   0x802ca640                ; storeTIMG(this=r3, timg)
stw  rZero, 0x50(r3)           ; inlined ctor: unk50 = 0
```
No `bl ctor`; the only type signal is `bl storeTIMG` (a JUTTexture method) taking r3 as `this`.

### Pattern C — STACK temporary (also real: `drawRevivalTexStamp__22TPollutionCounterLayerC`)
```
li    r25, 0
addi  r3, r1, 0x28             ; this = &stackframe[0x28]   (interior stack address)
stw   r25, 0x50(r1)            ; inlined ctor: unk50 = 0  (frame-relative)
bl    0x802ca640               ; storeTIMG(this=sp+0x28, timg)
addi  r3, r1, 0x28
bl    0x802caa3c               ; load(this=sp+0x28, mapid)
```
The object lives in the caller's STACK FRAME (size baked into the frame). `this` is `addi r1,off`.
Host port needs a host-sized JUTTexture somewhere (host stack object or a frame-slot handle).

### Pattern D — method on an EXISTING, field-loaded object (`changeTexture__10J2DPicture`)
```
lwz   r31, 0x20(r6)            ; this = container->mTexture   (already an engine field)
bl    0x802ca640              ; storeTIMG(this=r31, timg)
```
Already handled by field recovery IF the container is typed (the field 0x20 is declared a
JUTTexture* in the container's layout → loaded as a handle). No new mechanism needed; this is the
pay-off of getting A/B/C right (the field that A/B/C stored a handle into is read back here).

## The unifying mechanism — forward ALLOCATION-ORIGIN analysis (IMPLEMENTED + real-code verified)
Across A/B/C the single signal is: **a register passed as `this`/arg0 (or an engine-typed param)
to a KNOWN engine method/ctor IS that engine type.** The type of a freshly-made object is not known
at its allocation — only at this first use — so it must reach the allocation/ctor-writes somehow.

**Backward DEMAND propagation is the obvious idea but is UNSOUND** here: the ubiquitous
`p = new T; if (p==null) skip; <use p>; skip:` makes a CFG merge whose MEET (intersect) intersects
the demand on the used path away against the skip path, so the demand never reaches `operator new`
(this actually failed on the real TScreenTexture code). Worse, a MAY/join meet would mis-type field
WRITES across a redefine-on-one-arm merge (the SHARP EDGE).

**The chosen, sound mechanism is FORWARD** (`type_recovery.cpp` `find_alloc_sites` + `apply`'s
`alloc_types` seed): track each register's allocation ORIGIN forward — the pc of the `operator new`
bl, or the interior-stack `addi` — through copies (`mr`, `addi rD,rA,0`) and prologue spill/reload.
The origin survives the null-check merge cleanly (the skip path branches AROUND the use, so the MEET
on the used path is undiluted). When an origin-carrying register is used as a known engine method's
`this`/engine-arg, resolve `{origin pc -> engine type}`. A second forward TYPE pass then SEEDS those
origins with their resolved type, so the new object's type flows forward normally — typing the
inlined ctor writes and the container store of the handle. Opt-in via `recover_eng_fields`'s
`raw_allocators` + `alloc_sites` params (default-off, so the forward-only callers are unchanged).
VERIFIED on real DOL code: Pattern A flags `operator new` @0x8022d498 → JUTTexture (surviving its
null-check); Pattern C flags the three stack-temp `addi r3,r1,0x28` sites in `drawRevivalTexStamp` →
JUTTexture; Pattern B inlined-write typing proven on the unit test (same alloc-seed → forward type).

The DEFINITION site decides the storage translation:

| def of the engine-`this` register | meaning | translation |
|---|---|---|
| `bl <raw allocator>` (operator new 0x802c3ba4) | heap `new` | REWRITE the alloc to a host alloc returning a HANDLE; type the result reg from the alloc fwd (Patterns A, B) |
| `addi rD, r1, off` (interior stack addr) | stack temporary | host-side: a host engine object in a side table keyed by (frame, off) → handle; type the reg (Pattern C) |
| `lwz rD, off(container)` where container is engine/known | loaded field | the field is engine-typed in the container layout → already a handle (Pattern D) |
| a seeded parameter | passed in | already typed (existing forward pass) |
| `bl <bridged engine-returning fn>` (factory) | factory result | bridge already returns a handle → just type the return (forward) |

Why back-typing is unavoidable: the inlined ctor field writes (B) and the stack-temp ctor writes
(C) occur BEFORE the call that reveals the type. A purely forward pass cannot type them. (Verified:
`recover_eng_fields` leaves r3 untyped at the Pattern-B inlined writes.)

### Distinguishing a raw allocator from a factory
`operator new` returns RAW untyped memory; the type attaches only via the following ctor. A
factory returns an already-typed engine handle. So the rewrite-to-host-alloc rule keys on an
explicit set of RAW-ALLOCATOR addresses (`{0x802c3ba4}` to start; add new[]/array + JKRHeap::alloc
variants as found). Only when a raw-allocator result flows into an engine ctor/method as `this`
is that specific call site rewritten — `operator new` itself is generic (1949 sites) and must NOT
be blanket-rewritten.

## Runtime model — construction = host alloc + placement ctor
Mirror C++ `new T(args)` faithfully, with HOST storage:
- alloc rewrite emits `sb_eng_alloc(sizeof(HostT))` → `HostT* p = (HostT*)::operator new(sizeof(HostT)); return sb_eng_handle(p);`
  (raw host storage, registered, returns a handle; NO ctor yet — matches `operator new` semantics).
- the ctor `bl` is bridged normally with `this`=handle → the ctor bridge does
  `new (sb_eng_host(h)) JUTTexture(args)` (placement-new on the host storage) and returns h.
- for the INLINED ctor (Pattern B) there is no ctor bridge; the inlined member writes, now typed,
  emit as host-struct member writes on `sb_eng_host(h)` directly — which IS the construction.
- container field store of the handle is a plain 32-bit guest store (game objects stay
  guest-layout; the handle is just a 32-bit token living in guest RAM). Pattern D reads it back.

Destruction mirrors: a `delete`/`__dt` site releases the handle (`sb_eng_release`) + host `operator
delete`. Stack temps (C) release at scope end (the frame teardown) — needs a scope model.

## Implementation phasing (each oracle-verified on a thin slice before scaling)
1. **Recognition primitive — DONE (forward origin analysis), unit-tested + real-code-verified.**
   `type_recovery.cpp`: `find_alloc_sites` (forward origin fixpoint) → `{site pc -> engine type}`;
   `apply`'s `alloc_types` seeds those sites in the TYPE pass so inlined ctor writes resolve.
   Opt-in params `raw_allocators` + `alloc_sites` on `recover_eng_fields` (off by default).
   Tests: `type_recovery_test` cases 10–13 (Patterns A/B/C + negative). Real-code: `scratch/
   identity_check JUTTexture 8022d474 8019b7d8`. **NOT yet wired into the emitter (step 2).**
2. **Emitter alloc rewrite + `sb_eng_alloc`** — DONE (emission half). `EmitContext::alloc_sites`;
   a flagged heap site emits `cpu.gpr[3] = sb_eng_alloc<T>()` (runtime `intrinsics.h`: raw
   `::operator new(sizeof(T))` + `sb_eng_handle`); the inlined ctor writes (typed via eng_fields)
   init the HOST object; the following engine method stays a bridged `call_ppc`. Wired into
   `main.cpp` (raw_allocators={0x802c3ba4}, no-op when SUNBRIGHT_ENGINE_TYPES is empty). Test:
   `recomp_test` "OBJECT-IDENTITY emission". KEY FINDING (unblocked this): **JUTTexture is
   NON-POLYMORPHIC** (no `virtual` in the header; its ctor stores `stb`/`stw` to scalar fields, NO
   vtable store to `0(this)`), so raw alloc + the typed inlined writes faithfully mirror the guest
   `operator new + ctor` — no host-ctor injection needed for the inlined case.
   **END-TO-END PROVEN — Patterns A AND B (stub type):** `runtime/tests/run_construct_slice_test.sh`
   runs recompiled `new EngineTex` both ways — ORACLE (guest buffer) vs TAILORED
   (`sb_eng_alloc<EngineTex>()` host object) — and verifies the host object's members get the
   oracle's field values, with host offsets DIFFERENT from the guest displacements. PASS for both
   the INLINED ctor (B) and the OUT-OF-LINE ctor (A). **KEY INSIGHT (A): a non-polymorphic
   out-of-line ctor needs NO special bridge** — when the ctor function is recompiled TAILORED, its
   own `this` field writes are host-native (its `this` is signature-seeded), so it constructs the
   host object directly. So for non-polymorphic engine types the whole heap-construction path
   (A + B) is `sb_eng_alloc` + ordinary tailored recompilation; nothing else.
3. **Stack-temp (Pattern C) emission — DONE.** A flagged interior-stack `addi rD,r1,off` becomes a
   RAII host object: the emitter declares one `SbStackObj<T>` local per distinct frame slot at the
   top of the generated function (`runtime/intrinsics.h` — raw aligned storage + a handle, released
   on its dtor) and the `addi` yields `_eng_stack_<off>.handle()`. C++ RAII gives exactly the
   guest stack temporary's function-activation lifetime — no manual lifetime tracking, no leak — and
   every re-materialized `addi` for the same slot aliases ONE host object (same handle). The
   recompiled tailored inlined ctor/dtor init/destruct the storage via host field access (like the
   heap path). Tests: `recomp_test` "stack temporary" + `construct_slice` Pattern C (write off
   addi#1, read back off a re-materialized addi#2 → same object → matches the oracle). All four
   patterns (A/B/C heap+stack construction, D field-loaded) now handled for NON-POLYMORPHIC types.
   **POLYMORPHIC types — PROVEN too (stub):** an out-of-line PLACEMENT-NEW ctor bridge
   (`new(sb_eng_host(h)) T(args)`) runs the real C++ ctor so the HOST vtable is set — the
   recompiled/inlined-ctor path can't (the guest stores a guest vtable address). `construct_slice`
   Pattern AV: `sb_eng_alloc<EngineTexV>()` (raw) + a bridged ctor wrapper (`SB_ENGINE_TYPE` marshals
   `this` from a handle; `new(self) EngineTexV(); self->mWidth=w`) → the host virtual call resolves
   (`kind()==42`) and the field is set. So the bridge is just a per-type wrapper, no recompiler
   change. This is the path J2DWindow::Texture (the sweep gap) needs. ALL construction cases are now
   proven end-to-end with stubs. Real JUTTexture/J2DWindow still gated on GX + port/ link.
3. **Stack temporaries (Pattern C)** — interior `addi r1,off` typed as engine → host side object +
   handle + scope release. Heavier (needs the frame/scope model); defer until A/B are solid.
4. Scale: sweep allocation recognition across all engine-type ctors/methods; confirm no
   mis-recognition (a non-engine use of an operator-new result must stay guest memory).

## SCALE VALIDATION (2026-06-15, `scratch/identity_sweep <Type>` over all 9680 DOL funcs)
Recognition behaves across the whole game, not just hand-picked callers. JUTTexture: 21
construction functions, 30 heap-new sites, 36 stack-temp sites — and **1 function with 9 gaps**:
`__ct__9J2DWindow` (the SHARP EDGE firing as designed). J3DModelData: 0 constructions, 0 gaps
(it is loader-managed, never `new`'d). The gap finding is REAL and important:

**The gap is a POLYMORPHIC SUBCLASS's vtable store — the polymorphic-construction case.**
J2DWindow constructs `J2DWindow::Texture : JUTTexture` (J2DWindow.hpp:28), which adds
`virtual ~Texture()` → it IS polymorphic even though its JUTTexture base is not. `li r3,0x58 ;
bl operator new` (0x58 = JUTTexture 0x54 + a 4-byte vtable ptr CodeWarrior appends at the END for
a base-most-polymorphic class, per abi_findings.md), then the inlined ctor does `bl storeTIMG(base)`
+ `lis r0,0x803e ; addi r0,r0,0x77c ; stw r0,0x54(this)` — i.e. **store the Texture vtable @0x54**.
Recognition types `this` as JUTTexture (correct — it's the base subobject passed to a JUTTexture
method), so the vtable store at 0x54 is a GAP. This is the SHARP EDGE doing its job: it flags the
exact case the raw-alloc + typed-inlined-writes path CANNOT handle — a polymorphic construction
whose guest vtable-address store can't be replayed host-side (the host vtable address differs and is
set only by running the real C++ ctor). So **a type whose game-constructed subclasses are polymorphic
must route those constructions through the out-of-line PLACEMENT-NEW ctor bridge** (Pattern A bridge:
`new(sb_eng_host(h)) T(args)` runs the real ctor → real host vtable). The tell is the alloc size
(`li` before `operator new`) > the base type's size, and/or a `.data` vtable-address store at the
appended offset. GATE: a clean flip requires the sweep's gap report EMPTY for the active set —
`scratch/identity_sweep <Type>` is that gate. For JUTTexture, J2DWindow::Texture (+ JUTPalette) is
part of the closure: either flip those polymorphic subclasses via the ctor bridge, or scope the
first slice to callers that don't construct them.

## MOST-DERIVED-TYPE RECOGNITION (2026-06-15) — the J2DWindow sweep gap is CLOSED
The SCALE-VALIDATION gap above (`__ct__9J2DWindow` constructing the polymorphic subclass
`J2DWindow::Texture : JUTTexture`, 9 unmapped-offset gaps at the appended vtable) is now resolved at
the RECOGNITION level. The `li` before `operator new` is ground truth for the constructed object's
guest SIZE; the type resolved from the first engine-method call is only that method's `this` type
(the BASE subobject), so it under-reports a subclass. The fix prefers the most-derived KNOWN subclass
whose guest size matches the allocation `li`:
- **`tools/recompiler/decomp_parse.cpp`** now parses a NESTED qualified type (`Outer::Inner`) by
  narrowing to the enclosing class body (`locate_class` + the `::` split). So `J2DWindow::Texture`
  is parseable (base JUTTexture, polymorphic, 0 own data fields).
- **`tools/recompiler/type_db_build.cpp`** computes each type's GUEST SIZE (`guest_size_impl`: own
  fields + single-inheritance base chain + the CodeWarrior vtable slot — appended at base size for a
  class introducing virtuals over a NON-polymorphic base, per abi_findings.md; 0 = indeterminable),
  scans every header for class/struct defs with qualified names + first base (`scan_header_types`,
  enclosing-scope stack), and `discover_subclasses` pulls the active types' subclasses into the DB by
  leaf-name base matching. `build_type_db` now fills `TypeDB::sizes` + `TypeDB::bases` for the active
  set AND its subclasses, and `compose_layout` MAPS the appended vtable slot (`__vtbl`) so the store
  is recognized, not an unmapped gap. Verified: JUTTexture=0x54, J2DWindow::Texture=0x58 (base+vtable),
  base link → JUTTexture, layout maps @0x54.
- **`tools/recompiler/type_recovery.cpp`** `find_alloc_sites` captures the `li r3,<size>` before each
  `operator new` bl (`alloc_size_before`, bounded backward scan) and upgrades the signature-resolved
  type to `most_derived(db, base_ty, size)` — the deepest type in `db.bases` chain whose `db.sizes`
  matches. A wrong/unknown size can only MISS an upgrade, never force a false one (the guest `li` is
  the comparand), so it is the honest fallback. Stack-temp origins (an interior addi, no size signal)
  keep the signature type.
Tests: `type_recovery_test` cases 14–16 (subclass-by-size / base-size-no-upgrade / unknown-size
fallback); `type_db_build_test` (real-header discovery of J2DWindow::Texture + sizes + base + vtable
slot). **`scratch/identity_sweep JUTTexture` now reports `0 gaps in 0 funcs`** (was 9 gaps in
`__ct__9J2DWindow`) — and the only way @0x54 maps is if the alloc was upgraded to J2DWindow::Texture
(that offset exists in NO other type's layout), so the gap-clear proves the upgrade fired.

### ⚠ REQUIRED EMITTER FOLLOW-UP before flipping a type with a polymorphic subclass
Recognition now correctly TYPES a `new J2DWindow::Texture` as the polymorphic subclass. The emitter
must then ROUTE that construction through the out-of-line PLACEMENT-NEW ctor bridge (Pattern AV:
`new(sb_eng_host(h)) T(args)` runs the real C++ ctor → real host vtable) — it must NOT take the
raw-alloc + inlined-write path, because the inlined `stw <guest vtable addr>, 0x54(this)` cannot be
replayed host-side (the `__vtbl` slot is a recognition marker, not a writable host member). Until that
routing exists, do NOT add a polymorphic subclass (or a base whose game-constructed subclasses are
polymorphic, e.g. JUTTexture) to `SUNBRIGHT_ENGINE_TYPES`. The gate is unchanged: `identity_sweep
<Type>` must report ZERO gaps AND every flagged polymorphic construction must reach the ctor bridge.

## Open questions / risks (do NOT claim solved)
- **Stack temp scope/lifetime** (C): when is the host object freed? Frame teardown model needed.
- **new[] / array ctors** and the second allocator 0x802fa69c — uncharacterized.
- **Placement-new vs the host ctor's own allocation** — host JUTTexture ctor must not itself
  allocate the object (only its members). True for value/JKR types; verify per type.
- **vtable for polymorphic engine types** — RESOLVED for JUTTexture (non-polymorphic, see step 2).
  A polymorphic type's INLINED ctor sets the vtable via a guest-address store we can't replay on
  the host; such types MUST go through the out-of-line ctor placement-new bridge (or get a host
  vtable fix-up) — do not flip a polymorphic type with an inlined ctor on the raw-alloc path.
- **Multiple inheritance / vtable-at-end** ctors (see abi_findings.md) — the `this` adjustment
  (`addi this, base, +subobjOffset`) at a base ctor call must not be mistaken for a stack-temp.
- A MISS here is a correctness bug (raw guest pointer reaches `sb_eng_host` → fault), same SHARP
  EDGE as field recovery. Recognition must be COMPLETE for engine ctors/allocs.

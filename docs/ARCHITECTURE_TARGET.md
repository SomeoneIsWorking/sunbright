# Sunbright — THE target architecture (READ THIS FIRST)

## ⛔ CORRECTION (2026-06-15, user) — THE BOUNDARY IS FUNCTION CALLS ONLY. NO FIELD-FLIP.
The "data / object-field access" boundary described below (§"The game ↔ engine boundary" half 2,
the DE-RISK sections, `SUNBRIGHT_ENGINE_TYPES`, `recover_eng_fields` field-mapping, `eng_accessors`
field thunks, `emit_eng_field`/`sbf_`) was a **WRONG TURN** invented by prior sessions. The user
never asked for it. Verbatim intent: *"I never asked for a flip. I wanted PC native game engine
running recompiled GameCube game code. The gameplay logic stays in GameCube recomp. PC engine drives
it."* The no-Dolphin goal STAYS; the field-flip GOES.

**The real architecture:** PC-native engine (`port/`) ON TOP, **driving** recompiled GameCube
gameplay logic underneath. The boundary is **function calls only**:
- **Engine = `port/`** — J3D/J2D/JKR/JDrama/JUT/JAudio/renderer/platform, AND any function that
  manipulates engine-object internals (e.g. `SMS_SettingDrawShape`). Owns the frame loop.
- **Gameplay = recompiled** — Mario, enemies, NPCs, camera behavior, items, map. Runs on
  **guest-layout** objects in our arena; it **NEVER field-derefs an engine object**.
- **Boundary = function calls**: gameplay→engine method calls (bridged override; an engine pointer
  crosses as a 32-bit **handle**, never as flipped host-layout data), and engine→gameplay callbacks
  (the engine calls recompiled `perform`/`update`). Construction of an engine object by gameplay
  routes to a bridged factory (op-new site → host alloc + handle + ctor bridge). Virtual calls on an
  engine handle route to the host method (a boundary mechanism, not a flip).
- **Drawing the split:** choose the recomp/port line so gameplay never reads engine fields. Where
  SMS blurs it, PORT that function (move the line) or add a **bridged getter** (a call) — never a
  field-flip.

SURVIVES the correction: `port/` engine; `runtime/bridge.h` + handle table (`runtime/eng_handle`) +
`SB_ENGINE_TYPE` marshalling; the J3D load/ctor/calc/viewCalc bridges + overrides (2026-06-15, all
function-call bridges); `func_sig`/`decomp_parse` for SIGNATURES (which arg is an engine ptr→handle).
RETIRED — REMOVED from the code 2026-06-15 (the field-flip cleanup): the `SUNBRIGHT_ENGINE_TYPES`
field-flip path (`main.cpp`), `emit_eng_field`/`sbf_`/`sbnew_`/`sbsizeof_` + the `EngAccessorTable`
(`c_emitter`), the `eng_accessors.{h,cpp}` generation + its CMake object library, the runtime flip
glue (`eng_accessor_rt.h`, `SbDynStackObj`, `sb_eng_alloc`, `sb_host_to_guest`, `sb_set_guest_ptr`),
the engine-internal-method flip exclusion, and the field-slice/construct-slice/flip-compile de-risk
tests. KEPT as the surviving reg->engine-type DATAFLOW + signature infra (the function-call boundary
— virtual-dispatch-on-handle routing + bridge marshalling — reuses them): `type_recovery`
(`recover_eng_fields`, `EngField` moved here), `type_db_build`, `decomp_parse`, `func_sig`,
`abi_layout` and their unit tests; `runtime/bridge.h` `SB_ENGINE_TYPE`, `runtime/eng_handle`, the J3D
bridges/overrides. See memory `no-field-flip-boundary`. **Everything below this banner is the OLD
(field-flip) framing — kept for history; the function-call half (§1) is still right, the data half
(§2) is DELETED, not "solved".**

Authoritative as of 2026-06-14. **Supersedes the conflicting framing in
`docs/native_port_plan.md`, `docs/native_recomp_bridge.md`, and the source-port memory.** If
those disagree with this doc, this doc wins. Written because the two parallel tracks (`port/`
source-port vs `runtime/` native renderer) were redundant/confusing and a single coherent path
had to be chosen.

## North star (user, 2026-06-14, exact intent)
A single native PC binary, **no Dolphin**, that is:
- **Engine = a real PC game, in PC-native C++** — JSystem (J3D/J2D/JKR/JDrama/JUtility),
  JAudio, the renderer, and the platform (timing/threads/I/O/input), written as host-native C++
  with host objects, host ABI, host endianness. This is `port/` (source-ported from
  `reference/sms`).
- **Game behavior = recompiled, NOT hand-ported** — Mario, enemies, NPCs, camera, map, items.
  But via a **TAILORED recompiler** so the recompiled game code speaks the **PC engine's**
  ABI/layout, not the GameCube's. "GameCube game logic adapted to call the PC engine."
- One path. No env toggle selects native-vs-Dolphin or native-vs-GC-engine ([[done-right-over-working]]).

**Why not the alternatives** (settled — do not relitigate):
- *Recompile everything + native platform only* (keep the GC engine as recomp): rejected — the
  engine would still be GameCube code, not a PC game engine. The user wants the engine to BE PC.
- *Hand-port everything incl. game logic* (full source-port): rejected — ~9,700 funcs of
  game/actor logic is infeasible to hand-port; the recompiler is the port for game behavior.
- *port/ host-native engine + plain recomp game logic*: does NOT work as-is — recomp reads
  engine fields by raw guest offset/endianness, incompatible with host objects. The **tailored
  recompiler is exactly the bridge** that makes this combination work.

## The game ↔ engine boundary (the crux)

Two halves, very different difficulty:

1. **Function calls — TRACTABLE (mechanism proven).** Recompiled game code calling an engine
   function → the recompiler emits a marshalled native call into the PC engine, using the
   signature from the decomp symbol map (`reference/sms_gmse01_funcs.txt` + the decomp headers).
   This is the `SUNBRIGHT_BRIDGE` marshalling thunk (`runtime/bridge.h`, built + unit-tested
   2026-06-14) — applied at recomp time, not just as a runtime override. Virtual calls (`bctrl`
   through a vtable) dispatch to the **host** vtable using the static type.

2. **Data / object-field access — THE HARD, LOAD-BEARING CORE.** Recompiled game code reads
   engine-object fields as raw `lwz rD, off(rA)` — guest offset, big-endian, 32-bit. To hit a
   host-native engine object (different offsets / endianness / pointer width) the recompiler must
   know the **type** of `rA` at that site and translate to the host field/accessor. This needs
   **type recovery** — but *seeded* type recovery: every function's `this`/parameter types are
   known from the decomp signatures, so types propagate through dataflow rather than being
   recovered blind. Feasible, but this is the **single hardest, least-proven piece of the whole
   project** (harder than the renderer).

   OPEN mechanism choices (decide with the de-risk slice, do not assume):
   - Do GAME objects stay guest-layout (engine objects host-native, boundary translated), or do
     game objects also become host-native? Leaning: game objects guest-layout, engine objects
     host-native, recompiler translates at the boundary.
   - Engine-object pointers held in guest memory: raw host pointer (needs 64-bit slots) vs handle.
   - Embedded engine value types in game structs (e.g. `JGeometry::TVec3` inline): layout matches
     but endianness differs — marshalled at the boundary or kept guest-side.

**DE-RISK FIRST (mandatory before committing the whole port):** prove a thin vertical slice —
ONE real recompiled game function that (a) calls a real `port/` engine function through the
tailored boundary AND (b) reads/writes one engine-object field — end-to-end, verified against the
oracle. The function half is proven (the thunk); the data half is the experiment. If type-aware
translation of field access doesn't come out clean on the slice, escalate before scaling.

**DE-RISK RESULT (2026-06-14): GREEN — the emission mechanism is proven.**
`runtime/tests/run_field_slice_test.sh` (+ `field_slice_gen.cpp`, `field_slice_test.cpp`) runs ONE
real recompiled game function — `mr; lfs f1,8(this); bl eng_scale; stfs f1,8(this); blr` — through
the **real** decoder + collection + emitter, twice: ORACLE (raw guest-layout `MEM_RF32`,
big-endian, guest offset 8 = what today's recomp/Dolphin emits) and TAILORED (host-native, baked as
`((EngineCam*)sb_eng_host(cpu.gpr[3]))->mFov`). The host `EngineCam` has an 8-byte pointer member,
so `mFov` sits at **host offset 16** while its guest offset is 8 — a load-bearing divergence. Both
paths call the SAME bridged engine fn (`eng_scale`, via `SUNBRIGHT_BRIDGE`) and both produce 7.0;
TAILORED's host field write == ORACLE's guest field write. So a recompiled function CAN read a host
field, call an engine fn through the boundary, and write a host field back, with correct host
offset/endianness/pointer-width. The new emitter capability is `EmitContext::eng_fields`
(`tools/recompiler/c_emitter.{h,cpp}` `emit_eng_field`): at a typed load/store it bakes a direct
host-struct member access instead of `MEM_R*/MEM_W*`. Mechanism chosen for the slice (marked
explicitly): **engine pointers are 32-bit HANDLES** (`sb_eng_host(token)→host obj`), keeping the
recomp register file 32-bit; game objects stay guest-layout, engine objects are host-native.

**DE-RISK #2 progress (2026-06-14, same commit family): real recovery + the hard dataflow.**
The hand stub is replaced by a real recompiler pass `tools/recompiler/type_recovery.{h,cpp}`
(`recover_eng_fields`): signature-seeded (`this`/param engine types), propagated forward through
register copies, the **prologue stack spill/reload of `this`** (frame-slot type tracking — the
dominant real pattern: the compiler saves `this` to the stack and reloads it into a non-volatile
across a call), **chained field access through a nested engine pointer**, with volatile-clobber at
calls and conservative invalidation elsewhere. Unit-tested (`type_recovery_test`, 7 checks) AND run
end-to-end through the real emitter + oracle on a harder accessor (`stw this; lwz mNext; lfs
mNext->mFov; bl eng_scale; lwz this(reload); stfs this->mFov`) — tailored host == oracle guest.
KEY FINDING (settles an OPEN choice): an engine-pointer FIELD can't hold a raw 64-bit host pointer
in the 32-bit recomp register file, so a load of such a field yields a **handle**
(`sb_eng_handle`, the inverse of `sb_eng_host`) and a store consumes one — this is what makes
`obj->next->field` chains work. So: engine objects host-native with real host pointers between
them; recompiled code sees handles at the boundary.

**DE-RISK #2 step 1 DONE (2026-06-14, second commit family): auto DB + CFG recovery +
COMPLETE-coverage proof on REAL code.** The hand-built DB and straight-line pass are replaced
by the full auto pipeline, each unit-tested + ctest:
- `tools/recompiler/decomp_parse.{h,cpp}` — parse engine field lists (name, FKind, nested type,
  guest offset, base, polymorphic) from the decomp headers; validated against the decomp's own
  `/* 0x */` annotations on **304 real structs** (0 ABI/parser bugs; 5 residuals are documented
  decomp-data/HW quirks). FINDING: the CodeWarrior vtable is NOT always at offset 0 (base-most
  polymorphic classes append it at the END) — `docs/re_notes/abi_findings.md`; the DB reads
  absolute offsets from annotations so this never affects field resolution.
- `tools/recompiler/func_sig.{h,cpp}` — GNU-v2 demangler → `this`/param engine-type seeds
  (PPC-EABI GPR assignment, floats→FPR). Caveat: assumes instance methods (static methods, a
  minority, would mis-seed r3; to resolve via the header's `static`).
- `recover_eng_fields` is now a **forward CFG dataflow fixpoint** (meet at joins, loops
  converge) and reports the dangerous "typed base / unmapped offset" misses.
- **GATE: `coverage_real_test`** runs the whole pipeline on REAL DOL bytes and asserts COMPLETE
  coverage vs the decomp SOURCE oracle — proven on `TCameraMarioData::isMarioGoDown` (reads
  `unk10`; global derefs correctly untyped) and the branchy `::calcAndSetMarioData` (5 fields
  r/w across switch + if/else). 0 misses. So: emission equivalence (field_slice) + coverage
  (this) are both green for single-object straight-line AND branchy accessors.

**Still NOT proven (the residual risk — do NOT claim solved):**
- *Coverage at SCALE / harder shapes.* Proven on two single-object accessors of one clean type.
  Not yet: inheritance (base-subobject layout must be prepended — `decomp_parse` captures the
  base name but doesn't compose layouts yet), embedded value types (TVec3/Mtx/TParamRT — parsed
  but marked not-sizable, so their sub-fields aren't in the layout), the static-method seed
  caveat, and a broad sweep over the ~9,700 game functions. A MISS is a *correctness bug*
  (guest MEM access against a handle), not a safe fallback — coverage must be COMPLETE.
- *Pointer-into-object / interior addresses.* `&engineObj->field` (an `addi` by a nonzero offset)
  is deliberately left untyped (the pass is honest about it); deref'ing it under a different static
  type, or pointer arithmetic into an embedded sub-object, is NOT covered.
- *Embedded value types* (inline `JGeometry::TVec3` etc. — layout matches but endianness differs at
  the boundary) — not yet exercised.

## Consolidation — what each tree is now

| Tree | Role in the target | Notes |
|---|---|---|
| `port/` | **The PC-native engine** (central). | Source-ported JSystem/JAudio + renderer + platform. Eventually owns rendering from host objects. KEEP — not redundant. |
| `tools/recompiler` | **Tailored** to target the PC engine. | The new core effort: emit engine-aware calls + type-aware field translation for game logic. |
| `runtime/` | Recomp runtime, dispatch, the bridge, platform glue. | Its native renderer (reads **guest** J3D objects) is **TRANSITIONAL**: the Vulkan backend + `tex_decode` are reusable as the engine's GPU backend, but the "read guest objects" front is superseded once the engine renders its own host objects. |
| `externals/dolphin` | Offline oracle only. | Never linked in the shipping binary; A/B verification (`SUNBRIGHT_DISABLE_RECOMP`, DIFF). |

## Status (2026-06-14)
- `port/` engine: ~243 TUs compile; PAL heap + cooperative single-CPU scheduler + teardown safe;
  asset pipeline (endian-safe Yaz0/RARC/BTI); JKRDecomp worker runs natively. See
  [[port-runtime-bringup]].
- Bridge: `SUNBRIGHT_BRIDGE` marshalling thunk built + tested; link-coexistence solved
  (function-sections + `--gc-sections`). See [[native-recomp-bridge]].
- `runtime/` renderer (transitional): N1–N4 done — renders real Delfino geometry offscreen,
  Dolphin-free. See [[native-playable-path]] (note: its "renderer track is the fast path to
  playable" framing is now subordinate to THIS doc — the renderer ultimately moves into `port/`).

## Next
1. ~~The tailored-recomp de-risk slice~~ — DONE, GREEN (see DE-RISK RESULT above). The emission
   mechanism (`eng_fields` host field access) + the call bridge compose cleanly and match the oracle.
2. **De-risk #2: seeded type recovery** — core mechanism PROVEN (`type_recovery.cpp`: signature
   seeds, spill/reload, nested-handle chains; oracle-verified). REMAINING: auto-build the
   layout/signature DB from the decomp headers + `sms_gmse01_funcs.txt`, add merge/branch handling,
   and prove COMPLETE coverage on a REAL decomp function touching a REAL `port/` engine object
   (a miss = a correctness bug, so coverage is the bar). Stay oracle-verified.
3. Then build out the rest of the tailored boundary + grow the `port/` engine to cover what the game
   touches, slice by slice, oracle-verified.

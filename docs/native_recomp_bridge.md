# The native-engine ↔ recomp-game-logic bridge

Status: **DESIGN** (2026-06-14). Grounded in the actual SMS class structure and the existing
runtime override/dispatch machinery — not speculative. This is the gating milestone for a
native boot ([[port-runtime-bringup]]): the `port/` source-ported engine (host objects, host
pointers, host endianness) has to interoperate with the recompiler's game logic (guest memory:
a 24 MB linear array, big-endian, 32-bit guest addresses) for as long as game logic stays
recompiled (the scope seam in [[sms-pc-port-direction]]).

## 1. The two worlds, and the one hard constraint

| | Native engine (`port/`) | Recomp game logic |
|---|---|---|
| Memory | host heap, host pointers (64-bit) | guest RAM: `g_ram_base + (ea & 0x01FFFFFF)` |
| Endianness | host (little) | big-endian (`sb_r*/sb_w*` byteswap on access) |
| Pointer width | 64-bit | 32-bit guest addr |
| Calls | normal C++ ABI | guest PPC EABI via `CPUState` (gpr/fpr) |
| Dispatch | direct | `call_ppc(cpu, addr)` → `recomp_lookup(addr)` |

**The hard constraint (this drives everything):** recompiled game logic reads engine-object
fields by *hardcoded guest offset, in big-endian, as 32-bit*. A host-native engine object has a
different field layout (8-byte pointers shift offsets), different endianness, and different
pointer width. **Therefore a host-native object and recompiled code that field-accesses it are
fundamentally incompatible** — you cannot hand a `port/` host-native `J3DModel` to recompiled
`TMario` and have its `lwz r3, 0x84(model)` read the right thing. No thunk fixes raw field
access; only re-porting the *accessing* code does.

This is not a defeat — it precisely partitions what can go native *now* vs *last*.

## 2. The decisive finding: classify engine objects by how game logic touches them

Evidence from the real hierarchy: a SMS actor is `TMario : TTakeActor : TLiveActor : THitActor :
JDrama::TActor` — a **mixed object**. Game-side derived classes (Strategic/, recomp) sit on an
engine base sub-object (JDrama, engine). One shared vtable mixes engine virtuals (TViewObj::
perform/draw) and game-overridden virtuals (TLiveActor::control/moveObject/drawObject). The
actor holds *pointers* to heavier engine objects (`MActor* mMActor @0x74`, `J3DModel* getModel`)
and embeds engine *value types* inline (`JGeometry::TVec3 mVelocity @0xAC`,
`const TBGCheckData* mGroundPlane @0xC4`). So coupling is deep but it splits cleanly:

- **Service subsystems** — game logic holds an opaque handle/pointer and only ever *calls
  functions*; it never dereferences the object's internal fields. Examples: JKRDecomp
  (decompress a buffer), JKRArchive/JKRDvd (mount, fetch resource → returns a guest buffer),
  JAS/JAI audio (already native), CARD, DSP. **These can be `port/` host-native NOW**: the
  object lives host-side, game logic holds only a handle.
- **Object-graph subsystems** — game logic field-accesses guest-resident objects, embeds their
  value types, and inherits from them. Examples: J3D model/anim, JDrama actor/graph, J2D,
  JGeometry math, M3DUtil. **These cannot be host-native while their consumers are recomp.**
  They stay guest-layout (recompiled, or runtime-override native code that operates *on guest
  memory* via byteswap accessors — the existing `runtime/overrides/` style) and flip to
  host-native only *together with* the game logic that uses them.

## 3. The bridge mechanism (extends what already exists)

The recomp→native function-call path is **already built**: `SUNBRIGHT_OVERRIDE(name, guest_addr)`
registers a `void(CPUState&)` native fn; `recomp_lookup` routes `bl`/`bctr`/`blr`/JIT-entry to
it; `recomp_raw(addr)` is the super-call. Native overrides today read args from `cpu.gpr[3..]`,
translate guest pointers with `g_ram_base + (ea & 0x01FFFFFF)`, and return via
`call_ppc(cpu, cpu.lr)`. The bridge is the **marshalling layer** that lets a normal-C++
`port/` function be dropped into that seam without hand-writing CPUState glue each time.

### 3a. recomp → native: the marshalling thunk
A per-signature thunk adapts the guest PPC EABI (`CPUState`) to a host C++ call:
1. read integer args from `gpr[3], gpr[4], …`, float args from `fpr[1], …` (PPC EABI order);
2. translate any pointer arg from guest addr → host pointer (`sb_guest_to_host(ea)`);
3. call the `port/` C++ function;
4. write the return value to `gpr[3]` (or `fpr[1]` for float);
5. `call_ppc(cpu, cpu.lr)` to return to the caller.

This is mechanical and should be **generated from the function signature** (a C++ variadic
template `bridge_thunk<&JKRDecomp::orderSync>()` that maps each parameter type → its EABI slot,
applying pointer translation for pointer types). Write it once; every service function reuses it.
Worked example:
```
// port fn:  bool JKRDecomp::orderSync(u8* src, u8* dst, u32 srcLen, u32 dstLen)
// thunk:    src=host(gpr3) dst=host(gpr4) srcLen=gpr5 dstLen=gpr6
//           gpr3 = (u32)orderSync(...); return via lr
SUNBRIGHT_BRIDGE(0x<orderSync_guest_addr>, &JKRDecomp::orderSync);
```

### 3b. native → recomp: reverse dispatch
When a native subsystem must call back into guest code (a guest callback pointer, or a virtual
whose slot points at a recomp function), it marshals host args *into* a `CPUState`/guest ABI and
invokes `call_ppc(cpu, guest_addr)`. Same EABI mapping in reverse. Pointer args that refer to
host-only objects must be passed as their **guest handle**, never a raw host pointer (a 64-bit
host pointer cannot live in a 32-bit guest slot).

### 3c. object identity for service objects (handles)
A host-native service object is registered in a table and given a **guest handle** — a stable
32-bit token the game logic stores and passes back (the native side maps handle→host object).
This is exactly how `native_card`/`native_jas` already work (guest holds a handle; the real
object is host-side). Two handle styles: (a) an opaque small integer/cookie, or (b) a pinned
guest-memory shim struct whose address is the handle and whose few game-read fields are kept in
guest-endian. Prefer (a) where game logic treats the pointer as opaque.

### 3d. memory & endianness rules
- A native service that writes *results into guest memory* (e.g. JKRDecomp writing decompressed
  bytes to a guest `dst`) writes **raw bytes** with no swap; if it writes *typed* multi-byte
  fields that game logic will read, it must write them **big-endian** (`sb_w*`).
- On-disk asset data is big-endian; native parsers must byteswap on load (see the Yaz0 fix and
  the RARC/`JKRMemArchive` struct-overlay hazard — a `be<T>` shadow-header wrapper is the
  scalable fix). This is orthogonal to the bridge but bites the same subsystems.

### 3e. vtable bridging (object-graph case only)
A mixed object's vtable is in guest memory with 32-bit guest function pointers; some slots point
at recomp functions, some (once an engine base method is overridden native) at native thunks.
`recomp_lookup` already resolves a guest addr to native-or-recomp, so a guest vtable slot can
hold the *guest addr of a native thunk* and dispatch works in both directions. **But** the
object's data still has to be guest-layout for the recomp slots to read it — so vtable bridging
does not let object-graph objects go host-native early; it only matters when we begin flipping
individual engine base methods to native while the object stays guest-resident.

## 4. Phasing

1. **Build the marshalling thunk layer** (`SUNBRIGHT_BRIDGE` template) + a guest↔host pointer
   helper exposed to `port/`. ✅ DONE (e9514f0): `runtime/bridge.h` + `runtime/tests/bridge_test.cpp`
   (4/4, EABI bank-separation verified). **NEXT GATE — link coexistence:** routing a REAL guest
   call into a `port/` function needs `port/` and the sunbright binary to coexist in ONE link.
   They have conflicting symbols (`port/` host `JKRHeap`/`operator new`/OS scheduler vs the
   recomp world's guest `JKRHeap` + `runtime/native_threads` + libc++ `operator new`). Pulling a
   `port/` object drags its whole engine-runtime neighborhood. **COEXISTENCE MODEL — RESOLVED
   (verified link experiment):** plain object-granularity linking of `JKRDecomp.o` drags 113
   undefined refs (ARAM/OS/heap). But compiling `port/` with `-ffunction-sections -fdata-sections`
   and linking the binary with `-Wl,--gc-sections` makes referencing only `decodeSZS` link CLEAN
   (0 undefined) — the unreferenced siblings (`orderSync` …) and their deps are GC'd. So a pure
   leaf costs nothing extra and there is NO symbol conflict for leaves. Functions that genuinely
   need the `port/` heap/threads will pull them in, and THAT is where the `port/`-runtime vs
   recomp-runtime symbol boundary appears (handle then: leaf-first, and partition the runtimes
   when a non-leaf is needed). **Next concrete step:** wire `port/src/JKRDecomp.cpp` (function
   sections) into the sunbright binary, `SUNBRIGHT_BRIDGE(<decodeSZS guest addr>, &JKRDecomp::
   decodeSZS)`, build with `--gc-sections`, A/B-verify vs `recomp_raw`.
2. **Flip service subsystems to `port/` native** via the bridge, one at a time, each A/B-verified:
   decomp → archive/file I/O → (audio is already native, re-home it on `port/` later). High
   value (these are the slow/IO/asset paths), low coupling (no shared field access).
3. **Object-graph subsystems stay recomp/guest-layout** until their game-logic consumers are
   ported. When the project decides to port a game-logic cluster (port-on-problem at scale),
   flip its engine object-graph dependencies host-native *in the same move*.
4. **Endgame** ([[native-port-plan]]): when game logic is fully ported, guest memory and the
   recomp disappear; the thunks and handles vanish; `port/` host-native objects become the real
   objects. The bridge is transitional scaffolding, designed to be removed.

## 5. What this means for `port/`
- `port/` host-native objects are correct for **service** subsystems and for the **endgame**.
- For **object-graph** subsystems, `port/` host-native versions are built and unit-tested
  standalone (as now) but are NOT wired into the running hybrid until their consumers port.
- The bridge does **not** require re-porting game logic to get the first service wins — that is
  why it is the right next step.

## 6. Open risks / to-resolve when implementing
- EABI edge cases in the thunk generator: struct-by-value args, varargs (OSReport-style),
  8-byte (long long / double) register pairing, `this` in r3 for member fns, return-by-value
  structs. Enumerate from the actual service signatures before generalizing the template.
- Which exact guest addresses to bridge — resolved from `reference/sms_gmse01_funcs.txt`.
- Re-entrancy/threading: a bridged native call may run on a guest worker thread (cooperative
  scheduler); native services must be safe under the single-CPU cooperative model (they are by
  construction — one thread at a time).
- A/B harness: every bridged function keeps `recomp_raw` available so the recomp body remains
  the oracle for that function ([[recomp-overrides]] discipline).

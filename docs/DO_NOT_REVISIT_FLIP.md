# Do not revisit host-layout game objects

USER 2026-06-15: "first eradicate the flip architecture completely, delete everything related to it, leave a note to never revisit it."

The rejected design converted GameCube engine objects into host-layout C++ objects and exposed them
back to guest game code through handles, marshalled fields, and virtual-dispatch bridges.

The root defect is structural. Sunshine's gameplay code assumes 32-bit big-endian pointers, retail
field offsets, four-byte pointer-array strides, guest vtables, and direct access throughout a very
large consumer closure. Replacing one object with an LP64 host object makes every direct and inlined
consumer wrong unless the entire closure is translated into a second object model. A handle/getter
layer cannot close that boundary reliably and would duplicate game behavior.

The live native/dynarec architecture keeps game objects in Dolphin-owned guest memory. Dolphin's JIT
executes ordinary guest accesses. Native overrides read or write that canonical layout through
`gcnport` memory interfaces and may copy renderer-neutral values into `native-render/`; they never
publish host object pointers, host vtables, or host-layout arrays back to guest code. The native
decomp may exercise an equivalent value contract independently, but native and guest objects are
never shared.

Do not add host-object handles, generated field getters, guest-to-host object mirroring, virtual-call
marshalling, or a second host-layout engine. If a subsystem needs native ownership, intercept a
verified function/service boundary, reproduce its ABI and observable semantics, and keep object
identity on the guest side.

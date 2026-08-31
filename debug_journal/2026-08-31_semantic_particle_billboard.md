# First semantic JPA billboard slice (2026-08-31)

The first particle semantic boundary is the native decomp and recompiled
`JPADrawExecBillBoard::exec` call. A standard type-2 JPA billboard is a flat textured particle
quad. Both runtime adapters copy its position, scale, clipboard pivot/extents, four UVs, selected
texture, and raster policy into the shared renderer. `native-render/src/particle_billboard.cpp`
owns the six-vertex quad construction, so the two adapters use one geometry contract and no guest
object or vtable crosses the renderer boundary.

The decomp adapter accepts the reached direct-texture colour program (`ZERO, TEXC, ONE, ZERO`).
The recompiled adapter also accepts the reached modulated program (`ZERO, C0, TEXC, ZERO`),
applies JPA's integer `U8_THRE` colour multiplication, and maps the observed destination-alpha
blend (`DST_ALPHA`, `ONE_MINUS_DST_ALPHA`) to the shared `DestinationAlpha` raster policy. The
recompiled adapter resolves the JPA clipboard through the guest's live `r13 - 0x5AD8` small-data
address. An earlier hardcoded decomp symbol address read the runtime poison value `0xFADEBABE`
and rejected every call; the live small-data calculation is the root-cause fix.

The original JPA bodies remain below both hooks. Unsupported particle shapes/programs and missing
scene state are counted and fall through to the retained Aurora/GX body, which remains the
independent content oracle while semantic coverage expands. JPA uses `0xFFFF` in the per-particle
texture field for its built-in default texture rather than for a resource-table entry. The semantic
adapter now reads the `JPADefaultTexture` 8x8 IA8 bytes from the resource object, decodes them
through the shared image decoder, and submits that ordinary image value. Non-billboard particle
programs and effects are still not implemented.

Evidence from the bounded recompiled stage-one semantic audit after the fix:

```
run_rc=0
native JPA billboards: submitted=308 rejected=0 invisible=0 unsupported-shape=0 (type=0 program=0) invalid-texture=0 default-textures=66 texture-failures=0 no-scene=11 view-failures=0
semantic summary: submitted=30 completed=30 nonempty=29 mixed=20 operations=4737 pictures=40 glyphs=260 images=2424 models=4428 meshes=1968 mesh-vertices=2487228 first-nonclear-frame=1 first-nonclear-pixels=286720
```

The Clang build completed successfully. The GPU watcher reported no kernel GPU fault, reset, or
device loss through the final post-exit scan. The earlier 66 failures were all the default-texture
sentinel and are now submitted successfully; the remaining 11 calls without an active perspective
scene are explicit fallback cases, not evidence of a GPU failure. The short
decomp title audit still does not exercise a particle semantic submission; its content-free route
is therefore not a particle coverage claim.

The validation layer also reported one `VkShaderModule` still alive at `vkDestroyDevice` during
normal teardown. That is an object-lifetime cleanup warning, distinct from a GPU reset or device
loss; the process still exited 0 and the GPU watcher found no kernel fault.

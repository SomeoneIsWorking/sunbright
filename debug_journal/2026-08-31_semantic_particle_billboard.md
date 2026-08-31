# First semantic JPA billboard slice (2026-08-31)

The first particle semantic boundary is the native decomp `JPADrawExecBillBoard::exec` call. It
publishes only the exact reached family whose `JPABaseShape` is type 2 (standard billboard) and
whose colour program is direct texture output (`ZERO, TEXC, ONE, ZERO`). The adapter copies the
particle's global position, scale, clipboard pivot/extents, four UVs, texture selection, decoded
`JUTTexture`, and the shape's ordinary depth, alpha-test, and blend policy into the shared
renderer. `native-render/src/particle_billboard.cpp` owns the six-vertex quad construction so a
future recomp adapter can use the same geometry contract.

The original JPA body remains below the hook; Aurora therefore remains an independent content
oracle while semantic coverage is expanded. Unsupported programs, missing textures, and unsupported
raster policies increment a rejection counter and fall through to the retained renderer. No
decomp object or JPA layout crosses the shared renderer boundary.

Evidence: the shared `native_render_particle_billboard_test` checks the pivoted quad, UV ordering,
and vertex colour. The `sms-boot` and `sms-recomp` targets compile with Clang. A short decomp title
semantic audit initialized the AMD Radeon Vulkan device and exited with the existing non-clear
content gate because that route reached only solid rectangles (`models=0`, `billboards=0`); the
watcher classified the SIGABRT as a CPU/process signal, with no kernel GPU fault or reset. This run
does not claim live particle reach.

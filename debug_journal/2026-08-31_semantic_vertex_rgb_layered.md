# Per-vertex-RGB layered J3D materials (2026-08-31)

The next semantic material family was not a new shader equation. It was the existing two-layer,
signed-diffuse/specular family with a different J3D colour-channel source: program `0x068E` reads
the material/register colour, while `0x068F` sets the `matSrc` bit and reads authored per-vertex
RGB instead. The decomp `J3DColorChan` definition identifies that bit as the material-versus-
vertex source selector.

The shared classifier now accepts both program values and carries the source choice as renderer-
neutral `usesVertexRgb`. The model transform selects the vertex colour only for `0x068F`; the
existing `0x068E` path remains unchanged. The regression test proves both classification and a
different transformed colour for a non-white vertex.

Evidence from the bounded recompiled stage-one semantic audit:

```
run_rc=0
top-program[0]=300 ... channels=2:068f/0700,0212/0400 stages=2 ... path=0/300/300/0
J3D native-model coverage: ... material rejections: ... lighting=2660 ...
```

The 300 calls were accepted and had their resources ready, but this title-stage route had no
perspective scene for that exact top program, so it produced no live model submissions from this
family. The audit still reached 4,428 semantic models overall. The GPU watcher found no kernel
GPU fault, reset, device loss, or illegal command stream. Vulkan teardown still reports one live
shader module at device destruction; that is a separate lifetime-cleanup defect.

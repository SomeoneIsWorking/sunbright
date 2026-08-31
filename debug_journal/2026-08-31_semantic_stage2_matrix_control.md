# Stage-two semantic audit and matrix-control diagnostic (2026-08-31)

The stage-two semantic audit first stopped with `SIGABRT` before the first GPU submit. This was
not a GPU failure. `sbr_mtx_end_shape()` always compared its high-level matrix-object list with
the indexed-GX-load list even though `SBR_MTX_CHECK` is documented as an opt-in diagnostic.
Stage two uses the CPU skinning pipeline: `J3DShapeMtxMulti::load()` reaches
`GXLoadPosMtxImm`, so nine high-level bindings and zero indexed loads are a valid pair for that
pipeline. The unconditional assertion treated that expected case as corruption.

The fix gates the comparison on `sbr_mtx_check_enabled()` and still resets the shape scope when
the check is disabled. Enabling `SBR_MTX_CHECK` retains the explicit comparison and its fail-fast
message; the semantic matrix capture is not disabled.

The bounded rerun reached the renderer and exited normally:

```
run_rc=0
semantic frames: 20 completed, 20 non-empty
J3D models: 1,464 submitted, 1,211,328 vertices
GPU watcher: no kernel GPU fault, reset, device loss, or illegal command stream
```

The run also grounded the next material gap: 300 untextured, two-stage lit models using colour
channels `0x0686/0x0700` and `0x0212/0x0400`, with TEV programs `c021faeac108ffd0` and
`c208420ac300ff80`. Of those, 192 reached a perspective scene and none are accepted by the
semantic classifier yet. This is a renderer-coverage gap, not GPU evidence; the exact equation
must be decoded before adding a classifier.

Aurora still reports one `VkShaderModule` alive at `vkDestroyDevice`; that is a separate host
resource-lifetime warning and did not affect this run's successful exit.

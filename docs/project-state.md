# Project state

Factual capability coverage for Sunbright. Durable intent lives in `docs/project-goals.md`, atomic
work in `docs/issues/`, and subsystem placement in `docs/codemap.md`.

| ID | Capability / observable outcome | State | Dependencies | Goals |
|---|---|---|---|---|
| S001 | The recomp runtime boots and renders the game through Aurora GX | verified | — | — |
| S002 | The native decomp runtime boots and renders representative game flow through Aurora GX | verified | — | G002 |
| S003 | The recomp has a project-owned SDL3-GPU GX compatibility/reference renderer | partial | S001 | — |
| S004 | The recomp renders through a PC-native game-semantic renderer above GX | missing | S001 | G003 |
| S005 | The decomp renders through the shared PC-native game-semantic renderer above GX | missing | S002, S004 | G004 |
| S006 | Interpolated presentation covers every rendered target that should move between ticks | partial | S001 | G001 |
| S007 | The native decomp is upstream-converged, semantically named, and complete for reached game behavior | partial | S002 | G002 |

## Current focus

S004 is the current focus. The path previously counted toward it is now classified separately as
compatibility tooling: a native GPU backend reproducing GX is not a PC-native renderer. Issue 24
owns the semantic boundary.

## Capability details

### S001 — Recomp through Aurora GX

Evidence: the recomp path renders title, file select, and Delfino Plaza, and the documented stage
sweep reached and rendered all 24 selectable stages. `run.sh` is the guarded default launcher and
the Clang runtime test suite covers its hardware/OS seams.

### S002 — Decomp through Aurora GX

Evidence: the native `decomp/sms` plus Aurora runtime renders the title, file-select, and Delfino
flow and remains the readable game-behavior oracle. Its boot/runtime ownership is documented in
`AGENTS.md` and `docs/codemap.md`.

### S003 — SDL3-GPU GX compatibility renderer

The explicit `run-render.sh` path owns an SDL3-GPU device and presentation while consuming parsed
GX/FIFO state. It reconstructs geometry and reproduces GX raster, TEV, texture, and EFB-copy
semantics. C075 records an exact-frame Aurora comparison and its working identity control.

Gap: visual compatibility is incomplete, but completing it still would not satisfy a native-renderer
goal because the shipping abstraction remains GameCube GX. This path is retained as reference and
diagnostic infrastructure.

### S004 — Recomp PC-native semantic renderer

Missing capability: no shipping renderer consumes J3D/J2D/particle/material/light/effect semantics
above GX. No renderer-neutral semantic scene interface currently bypasses FIFO, BP/XF registers,
TEV programs, and EFB-copy choreography. Issue 24 defines the required seam, constraints, and first
vertical slice at `J2DPicture::drawSelf(int, int, Mtx*)`.

### S005 — Decomp PC-native semantic renderer

Missing capability: the decomp has no adapter from native game objects into the shared semantic
scene interface and no Aurora-free PC-native presentation lane. It depends on the renderer-neutral
schema and renderer ownership established by S004, but must keep a separate native-layout adapter.

### S006 — Lerp coverage

Schema-5 comparison now binds guest time, camera matrices, assets, binaries, frame hashes, texture
descriptors, and GPU-clean completion. C076 shows the visible water region improves slightly during
a controlled camera rotation, while issue 15 localizes the strongest residual to a palm/sky cell.

Gap: the residual cell is not joined to a stable draw identity, and the graphics registry still has
partial populations. Screen-space localization alone cannot name the missing target.

### S007 — Decomp expansion

The native decomp is integrated as a runnable game path and carries project-specific native safety
adaptations while tracking upstream `doldecomp/sms`.

Gap: upstream convergence debt, known `unk*` names, and reachable unimplemented bodies remain. Each
expansion pass must still follow rebase → rename established unknowns → extend from binary evidence.

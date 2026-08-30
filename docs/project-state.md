# Project state

Factual capability coverage for Sunbright. Durable intent lives in `docs/project-goals.md`, atomic
work in `docs/issues/`, and subsystem placement in `docs/codemap.md`.

| ID | Capability / observable outcome | State | Dependencies | Goals |
|---|---|---|---|---|
| S001 | The recomp runtime boots and renders the game through Aurora GX | verified | — | — |
| S002 | The native decomp runtime boots and renders representative game flow through Aurora GX | verified | — | G002 |
| S003 | The recomp has a project-owned SDL3-GPU GX compatibility/reference renderer | partial | S001 | — |
| S004 | The recomp feeds real game-frame J2D pictures to the shared PC-native semantic renderer above GX | partial | S001 | G003 |
| S005 | The native decomp feeds real game-frame J2D pictures to the same shared PC-native semantic renderer above GX | partial | S002, S004 | G004 |
| S006 | Interpolated presentation covers every rendered target that should move between ticks | partial | S001 | G001 |
| S007 | The native decomp is upstream-converged, semantically named, and complete for reached game behavior | partial | S002 | G002 |

## Current focus

S004 is the current focus. The path previously counted toward it is now classified separately as
compatibility tooling: a native GPU backend reproducing GX is not a PC-native renderer. The current
atomic gap is the missing unified text/window/fill ordering required before semantic output can own
visible presentation.

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
semantics. C083 records the current exact-frame Aurora comparison and its working identity control;
C075 is the expired prior measurement.

Gap: visual compatibility is incomplete, but completing it still would not satisfy a native-renderer
goal because the shipping abstraction remains GameCube GX. This path is retained as reference and
diagnostic infrastructure.

### S004 — Recomp PC-native semantic renderer

The shared `native-render/` core now defines the first renderer-neutral picture command, semantic
`J2DPicture::drawFullSet` layout resolver, material-layer contract, and guarded submission sink. The
recomp's sole `0x802cc7c0` override reads guest `J2DPicture` state at entry through a tested
big-endian adapter and always calls the retained body. A separate SDL3 pass consumes decoded RGBA
images and these commands directly; its guarded GPU control verifies clipping, texture sampling,
alpha, repeat determinism, and a changed-content/revision result. No FIFO, BP/XF register, TEV
program, EFB operation, Aurora type, or GX compatibility type crosses this interface. C077 records
the falsifiable combined evidence.

The same core now owns renderer-neutral decoding of all eleven tiled game-asset image encodings and
all three palette encodings. Its CPU controls cover tile order and edge rounding, exact source and
palette ranges, palette-index refusal, bit-replicated colour expansion, GX CMPR's 3/8 interpolation
and transparent midpoint RGB, mip-chain sizing, and content revision changes. The legacy recomp GX
compatibility texture adapter consumes this same decoder but retains guest addresses and GX names on
its side of the boundary. C078 records this narrower asset-input result; it is not counted as a
rendering pass.

Both runtime adapters now resolve `JUTTexture::mTexData` and the active `JUTPalette` at
`J2DPicture::drawSelf` entry, decode the exact byte ranges immediately, derive nonzero content
revisions, and submit the command plus all referenced images as one synchronous operation. The
recomp adapter control covers direct RGBA8 and C4+IA8 guest layouts, changed-content revisions, and
short palette refusal. The decomp production-linked control uses real J2DPicture/JUTTexture/
JUTPalette objects, verifies C4+IA8 and I8 output, stable and changed revisions, span copying during
the callback, and the required host-allocation gate. C079 records this temporal/lifetime contract.

The shared core also owns a bounded, SDL-free semantic frame collector and fixed-depth J2D context
stack. Each picture carries its own logical ortho rectangle and physical viewport because one game
frame can draw multiple J2DScreens with different graphs. The collector copies transient decoded
pixels, preserves draw order, coalesces only byte-identical resource/revision pairs, rejects a key
whose bytes disagree, and refuses command/image/byte overflow or invalid lifecycle transitions.
The sealed frame contains only target extent, clear colour, ordered picture draws, and owned RGBA
views; it still contains no runtime or GX representation. C080 records these controls.

Both recomp and decomp now scope the semantic producer around the retained `J2DScreen::draw` body.
They copy the supplied ortho graph's logical rectangle and viewport (or the exact null-context
640x480 default), carry `mbClipToParent`, and attach the leaf pane's final hierarchy `mClipRect`.
The GPU control includes a nonzero sub-viewport known-different answer.

The shared core also owns the one process-level semantic frame bridge, one SDL GPU platform, and the
offscreen semantic-frame client. The bridge claims the guarded picture sink by lease and brackets
the bounded collector at the exact runtime frame boundaries. `SdlSemanticFrameClient` consumes each
sealed sequence once, owns a 640x480 no-depth target and `PicturePass`, submits with a fence, and
reads back until it observes pixels different from its controlled black clear. Device-only platform
initialization creates no SDL window claim; the optional presenter remains the sole window owner for
GX compatibility. `PicturePass` commits or rolls back its image-cache transaction only after the
caller reports submission.

With `SB_SEMANTIC_PICTURE_AUDIT=1`, recomp host composition now activates that client while Aurora
continues to present the visible GX frame. A guarded 100-present title run completed all 50 semantic
simulation frames, carrying 1,302 picture draws and images; frame 9 produced 14,181 pixels distinct
from clear. The production GPU control independently proves an empty semantic frame stays exactly
clear, a planted picture produces a different non-clear hash, and a duplicate sequence is refused.

Gap: this remains offscreen liveness and ownership evidence, not visual-correctness, completeness,
cross-runtime-parity, or visible-presentation evidence. Direct `J2DPicture::draw` callers bypass the
screen scope. Text, windows, fills, 3D, authored mip chains, J3D, particles, lights, and effects are
not in one semantic order stream, so a picture-only result must not be overlaid or presented as the
game frame.

### S005 — Decomp PC-native semantic renderer

The native decomp `J2DPicture::drawSelf` now publishes the same shared semantic command from native
J2D/JUT fields before running its retained GX body. Its adapter is layout-local and shares only
values and the renderer-neutral resolver with recomp; there is no recomp/decomp object interop.
Established picture fields at `0x104`, `0x114`, and `0x130` are named for blend weights and flip.
It also publishes decoded/versioned images inside an explicit host-allocation gate; a
production-linked native-layout control proves the gate is balanced and that the sink copies its
transient spans before return. The native-only `JUTTexture()` empty state is now fully initialized,
instead of leaving seventeen fields indeterminate before `storeTIMG`.

The decomp scopes each retained `J2DScreen::draw`, copies the real J2DOrthoGraph values without
retaining its stack pointer, and publishes the final logical pane clip at picture entry. Its host
composition activates the same offscreen client. Frame begin, seal, consume, and next begin remain
inside the host-allocation boundary where required, and semantic GPU teardown completes before
Aurora teardown. A guarded 400-present title run completed all 400 semantic frames; 297 carried
draws, totaling 9,207 pictures and images, and semantic frame 104 produced 158,038 pixels distinct
from clear. Aurora remains the visible GX renderer.

Gap: the same missing unified ordering and semantic families described above prevent visible decomp
semantic presentation.

### S006 — Lerp coverage

Schema-5 comparison now binds guest time, camera matrices, assets, binaries, frame hashes, texture
descriptors, and GPU-clean completion. C076 shows the visible water region improves slightly during
a controlled camera rotation, while issue 15 localizes the strongest residual to a palm/sky cell.

Gap: the residual cell is not joined to a stable draw identity, and the graphics registry still has
partial populations. Screen-space localization alone cannot name the missing target.

### S007 — Decomp expansion

The native decomp is integrated as a runnable game path and carries project-specific native safety
adaptations while tracking upstream `doldecomp/sms`. The 2026-08-30 sync incorporated seven
upstream commits, reconciled the overlapping MActor/SDLModel/MarioCap API renames as complete
header/source ownership units, passed the native Clang audit, and reached bounded title and gameplay
frames under the guarded launcher.

Gap: upstream convergence debt, known `unk*` names, and reachable unimplemented bodies remain. Each
expansion pass must still follow rebase → rename established unknowns → extend from binary evidence.

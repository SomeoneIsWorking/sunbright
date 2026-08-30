# Project state

Factual capability coverage for Sunbright. Durable intent lives in `docs/project-goals.md`, atomic
work in `docs/issues/`, and subsystem placement in `docs/codemap.md`.

| ID | Capability / observable outcome | State | Dependencies | Goals |
|---|---|---|---|---|
| S001 | The recomp runtime boots and renders the game through Aurora GX | verified | — | — |
| S002 | The native decomp runtime boots and renders representative game flow through Aurora GX | verified | — | G002 |
| S003 | The recomp has a project-owned SDL3-GPU GX compatibility/reference renderer | partial | S001 | — |
| S004 | The recomp feeds ordered game-frame J2D pictures, resource-font glyphs, generic J2D filled/gradient boxes, and GC2D solid rectangles to the shared PC-native semantic renderer above GX | partial | S001 | G003 |
| S005 | The native decomp feeds the same ordered semantic picture/glyph/filled-box stream to that renderer | partial | S002, S004 | G004 |
| S006 | Interpolated presentation covers every rendered target that should move between ticks | partial | S001 | G001 |
| S007 | The native decomp is upstream-converged, semantically named, and complete for reached game behavior | partial | S002 | G002 |

## Current focus

S004 is the current focus. The path previously counted toward it is now classified separately as
compatibility tooling: a native GPU backend reproducing GX is not a PC-native renderer. The current
atomic gap is adding J2D window contents and frame pieces to the same ordered stream before semantic
output can own visible 2D presentation.

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

The shared `native-render/` core defines renderer-neutral picture and solid-rectangle commands, the
semantic `J2DPicture::drawFullSet` layout resolver, material-layer contract, and guarded submission
sink. The
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
frame can draw multiple J2DScreens with different graphs. Pictures and solid rectangles enter one
`SemanticDraw` variant sequence, so a picture → fill → picture submission cannot be regrouped into
family passes. The collector copies transient decoded pixels, coalesces only byte-identical
resource/revision pairs, rejects a key whose bytes disagree, and refuses command/image/byte
overflow or invalid lifecycle transitions. The sealed frame contains only target extent, clear
colour, ordered semantic operations, and owned RGBA views; it still contains no runtime or GX
representation. C080 records these controls.

Both recomp and decomp now scope the semantic producer around the retained `J2DScreen::draw` body.
They copy the supplied ortho graph's logical rectangle and viewport (or the exact null-context
640x480 default), carry `mbClipToParent`, and attach the leaf pane's final hierarchy `mClipRect`.
The GPU control includes a nonzero sub-viewport known-different answer.

The shared core also owns the one process-level semantic frame bridge, one SDL GPU platform, and the
offscreen semantic-frame client. The bridge claims the guarded semantic sink by lease and brackets
the bounded collector at the exact runtime frame boundaries. `SdlSemanticFrameClient` consumes each
sealed sequence once, owns a 640x480 no-depth target and `Semantic2dPass`, submits with a fence, and
reads back until it observes pixels different from its controlled black clear. Device-only platform
initialization creates no SDL window claim; the optional presenter remains the sole window owner for
GX compatibility. `Semantic2dPass` commits or rolls back its image-cache transaction only after the
caller reports submission. It opens one render pass and walks the variant sequence in order,
switching between textured-picture and vertex-colour pipelines without a fake texture or any GX
state. Its watched GPU control proves solid/picture/solid overlap, the opposite result after
reordering, clipped-solid no-op behavior, and alpha blending.

The recomp's existing `GC2D fill_rect` override at `0x80140390` now captures the final guest
`JDrama::TRect` and packed RGBA value after any widescreen expansion, submits a semantic solid
rectangle, and then calls the retained recompiled body. Its pure big-endian adapter control covers
negative widened coordinates, canonical corner order, packed colour, short reads, and a degenerate
rectangle.

Generic `J2DGrafContext::fillBox` calls now enter that same solid-rectangle family through a
separate high-level source identity. The shared resolver reproduces the retail signed-16-bit vertex
narrowing and loaded 3x4 position transform; each adapter preserves the counterintuitive retail
corner ownership in which `mColorBR` is emitted at geometric bottom-left and `mColorBL` at
bottom-right. J2D scissor bounds remain target-pixel clips rather than being rescaled as logical
canvas coordinates. The recomp override at `0x802eba70` and the guarded decomp source call both
retain their original GX bodies. Big-endian and production-linked native-layout controls cover
field offsets, wraparound coordinates, transforms, four distinct colours, clipping, short input,
and a degenerate rectangle. The guarded semantic GPU control passed without a kernel fault, and a
dedicated runtime statistic distinguishes these boxes from existing GC2D fills. A clean
180-present Delfino run did not organically reach this routine, so it is runtime-safety evidence,
not live filled-box coverage.

Immediate `J2DPicture::draw` calls are also in the ordered stream. `J2DGrafContext::setup2D`
publishes the active orthographic canvas independently of a `J2DScreen` scope; the picture override
retains the complete guest body, then copies the position matrix that body built plus the saved
destination extent and orientation flags. The shared resolver reproduces the retail signed-16-bit
vertex narrowing and both ordinary and transposed UV associations without carrying GX state. Its
known-different control proves the transposed branch changes UV ownership. A guarded 400-present
Delfino run reached the direct entry point and completed 3,019 semantic operations (3,010 pictures
and nine solid rectangles) without a GPU fault or missing-context/capture refusal.

Resource-font text is now a distinct ordered semantic family rather than a textured-picture alias.
The shared glyph resolver owns the retail bearing, advance, vertical metrics, fixed-point atlas UV,
corner-colour, black/white remap, clip, and transformed-quad contract. The recomp wraps the retained
`J2DTextBox::draw` and matrix-taking `drawSelf` bodies to publish their high-level text transform,
tracks both retained `JUTResFont::setGX` forms, then calls the retained `drawChar_scale` body before
copying the selected big-endian glyph page and width entry through the renderer-neutral adapter.
Malformed block bounds, unsupported indexed font pages, short guest reads, and missing semantic
context fail at the adapter boundary. Close controls cover fixed versus proportional advance,
bearing application, the retail 15-bit atlas-coordinate rounding, corner order, content revision,
and a changed glyph page. A guarded 180-present Delfino run completed 90 semantic frames with 1,209
ordered operations: 160 pictures, 1,040 actual resource-font glyphs, and nine solid rectangles.

The active-canvas classifier requires both the retail `J2DOrthoGraph` vtable and its type word.
Retail base `J2DGrafContext` constructors leave that word uninitialized, so trusting the type alone
can reinterpret a base object as the larger derived layout. A known-different control plants the
stale type under the base vtable and requires refusal; native decomp initializes the base
discriminator to zero without changing non-native decomp behavior.

With `SB_SEMANTIC_FRAME_AUDIT=1`, recomp host composition now activates that client while Aurora
continues to present the visible GX frame. A guarded 100-present title run completed all 50 semantic
simulation frames. All 50 were nonempty; six contained both operation families. The stream carried
1,316 operations: 1,302 pictures, 14 solid rectangles, and 1,302 images. Its first sampled frame
already contained 286,720 pixels distinct from clear. The production GPU control independently
proves an empty semantic frame stays exactly clear, a planted mixed frame produces a different
non-clear hash, and a duplicate sequence is refused.

Gap: this remains offscreen liveness and ownership evidence, not visual-correctness, completeness,
cross-runtime-parity, or visible-presentation evidence. J2D windows, 3D,
authored mip chains, J3D, particles, lights, and effects are not in the semantic stream, so the
partial result must not be overlaid or presented as the game frame.

### S005 — Decomp PC-native semantic renderer

The native decomp `J2DPicture::drawSelf` now publishes the same shared semantic command from native
J2D/JUT fields before running its retained GX body. Its adapter is layout-local and shares only
values and the renderer-neutral resolver with recomp; there is no recomp/decomp object interop.
Established picture fields at `0x104`, `0x114`, and `0x130` are named for blend weights and flip.
It also publishes decoded/versioned images inside an explicit host-allocation gate; a
production-linked native-layout control proves the gate is balanced and that the sink copies its
transient spans before return. The native-only `JUTTexture()` empty state is now fully initialized,
instead of leaving seventeen fields indeterminate before `storeTIMG`.

The decomp's source-level `GC2D::fill_rect` now publishes its native `JDrama::TRect` and packed
colour through a layout-local adapter before retaining the original GX body. It shares the same
semantic solid-rectangle value type and ordered sink as recomp, never a game object or layout.

The decomp's source-level `J2DGrafContext::fillBox` likewise publishes its native rectangle,
position matrix, four colours, active canvas, and J2D scissor through a layout-local adapter before
the original GX body. Its production-linked control drives real J2D objects through the shared
signed-16-bit layout resolver; no guest layout crosses into this adapter.

The decomp scopes each retained `J2DScreen::draw`, copies the real J2DOrthoGraph values without
retaining its stack pointer, and publishes the final logical pane clip at picture entry. Its host
composition activates the same offscreen client. Frame begin, seal, consume, and next begin remain
inside the host-allocation boundary where required, and semantic GPU teardown completes before
Aurora teardown. A guarded 400-present title run completed all 400 semantic frames; 350 were
nonempty and eleven contained both operation families. The stream carried 9,271 operations: 9,207
pictures, 64 solid rectangles, and 9,207 images. Semantic frame 104 was the first sampled nonclear
frame and contained 149,927 pixels distinct from clear. Aurora remains the visible GX renderer.

The native decomp `J2DGrafContext::setup2D` and `J2DPicture::draw` now carry the same active-canvas
and immediate-picture behavior through native source. The call publishes the already-built native
position matrix and copied scalar arguments before retaining the original GX vertex body. A
production-linked native-layout control exercises the active context, direct positions, mirrored
UVs, transient image copy, and balanced host-allocation gate.

Native `J2DTextBox` bodies now publish their exact transform and clip around the retained
`J2DPrint` traversal, while native `JUTResFont` publishes the selected glyph metrics, atlas page,
corner colours, and remap after retaining its GX body. The layout-local adapter decodes that page
inside the host-allocation gate and submits the same shared `GlyphDraw` value used by recomp. The
production-linked native-layout control drives the real bridge with encoded font-page bytes and
proves the transient image is copied before return. Bounded title and stage-one decomp runs did not
exercise a resource-font glyph on their reached paths, so they prove runtime safety but not live
decomp glyph coverage; that remains explicit rather than inferred from the adapter test.

Gap: the same missing J2D window family described above prevents visible decomp semantic
presentation.

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

# Project state

Factual capability coverage for Sunbright. Durable intent lives in `docs/project-goals.md`, atomic
work in `docs/issues/`, and subsystem placement in `docs/codemap.md`.

| ID | Capability / observable outcome | State | Dependencies | Goals |
|---|---|---|---|---|
| S001 | The recomp runtime boots and renders the game through Aurora GX | verified | — | — |
| S002 | The native decomp runtime boots and renders representative game flow through Aurora GX | verified | — | G002 |
| S003 | The recomp has a project-owned SDL3-GPU GX compatibility/reference renderer | partial | S001 | — |
| S004 | The recomp feeds ordered 2D/UI draws and rigid unlit plus first diffuse- and specular-lit single-texture J3D material families to the shared PC-native semantic renderer above GX | partial | S001 | G003 |
| S005 | The native decomp feeds the same semantic 2D/UI and J3D material families to that renderer through native-layout adapters | partial | S002, S004 | G004 |
| S006 | Interpolated presentation covers every rendered target that should move between ticks | partial | S001 | G001 |
| S007 | The native decomp is upstream-converged, semantically named, and complete for reached game behavior | partial | S002 | G002 |

## Current focus

S004 is the current focus. The path previously counted toward it is classified separately as
compatibility tooling: a native GPU backend reproducing GX is not a PC-native renderer. The shared
semantic frame now combines the ported 2D/UI stream with rigid J3D triangle meshes using ordinary
model/view/projection matrices, decoded RGBA textures, vertex colour, and a PC-native depth-tested
SDL3 pass. Both runtimes publish the same values through separate layout adapters and always retain
their original draw bodies for A/B. Camera projection now comes directly from the `TGraphics`
value written by the game's camera and is scoped around high-level `TViewObj::testPerform` draw
dispatches in both runtimes; the semantic J3D adapters no longer consult `GXSetProjection`,
`GXGetProjectionv`, FIFO, or compatibility-renderer state. The same high-level J3D material objects
now supply culling, depth test/write, alpha cutout, source-alpha blending, decoded normals,
material/vertex colour choice, ambient colour, stage point lights, and authored directional
specular lighting for the exact supported families. Broader J3D materials are next: the remaining
lit and multi-stage programs,
skinning, authored mip chains, particles, and effects still fall back to the retained renderer.

## Capability details

### S001 — Recomp through Aurora GX

Evidence: the recomp path renders title, file select, and Delfino Plaza, and the documented stage
sweep reached and rendered all 24 selectable stages. `run.sh` is the guarded default launcher and
the Clang runtime test suite covers its hardware/OS seams.

### S002 — Decomp through Aurora GX

Evidence: the native `decomp/sms` plus Aurora runtime renders the title, file-select, and Delfino
flow and remains the readable game-behavior oracle. Its boot/runtime ownership is documented in
`AGENTS.md` and `docs/codemap.md`. Issue 30 records a pre-existing stage-one retained-GX path that
can abort when an indirect draw supplies illegal texture wrap value 3; older captures and the new
core establish the symptom, but its originating texture-state write is not yet traced.

### S003 — SDL3-GPU GX compatibility renderer

The explicit `run-render.sh` path owns an SDL3-GPU device and presentation while consuming parsed
GX/FIFO state. It reconstructs geometry and reproduces GX raster, TEV, texture, and EFB-copy
semantics. C083 records the current exact-frame Aurora comparison and its working identity control;
C075 is the expired prior measurement.

Gap: visual compatibility is incomplete, but completing it still would not satisfy a native-renderer
goal because the shipping abstraction remains GameCube GX. This path is retained as reference and
diagnostic infrastructure.

### S004 — Recomp PC-native semantic renderer

The first 3D slice now intercepts the high-level `J3DShape::draw` boundary, not a GX/FIFO boundary.
One shared decoder owns J3D vertex-layout normalization, display-list primitive decoding, asset
image decoding, and the exact first material family: rigid, unlit, one texture, one texture
coordinate, authored vertex colour, and the observed texture-times-raster-colour stage program.
The recomp adapter reads retail big-endian objects and arrays; the SDL pass receives only triangle
vertices, matrices, an ordinary RGBA image, and sampler policy. Its projection matrix is currently
copied from `JDrama::TGraphics + 0x74` at the game's high-level draw-dispatch funnel. The scope
classifies perspective and orthographic camera matrices and explicitly suppresses uninitialized
pre-camera traversals, so nested/non-3D passes cannot inherit stale projection state. The adapter
has no dependency on the runtime's `GXSetProjection` mirror. A planted shader control proves the
same triangle changes from red to green when the decoded texture is bound. The final combined-tree
guarded 60-present camera-scope audit submitted 6,468 model draws and 2,512,884 decoded vertices
with zero unreadable, layout, rigid-matrix, or mesh-decode failures and no GPU fault. It observed
11,331 perspective, 728 orthographic, and 46 pre-camera dispatches; 44 model attempts outside a
perspective scope correctly fell back. Original recompiled bodies remain callable and execute
after every semantic submission. C093 records the falsifiable camera-boundary evidence.

The renderer-neutral material value now also carries ordinary cull, depth compare/write, alpha
cutout, and blend policy. The guest adapter reads those values from the retail J3D colour and pixel
engine blocks; the shared classifier accepts the three common high-level policy families and full
blocks only when every relevant field exactly expands to one of those families. The SDL pass keys
its pipelines by that policy, rejects cull-all draws before upload, and uses PC shaders for the
cutout threshold. A guarded GPU control distinguishes front/back/all culling, alpha 127 from 128,
replace from source-alpha blending, and depth-write on from off. The post-change guarded recomp run
exited cleanly after 60 presents with 6,006 cutout/back-cull models and 1,092,366 vertices; it
separately rejected 682 unsupported textured raster policies rather than approximating them and
reported no GPU fault. Full blocks with a present but `GX_FOG_NONE` fog object remain eligible;
actual fog modes fall back.

The first lit 3D family uses one decoded texture multiplied by per-vertex diffuse lighting. The
shared model contract carries decoded normals, an ordinary ambient colour, and up to two view-space
point lights with authored distance falloff. The recomp override runs the real
`TLightCommon::setLight` body, then reads the same high-level world positions, colours, ambient, and
camera matrix through the game's getters; it does not read a GX light object, XF register, FIFO, or
compatibility-renderer mirror. The classifier admits only the observed one-stage texture-times-
raster program with the exact material-colour or vertex-colour channel policy. Full pixel-policy
blocks are normalized by their observable alpha predicate, so AND, OR, and XNOR over two
always-true comparisons all become the same ordinary pass-all policy while XOR is refused. This
correction raised guarded Delfino submissions from 60 to 100 lit models in the recomp runtime and
from 36 to 630 in the native-decomp runtime; the runs submitted 3,260 and 41,997 total models,
respectively, exited cleanly, and reported no GPU fault.

The classifier also recognizes one observed two-stage Mario hand family as a high-level operation:
mix white and diffuse-lit vertex RGB equally, then multiply one decoded texture while preserving
material alpha and source-alpha blending. CPU controls cover that equation, its independent RGB/
alpha sources, the exact half-weight selector, and the XOR raster rejection. The live census found
the family 200 times, but all occurred outside an active perspective scene and therefore submitted
zero visible models; it is classified and production-linked, not yet claimed as rendered coverage.
The same census ranks remaining lit programs by observations inside a perspective scene and reports
each material's progress through classification, texture decode, scene readiness, and submission,
so setup-only states cannot masquerade as renderer progress. C095 owns the cross-runtime lighting
evidence and falsifier. Other lit programs fall back to the retained renderer by named rejection.

The first perspective-reached two-channel specular family is now native too: Mario's
`_mat_hand3_L` material combines the decoded hand-mask texture, diffuse stage lighting, an authored
fixed tint, and the primary light's directional specular highlight. `TLightCommon` publishes its
authored shininess before either runtime builds a console light object, and both material adapters
publish the second high-level colour channel and tint. The shared PC shader evaluates the ordinary
affine form `texture * lit-colour + tint-and-highlight`; no GX light, fixed-function stage, or
compatibility-renderer value crosses the renderer boundary. Exact positive and deliberately altered
channel, tint-selector, stage, normal, and shininess controls passed. In the guarded 60-present
The watched shipping-shader control also distinguished the texture-times-diffuse baseline from the
red-tinted affine result while preserving green, including the exact sRGB sampling/output
conversion, with no kernel GPU fault. In the guarded 60-present recomp run, the family progressed
through all 60 perspective observations to 60 decoded-resource
submissions, raising lit models from 100 to 160 and total models from 3,260 to 3,320. The guarded
400-present native-decomp run raised lit models from 630 to 666 among 42,033 total models. Both runs
exited cleanly under the live GPU watcher.

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
semantic-frame client. The bridge claims the guarded semantic sink by lease and brackets the bounded
collector at the exact runtime frame boundaries. `SdlSemanticFrameClient` consumes each sealed
sequence once, owns a 640x480 no-depth target and `Semantic2dPass`, submits with a fence, and reads
back until it observes pixels different from its controlled black clear. In audit mode it initializes
the platform without claiming a window. In explicit preview mode it attaches the platform's sole
presenter, while runtime composition disables Aurora presentation but keeps Aurora rendering
offscreen as the retained reference. `Semantic2dPass` commits or rolls back its image-cache
transaction only after the caller reports submission. It opens one render pass and walks the variant
sequence in order, switching between textured-picture and vertex-colour pipelines without a fake
texture or any GX state. Its watched GPU control proves solid/picture/solid overlap, the opposite
result after reordering, clipped-solid no-op behavior, and alpha blending.

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

`J2DWindow::draw_private` now enters the same ordered stream as a semantic composition rather than
as GX primitives: one four-corner-colour contents rectangle, an optional centered contents texture,
four corners, then four edge strips sampled from their owning corner textures. The shared resolver
owns the matching decomp body's signed-16-bit position narrowing, stored minimum-size gate, matrix
concatenation, asymmetric corner sizes, centered contents UVs, and all eight mirror bits. It emits
only the existing renderer-neutral rectangle and one-texture picture commands; no GX window state
or window-specific shader was added. The recomp adapter reads retail big-endian fields at the
existing widescreen override after its rectangle/matrix edits and before the retained super-call.
Its close control parses the actual object offsets, decodes five textures, proves the nine textured
parts and gradient order, and falsifies malformed sampler input. The original recompiled body stays
available on every call.

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

With `SB_SEMANTIC_FRAME_MODE=audit`, recomp host composition activates that client while Aurora
continues to present the visible GX frame. A guarded 100-present title run completed all 50 semantic
simulation frames. All 50 were nonempty; six contained both operation families. The stream carried
1,316 operations: 1,302 pictures, 14 solid rectangles, and 1,302 images. Its first sampled frame
already contained 286,720 pixels distinct from clear. The production GPU control independently
proves an empty semantic frame stays exactly clear, a planted mixed frame produces a different
non-clear hash, and a duplicate sequence is refused.

`./run.sh --semantic-preview` exposes that same target in the live application window without
letting Aurora present over it. A guarded 130-present recomp run completed and presented all 65
semantic simulation frames, observed no unavailable-window frame, and sampled 286,720 pixels
different from clear. The launcher and runtime both identify this as an incomplete native 2D
preview. The production GPU control separately refuses hidden-window startup, presents a known
semantic frame, and then counts a window hidden after startup as temporarily unavailable while
still completing the semantic submission. C091 records the combined window-ownership evidence and
its falsifier.

Gap: this is a real model path, not full-frame visual correctness. Custom pixel policies and
unsupported J3D programs are refused rather than approximated. The remaining lit and multi-texture
programs, skinning, authored mip chains, particles, and effects
remain absent, so the preview is not yet a complete product renderer.

### S005 — Decomp PC-native semantic renderer

The decomp `J3DShape::draw` body now calls a native-layout semantic adapter and then always continues
through its original GX body. Its loader-swapped vertex arrays are identified explicitly as host
byte order while display-list commands and indices remain big-endian; a planted mixed-byte-order
control proves positions and UVs decode correctly. Relocated host-order `ResTIMG` headers feed the
same shared image decoder used by the recomp adapter. The active projection currently comes from
the same high-level `TGraphics` camera value as recomp, copied at the native decomp's
`TViewObj::testPerform` funnel; `GXGetProjectionv` is no longer used by the semantic adapter. A
guarded 400-frame direct-to-Delfino audit
considered 141,825 shape draws and submitted 42,852 supported model draws containing 27,326,178
vertices, with zero layout, rigid-matrix, or mesh-decode failures. It rejected 55,825 unsupported
materials and 43,148 non-perspective draw contexts, leaving those to the retained renderer.
A post-change guarded 180-present run submitted 11,858 models/7,194,726 vertices with zero layout,
rigid-matrix, or decode failures. It measured 36,422 perspective, 1,713 orthographic, and 3,547
pre-camera high-level dispatches; 12,154 model attempts outside perspective fell back cleanly.
After the shared raster-policy integration, a guarded 120-present run exited cleanly and submitted
2,278 cutout/back-cull models containing 513,876 vertices, with zero layout, rigid-matrix, or decode
failures. The first cold run exceeded the default 15-second in-process watchdog while Aurora was
creating a pipeline; rerunning with a 60-second diagnostic watchdog completed normally, so the
aborted cold run is recorded as startup-cost evidence rather than silently discarded.

Native `TLightCommon::setLight` now publishes the same high-level stage-light input, including the
primary light's authored directional-specular shininess, through a small
value-only bridge after using those values for the retained GX body. The bridge is linked directly
into `sms-boot`, while the decomp callback remains weak so decomp-only tests do not acquire a host
renderer dependency. The native-layout audit independently exercises material, normal, ambient, and
light extraction while sharing only the renderer-neutral lighting calculation with recomp; its
reached-model evidence is recorded once in C095. The latest guarded 400-present run submitted 666
lit models among 42,033 total after the shared specular hand material was admitted.

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

The matching decomp `J2DWindow::draw_private` body now publishes the same gradient, optional
contents texture, corners, and edge strips before retaining GX emission. Anonymous members were
renamed for the palette, five textures, mirror flags, four contents colours, frame remap colours,
and stored minimum dimensions. The production-linked native-layout control drives a real
`J2DWindow` through the shipping adapter and observes one rectangle plus nine picture submissions,
including copied decoded pixels and a balanced host-allocation gate. This extends readable decomp
behavior alongside the recomp override without sharing object layouts.

The decomp scopes each retained `J2DScreen::draw`, copies the real J2DOrthoGraph values without
retaining its stack pointer, and publishes the final logical pane clip at picture entry. Its host
composition activates the same client. Frame begin, seal, consume, and next begin remain
inside the host-allocation boundary where required, and semantic GPU teardown completes before
Aurora teardown. A guarded 400-present title run completed all 400 semantic frames; 350 were
nonempty and eleven contained both operation families. The stream carried 9,271 operations: 9,207
pictures, 64 solid rectangles, and 9,207 images. Semantic frame 104 was the first sampled nonclear
frame and contained 149,927 pixels distinct from clear. Aurora remains the visible GX renderer.

The explicit decomp preview uses the same window-claim path while Aurora remains an offscreen
reference. A guarded 400-present run completed and presented all 400 semantic frames, reported zero
unavailable-window frames, carried 2,790 native pictures plus 52 solid rectangles, and first
observed 158,038 non-clear pixels on semantic frame 311. The earlier 130-present attempt correctly
failed its non-clear evidence gate: it had presented every frame but had not yet reached visible
semantic content, so presentation count alone was not accepted as proof of useful output.

The title audit's window-specific counters remained zero, so its 9,271-operation result proves the
new code is inert when no window is drawn, not live window coverage. A stage-one audit stopped in a
pre-existing retained-Aurora invalid-wrap failure before any window submission; issue 30 records
the core and older reproductions rather than attributing that failure to this adapter.

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

Gap: decomp window behavior has close production-linked coverage but not an organically reached
live window in the bounded title/stage-one routes. Custom J3D pixel policies, the remaining lit and
multi-texture programs, skinning, authored mip chains,
particles, and effects remain missing from its visible native preview.

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

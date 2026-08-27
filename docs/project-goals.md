# Project goals

This is the canonical registry of Sunbright's durable epic-level outcomes. It does not track
current progress; consult `docs/project-state.md` when that registry exists, then the issue catalog
and `docs/codemap.md`, for the next concrete work point.

The goal names are stable commands. When the user says **Work on _name_**, use the matching goal
below as the scope, consult the project registries for current evidence, and take the next
ground-truth-ready work point without asking the user to restate the goal.

| ID | Name | Command |
|---|---|---|
| G001 | Lerp Coverage | `Work on Lerp Coverage` |
| G002 | Decomp Expansion | `Work on Decomp Expansion` |
| G003 | Recomp Native Renderer | `Work on Recomp Native Renderer` |
| G004 | Decomp Native Renderer | `Work on Decomp Native Renderer` |

## G001 — Lerp Coverage

**Outcome:** Every rendered target that should move between simulation ticks is identified and
given the correct interpolation treatment.

**Why it matters:** Missing or wrongly classified lerp targets leave visible 30 Hz stepping in an
otherwise interpolated presentation path. A complete census also prevents first sightings,
discontinuous returns, and intentionally exact or 2D draws from being misreported as defects.

**Success conditions:**

- The graphics census observes the exercised rendering waists and keeps an explicit unlabelled
  control for output it could not attribute.
- Each observed target is evidence-classified as interpolated, correctly snapped, or a genuine
  missing target; targets that interpolate have stable identity and the required matrix, vertex,
  texture, camera, or effect state coverage.
- Representative coverage includes every stage and game path needed to expose stage-specific
  emitters, rather than treating a shared boot scaffold as stage coverage.
- Every new comparison or coverage instrument has a control that visibly produces the other
  answer and declares what it does not measure.

**Constraints and non-goals:**

- Do not raise a percentage by counting births, long gaps, discontinuities, 2D draws, or exact
  screen-space effects as interpolation successes.
- Do not use a magic motion threshold or unstable draw ordinal as identity.
- This goal covers presentation interpolation of 30 Hz game state. Native simulation-rate work is
  separate unless it directly exposes a missing interpolation target.

## G002 — Decomp Expansion

**Outcome:** Grow the native decomp toward complete, well-named game behavior while converging with
upstream instead of building a permanent fork.

**Why it matters:** The decomp is the moddable end state, the readable specification for retail
guest behavior, and the source path that prevents guest-dependent recomp work from existing only as
an opaque override.

**Required order whenever this goal is selected:**

1. Rebase on upstream first, audit the result, and converge equal-or-better upstream files.
2. Rename known `unk*` fields and functions whose semantics are already established by binary
   evidence or use sites.
3. Extend the remaining decomp gaps from binary evidence, with close tests before whole-system
   checks.

**Success conditions:**

- The current upstream work is integrated before any gap is hand-ported, so local work does not
  duplicate community implementations.
- Established unknown semantics receive precise names and every declaration, definition, and use
  moves together.
- Remaining reachable gaps are replaced by faithful implementations supported by decompiler or
  equivalent binary evidence and focused regression tests.
- Native-platform adaptations preserve observable GameCube behavior without reproducing host-side
  corruption, and rendering-affecting game logic runs natively.

**Constraints and non-goals:**

- Do not guess names from shape or proximity; an unknown stays unknown until its semantics are
  established.
- Do not preserve local divergence when upstream is equal or better, and do not treat a green
  rebase as completion of the expansion pass.
- Do not hunk-merge class interfaces and implementations from different sides; move matching
  header/source ownership together.

## G003 — Recomp Native Renderer

**Outcome:** The recomp runtime renders through a PC-native, game-semantic renderer that does not
consume or reproduce the GameCube GX/FIFO pipeline.

**Why it matters:** A second implementation of GX is still GameCube rendering. The native renderer
exists to own PC scene, material, lighting, effect, resource, and presentation semantics directly,
making them understandable and extensible without a fixed-function compatibility layer.

**Success conditions:**

- Runtime overrides at verified J3D, J2D, particle, camera/light, material/resource, and named-effect
  seams emit renderer-neutral semantic scene data while retaining the original recomp bodies.
- The renderer owns PC-native meshes, materials, shaders, passes, resources, and presentation; its
  shipping input contains no FIFO commands, BP/XF register model, TEV program, or EFB-copy protocol.
- Representative scenes preserve the game's authored content, ordering, visibility, animation, and
  intended appearance without requiring pixel identity to GameCube fixed-function output.
- The completed recomp native lane runs and presents without Aurora or the SDL3 GX compatibility
  renderer.

**Constraints and non-goals:**

- Aurora and the SDL3 GX compatibility renderer are coverage/reference tools, not target
  architecture. Exact parity to either is neither necessary nor sufficient for this goal.
- Keep guest layout end to end. Recomp-to-decomp object interop remains banned.
- Native overrides keep their recompiled bodies available for controlled A/B and fallback; do not
  delete game behavior merely because a semantic native pass supersedes its draw path.
- The guest-code parity rule below applies to any renderer work that depends on game-owned guest
  behavior.

## G004 — Decomp Native Renderer

**Outcome:** The decomp runtime feeds the same PC-native, game-semantic renderer directly from native
game code, without lowering its shipping draw path through GX.

**Why it matters:** This gives the moddable native game path a fully project-owned renderer and
keeps renderer behavior shared across both runtimes instead of creating two implementations that
drift.

**Success conditions:**

- Native J3D, J2D, particle, camera/light, material/resource, and effect owners emit the same
  renderer-neutral semantic schema as the recomp adapters.
- The PC-native renderer implementation and semantic resource formats are shared between runtimes;
  only their object-layout adapters differ.
- Controlled comparisons against decomp-plus-Aurora establish content and behavior coverage without
  making GameCube pixel identity the endpoint.
- The completed decomp native lane launches, renders, and presents without Aurora or GX translation.

**Constraints and non-goals:**

- Do not build a second decomp-only renderer or copy the recomp layout adapter.
- The decomp remains native game code; do not route it through recompiled guest objects or revive
  the retired flip/interoperability boundary.
- Do not route the native result back through GX calls, FIFO records, TEV emulation, or EFB-copy
  emulation and call that native rendering.

## Cross-goal rule — guest-code parity

**Any recomp work that relies on guest code must also go through the decomp path.** A recomp-only
result is not complete when its behavior depends on game-owned guest logic: the corresponding
behavior, naming, evidence, or implementation must be carried through the decomp as part of the
same work. Recomp-only ownership remains appropriate for the recompiler, guest runtime substrate,
host application, hardware/OS seams, and renderer machinery that does not reimplement game logic.

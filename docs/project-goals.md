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

**Outcome:** The recomp runtime renders faithfully through Sunbright's native SDL3-GPU renderer,
with no Aurora dependency in the completed lane.

**Why it matters:** Owning the renderer makes the GX-to-PC translation inspectable, testable, and
modifiable without debugging final behavior through a third-party GX interpreter.

**Success conditions:**

- The renderer consumes the authoritative shared parsed-FIFO representation rather than a second
  ad hoc GX frontend.
- Geometry, transforms, raster state, TEV, textures, lighting, copies, effects, and presentation
  reach verified visual parity across representative scenes and stages.
- Intermediate state and final frames are compared against Aurora with controlled instruments
  while parity work remains.
- After parity is demonstrated, the recomp native-renderer lane runs and presents without Aurora.

**Constraints and non-goals:**

- Aurora remains the in-process parity oracle until the native path reaches parity; do not delete
  the oracle to make a comparison green.
- Keep guest layout end to end. Recomp-to-decomp object interop remains banned.
- The guest-code parity rule below applies to any renderer work that depends on game-owned guest
  behavior.

## G004 — Decomp Native Renderer

**Outcome:** The decomp runtime renders faithfully through the same native SDL3-GPU renderer rather
than Aurora.

**Why it matters:** This gives the moddable native game path a fully project-owned renderer and
keeps renderer behavior shared across both runtimes instead of creating two implementations that
drift.

**Success conditions:**

- The decomp GX path feeds the authoritative shared parsed-FIFO interface used by the native
  renderer.
- The renderer backend, GX state model, shaders, and verification tools are shared with the recomp
  native-renderer lane wherever their semantics are the same.
- Controlled comparisons against the existing decomp-plus-Aurora path establish parity across
  representative scenes and stages.
- The decomp native-renderer lane launches, renders, and presents without Aurora after parity is
  demonstrated.

**Constraints and non-goals:**

- Do not build a second decomp-only renderer or copy the recomp frontend.
- The decomp remains native game code; do not route it through recompiled guest objects or revive
  the retired flip/interoperability boundary.
- Aurora remains available as the oracle until this lane independently reaches parity.

## Cross-goal rule — guest-code parity

**Any recomp work that relies on guest code must also go through the decomp path.** A recomp-only
result is not complete when its behavior depends on game-owned guest logic: the corresponding
behavior, naming, evidence, or implementation must be carried through the decomp as part of the
same work. Recomp-only ownership remains appropriate for the recompiler, guest runtime substrate,
host application, hardware/OS seams, and renderer machinery that does not reimplement game logic.

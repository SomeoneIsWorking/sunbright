# The sea wash: direct attribution instead of bisection (2026-07-22)

Recomp renders the file-select/title sea washed near-white; the oracle (decomp+aurora, same
renderer) does not. Measured on the sea rect (150,640)-(470,800):

| run | near-white | mean |
|---|---|---|
| recomp baseline | 82.1% | (230,227,228) |
| oracle | 0.0% | (80,189,205) |

## The instrument

`SB_PIXEL_WATCH=x,y[,frame]` in aurora `draw_prim` transforms every prim's vertices through the
same matrices the GPU uses (per-vertex PNMTXIDX -> position matrix -> projection), divides by w,
and maps to screen pixels via `logicalViewport`. Each prim is classified:

- **COVERS** — the screen bounding box contains the watch point.
- **CROSSES** — at least one vertex has clip `w <= 0`. The box is *not* a valid coverage answer:
  the clipped polygon can smear far outside the box of the vertices in front, and a prim with
  every vertex behind the eye produces no box at all.

`SB_SKIP_COVERING=1` drops COVERS prims, `=2` drops COVERS + CROSSES. Both skip **in `draw_prim`,
before the vertex push, with `pos` still advanced** — a prim that merges into the previous draw
returns before ever reaching `push_gx_draw`, so a skip at that later site silently misses the
merged majority, and a skip that does not advance `pos` desynchronizes the FIFO parse.

## Results

1. `SB_SKIP_COVERING=1` — **393,001 covering draws dropped, sea UNCHANGED (82.2%)**.
   No prim whose transformed box contains the pixel paints it.
2. `SB_SKIP_COVERING=2` — sea **0.2% near-white, mean (22,59,64)**. The wash is produced by prims
   in the CROSSES class. (Too dark, because legitimate sea geometry was dropped along with it.)
3. Narrowing =2 by blend pair: `SB_SKIP_WNEG_BF=1,3` and `=1,5` **each independently** give
   0.2% / (22,59,64) — byte-identical. Two disjoint subsets cannot each be the sole cause;
   either the filter is not discriminating or the wash is produced redundantly by many draws.
   **Unresolved — do not build on result 3.**

### Negative controls (both refute "removing enough draws clears anything")

- 393,001 covering draws dropped -> wash unchanged.
- 640,401 opaque perspective 4-vertex quads dropped (`SB_SKIP_OPAQUE_P4`) -> wash unchanged.

### The oracle also has crossing prims

7,239 CROSSES prims in a sampled oracle title frame, and it renders correctly — so `wneg > 0`
per se is NOT the defect. The classes differ in kind: the oracle's dominant crossing family is
`bf=1/0` (opaque) with `wneg == vtxCount` (entirely behind the eye, correctly culled); the
recomp's at the watch pixel are `bf=1/3` (ONE/INVSRCCLR, 64x64) and `bf=1/5` (ONE/INVSRCALPHA,
256x256) with `wneg = 1` — *partial* crossings, which are the ones that smear.

## Falsified along the way

- "The sea draw paints the wash" — `SB_SKIP_VERTS=52` proved it does not.
- The extra alpha draws, the fader family (123,401 draws), the additive scene quad, and the
  opaque perspective quads: each skip verified to fire, none changed the wash.
- `SB_QUAD_RECT`'s "no large ortho quad" conclusion is **unsound** — it applied neither the
  position matrix nor the projection.

## Harness defects found and fixed

- Behind-eye vertices were `continue`d silently, so a prim could be reported as not covering the
  point while its clipped polygon crossed it. Now counted and surfaced as CROSSES.
- Indexed positions were read big-endian unconditionally, ignoring `arr.le`. Dormant for the
  recomp (guest arrays are BE) but wrong the moment the harness is pointed at a host-endian
  runtime — which is exactly the oracle A/B this investigation depends on.
- Draw indices are **not comparable between instruments or runtimes**; identification and
  causality must come from one instrument in one run, which is why the skip is keyed off the
  same `covers` verdict the report prints.

## Cross-run counts are not comparable

Each run is wall-clock bounded, so the number of frames rendered varies and cumulative draw
counts vary with it. Compare fractions within a run, never absolute counts across runs.

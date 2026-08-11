# The graphics registry

`graphics_db.tsv` in this directory is a census of **every graphic the port has been observed to
draw**, what is known about each one, and whether it interpolates at 60fps. The game writes it
itself; nobody maintains a list.

Playing the game populates it. Later, "work on the DB entries" has a definite meaning: run
`tools/gfx/graphics_db.py next`, take the top row, reverse-engineer what draws it, and give it an
identity so it interpolates — then record the verdict in the row.

## How a row gets there

Geometry reaches the GPU through three narrow waists, and all three are hooked
(`sms-recomp/frame_interp/tag_gap.cpp`):

| waist | kind | what it means for interpolation |
|---|---|---|
| `GXCallDisplayList` | `indexed` | positions live in a persistent vertex array — a **matrix** lerp reaches this geometry |
| `GXBegin` | `immediate` | the mesh is rebuilt by the CPU every tick — only the **vertex** path reaches it |
| `J3DShapeDraw::draw` | `j3d-shape` | J3D's own shape draw, attributed to *its* caller so the row names a game system, not a J3D internal |

The first time a guest address emits geometry through one of them, it is given a population id, its
symbol is resolved against `reference/sms_gmse01_funcs.txt`, and it gets a row. The label rides the
FIFO stream as `GX_AURORA_DRAW_POP`, so aurora can file every draw under the emitter that produced
it and report the fate it received.

The hand-labelled populations (`sms-recomp/frame_interp/populations.h`) have rows too, keyed
`pop.*`. A hand-written label always wins over an automatic one — it covers a whole subtree that the
emitter address cannot.

## The columns

**Measured — rewritten every run, do not edit:**

- `lerp` — `yes` · `partial` · `camera-only` (follows the camera but not its own motion) · `no` ·
  `2d-correct` (orthographic: there *is* no in-between, so snapping is right) · `exact-correct`
  (screen-space under a *perspective* projection — declared by a seam, because nothing else can
  detect it; it must not move) · `no-primitives` (the call site emits no geometry — a material
  display list) · `seam-owned` (a hand-written seam claims this site's primitives, so the site has
  no verdict of its own — the note names the `pop.*` row that holds the measurement) · `drew-once`
  (every draw was a FIRST SIGHTING — the graphic appeared on one tick and never again, so there was
  never a previous pose to interpolate from and there is no verdict to give) · `unmeasured`.

  `2d-correct` is a statement about the PROJECTION, not a proof that nothing was lost. The audit's
  screen-space motion report (`report_ortho_motion`) measures, per population, whether the ortho
  geometry differs tick to tick — and it reports WHICH DATA it hashed, because that decides how much
  the row is worth:

  - **own vertices** — the strong form. Only the screen wipe qualifies on Delfino; every other 2D
    population has no direct f32 positions.
  - **position matrix** — the element's PLACEMENT. A row reading 0% means "the placement did not
    move", not "nothing changed": a shape carried in vertices this path never saw is invisible to
    it. `fill_rect`'s provably-still verdict is this weaker claim.

  Either way a difference is not a defect. It may be smooth motion, which has an in-between, or a
  discrete content change such as a different glyph, which does not, and the measure cannot separate
  them. Read `2d-correct` as "orthographic, and unexamined beyond that" unless the motion report
  calls the row provably still — and then read which data it used.

  The report also prints how many DISTINCT position matrices the 2D draws carried per tick. That
  line exists because six unrelated sites once all read exactly "387 of 388", which is what one
  shared quantity reported under six names looks like; the count refuted it (12.48 per tick, 31 at
  most — the matrices are per-element and the identical counts were saturation). A near-1 count
  would mean every matrix-hashed row is the same global value wearing different labels.

  A **first sighting is excluded from the percentage entirely**, on both sides. An object being
  drawn for the first time has nothing to pair with, so counting it as a failure made every
  once-per-tick emitter read `partial` at 99.7% forever, and counting it as a success would credit
  the path for a frame it never produced. The audit prints the excluded count beside every row.
- `stages`, `first_seen`.

There are deliberately **no draw counts**. A row is a flag that a source of visual output exists and
what is known about it; counts changed on every run and made the tracked file churn without saying
anything. Draws are still measured every run — they decide the verdict — they are just not stored.

**Curated — the game never touches these:**

- `re` — `unknown` · `native-override` (an automatic *hint*: a native override exists for the
  emitting function) · `identified` · then the real verdicts a person records: `yes`, `partial`,
  `no`.
- `note` — why. Especially why a row that snaps is *correct* to snap.

Edit curated columns with the tool, never by hand:

```bash
tools/gfx/graphics_db.py set 0x8027d034 re=yes note="mirror quad; eye-space texgen, snaps by design"
```

## Attribution of SDK helpers

`GXDrawCube` and `GXDrawSphere` build their geometry themselves, so every cube in the game would
land in one row called `GXDrawCube+0x100`, which names nothing. Those two helpers redirect
attribution to *their* caller (`attrib_helpers.cpp`), which is how the row split into
`TMario::perform+0x654` and `+0x6c4` — Mario's occlusion-probe boxes, which then turned out to be
interpolable and now are.

## What it does not say

- **Absence is not evidence.** A graphic that has never drawn in a recorded run has no row. Play a
  stage and it appears. `summary` prints which stages have contributed, so a registry built entirely
  from Delfino says so.
- **`unmeasured` is not `no`.** The `lerp` column comes from aurora's interpolation audit, which
  only classifies draws when interpolation is running (`SBR_LERP60=1`). A row from a plain run says
  `unmeasured`, and no tool here counts that as a defect.
- **`camera-only` is an upper bound on the defect, not a measurement.** For genuinely static
  scenery, the camera delta alone is exactly right. Distinguishing the two needs the object, which
  is what the RE work in `next` produces.
- **A symbol like `sub_801983a8 (in drawUpper__8TMapWireCFv)`** is a function the US symbol list does
  not have — it omits weak methods entirely. The containing function comes from the *recompiler's*
  own table, which knows the real boundaries, and the parenthesised name is only the nearest listed
  neighbour. That distinction is not cosmetic: before it, three separate emitters were reported as
  `TMapWire::drawUpper+0x48 / +0x17c / +0x258`, and drawUpper contains exactly one `GXBegin` — two of
  the three were in `drawLower`.

## Do not delete the file to "regenerate" it

The curated columns live in the file and nowhere else — the game merges into it, it does not rebuild
it. Deleting it and running the game produces a complete, correct-looking registry with every `re`
and `note` gone. (Ask how this warning got written.) To re-measure, just run the game; to recover
from a bad edit, `git checkout docs/graphics/graphics_db.tsv`.

A row whose emitter later gets claimed by a hand-written seam stops being observed, and keeps the
last verdict it was measured with. That is not a bug in the file — it is what "a census of what was
observed" means — but it is why such rows should be curated with a note saying where the graphic
went, rather than left to read as a live measurement.

## The control

`(unlabelled)` — population 0 — is aurora's bucket for draws that **no** seam and **no** detected
site claimed. If a waist stops being hooked, or the game starts reaching the FIFO another way, those
draws pile up there and the run warns:

```
[gfxdb] N draw(s) were still filed under (unlabelled) — detection has a hole
```

That warning is the reason a quiet registry can be trusted: it is not a list of everything found, it
is a list with a counter for everything missed.

## Switches

- `SBR_GFXDB=0` — turn detection off entirely (it is on by default, in every run).
- `SBR_GFXDB_PATH=<file>` — write somewhere other than `docs/graphics/graphics_db.tsv`.
- `SBR_LERP60=1` — needed for the `lerp` column to be anything but `unmeasured`.

The file is rewritten every 300 presents rather than at exit, because automated runs end in
`SIGKILL` and an exit-only write would be empty for exactly the runs that draw the most.

## How complete is this census? (measured 2026-08-12)

The header already warns that a graphic which has never drawn in a recorded run is ABSENT, not
non-existent. Here is what that amounts to right now, so nobody has to infer it from row counts.

All 24 stages (1–15, 20–28) have contributed at least one row, which reads like full coverage and
is not. Eight of them — **stage10, stage11, stage12, stage15, stage25, stage26, stage27, stage28** —
each contributed exactly 15 rows, and those row sets are **identical to one another**. A run that
observes the same fifteen generic emitters as seven other stages and nothing else has not sampled
its stage; it has sampled the scaffolding every stage draws.

Two things could produce that, and this registry cannot tell them apart:

* the stage genuinely draws nothing beyond the common set (plausible for stage15, which is
  file-select, and possibly for some of 25–28), or
* the run never reached the stage's own content — it booted, drew the shared populations, and hit
  its present cap before anything distinctive loaded.

Until one of those is established per stage, treat a row's `seen:` list as evidence of where a
graphic HAS been observed and never as evidence of where it is absent.

The rest of the picture, for scale:

| | |
|---|--:|
| rows | 65 |
| rows present in EVERY sampled stage | 1 |
| stages contributing beyond the common set | 16 |
| stages whose sample is the common set only | 8 |
| rows observed in exactly one stage | 20 |

Stage-exclusive rows cluster in the stages that were actually played through — stage8 (8 rows),
stage4 and stage9 (3 each), stage6, stage1 and stage2 (2 each). That distribution is the honest
shape of this census: deep in a handful of stages, shallow in the rest.

Reproduce with the snippet in `debug_journal/2026-08-12_graphics_census_coverage.md`.

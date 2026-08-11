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

The eleven hand-labelled populations (`sms-recomp/frame_interp/populations.h`) have rows too, keyed
`pop.*`. A hand-written label always wins over an automatic one — it covers a whole subtree that the
emitter address cannot.

## The columns

**Measured — rewritten every run, do not edit:**

- `draws` / `calls` — draws aurora classified, and calls counted at the waist.
- `lerp` — `yes` · `partial` · `camera-only` (follows the camera but not its own motion) · `no` ·
  `2d-correct` (screen-space: there *is* no in-between, so snapping is right) · `no-primitives`
  (the call site emits no geometry — a material display list) · `unmeasured`.
- `interp_pct`, `stages`, `runs`, `first_seen`, `last_seen`.

**Curated — the game never touches these:**

- `re` — `unknown` · `native-override` (an automatic *hint*: a native override exists for the
  emitting function) · `identified` · then the real verdicts a person records: `yes`, `partial`,
  `no`.
- `note` — why. Especially why a row that snaps is *correct* to snap.

Edit curated columns with the tool, never by hand:

```bash
tools/gfx/graphics_db.py set 0x8027d034 re=yes note="mirror quad; eye-space texgen, snaps by design"
```

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
- **Symbols marked `?`** (e.g. `+0x3e24?`) fell in a gap of the US function list, which omits weak
  virtual methods entirely. The address is right; the name is the nearest preceding symbol and is
  probably the wrong function. Resolve those with `tools/re/vtable_re.py`.

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

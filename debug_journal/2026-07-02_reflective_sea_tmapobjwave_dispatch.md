# 2026-07-02 — Reflective sea root-cause: TMapObjWave not dispatched under SB_OWN_GXLIST=1

## The four-step plan, executed

### Step 1: oracle GXTEV capture at settled state

Ran the oracle at the settled title-screen (VI-timed Start presses baked into
`oracle_cache.sh`) with `SUNBRIGHT_DBG_GXTEV=1 SUNBRIGHT_DBG_GXAT=248
SUNBRIGHT_DBG_GXAT_WIDTH=4 SUNBRIGHT_GX_ATTRIB=1`. At the settled state (frame
~250), oracle draws **26 identical SRCALPHA/SRCCLR draws at efb_pass=3 tev=2
proj=persp, 52 verts each = 1352 verts total** — the reflective-sea composite.

TEV combiner content (identical for all 26):

```
s0 C d=ZERO a=ZERO b=ZERO c=ZERO op=+ bias=0 sc=x1 clamp dst=prev
   A d=ZERO a=ZERO b=tex c=ras op=+ bias=0 sc=x1 clamp dst=prev
s1 C d=ZERO a=ras.rgb b=ZERO c=ZERO op=+ bias=0 sc=x2 clamp dst=prev
   A d=ZERO a=ZERO b=tex c=prev op=+ bias=0 sc=x2 clamp dst=prev
reg0 R=255 A=255 B=255 G=255
reg1 R=194 A=0   B=190 G=242    ← turquoise sea constant (unused by color combiner)
reg2 R=0   A=72  B=0   G=0
reg3 R=0   A=144 B=0   G=0
```

Color combiner reads only `ras.rgb` (× 2, clamped). The turquoise constant in
reg1 is never consumed by the color combiner. Sea colour is vertex colour × 2.

Attribution (`[gxattrib]`) unambiguously names
`draw__11TMapObjWaveFv+0x184..0x23c` as the caller. This confirms
`reference/sms/src/MoveBG/MapObjWave.cpp::TMapObjWave::draw()` is the oracle
source of the reflective sea.

### Step 2: cross-match against native

Ran sms-boot at the same settled state with `SB_OWN_GXLIST=1 SB_BATCH_DBG=-1
SB_SEL_DUMP_SETTLED=2 SB_WAVE_DBG=1`. Findings:

- Native captures **2 SRCALPHA/SRCCLR scene batches (b12 ph1 + b72 ph6, 15
  verts each)** — but these are the file-select mask (`shaderKey=eb5c8e74…`,
  vertex colour white, texmean=9,9,9,9), NOT the sea.
- `imm_batches=24 imm_tris=198` — the imm buffer holds the HUD 2D overlay
  only, no wave grid.
- `[wave] ctor` **fires** → actor is genObject-constructed.
- `[wave] load` **fires** → tex/coef load runs.
- `[wave] perform` — **ZERO** logs.

So on native, TMapObjWave IS created + registered but its `perform()` is
NEVER dispatched. The oracle's 1352-vert sea composite is absent on native.

### Step 3: find the population that owns the material

Parsed the option.szs scene tree with `tools/scene_type_parse.py`:

```
[C] GroupObj                      /全体シーン
[C] GroupObj                      /全体シーン/コンダクター初期化用
    …
[C] GroupObj                      /全体シーン/鏡シーン
    MirrorCamera                  /全体シーン/鏡シーン/鏡カメラ
    MirrorModelManager            /全体シーン/鏡表示モデル管理
    MapObjWave                    /全体シーン/波            ← the sea
[C] GroupObj                      /全体シーン/スペキュラシーン
[C] GroupObj                      /全体シーン/インダイレクトシーン    ← EMPTY
    MarScene                      /全体シーン/通常シーン
```

Two key facts:

1. `/全体シーン/インダイレクトシーン` is EMPTY on disc. This RE-CONFIRMS
   that `TMapObjSeaIndirect::perform` being an empty stub in the source is
   FAITHFUL. Prior memory entries suggesting the sea = TMapObjSeaIndirect
   are falsified.
2. `MapObjWave` at `/全体シーン/波` is a **direct child of 全体シーン**, a
   sibling of `通常シーン` (MarScene). Native's `scene_drive.cpp`
   sb_boot_drive_scene walks only `通常シーン`, so `波` is a sibling that
   the walk cannot reach.

Under `SB_OWN_GXLIST=1` the code returns early with a comment claiming "the
real master GX perform list ALREADY dispatches TMapObjWave::perform(0x8)".
That comment is **empirically FALSE**: the stage-15 PerformLists.bin does
not push `波` into `mPerformListGX` (it drives via `描画ステージ /
DrawBuf*` pushes rooted at `通常シーン`), and `波` receives no dispatch.

This is the same class as the sky/chr/hud dropped-draw problem — native's
data-driven perform-list dispatch is short of the oracle for objects
outside the `通常シーン` subtree.

### Step 4: port the population — own the wave draw

`native/src/scene_drive.cpp` edit: under `SB_OWN_GXLIST=1`, call
`drive_wave()` explicitly (same pattern as the sky/chr/hud "own the draw"
work). This is not a bandaid — it's the sanctioned "own the path" fix
for a class of dispatch native doesn't reach.

The prior "double-draw" concern from session-15
([[fileselect-scene-underdraw-not-overdraw]]) has inverted: with
`drive_wave` off under OWN_GXLIST=1, native captures zero wave imm; with
`drive_wave` on, imm_tris jumps 198 → 1498 (+1300 tris = 26 strips × 50
tris, exact wave grid count), imm_batches 24 → 25 (drive_wave gate coalesces
into one batch).

`title_drawdiff.sh` before vs after:

- Before: `(1, 4, 2) sea signature` → `MISSING (bucket present on one side only)`
- After:  `(1, 4, 2) sea signature` → `oracle=29 native=3` (class NOW PRESENT)

The 29-vs-3 count residual is expected: native's raw-GX imm capture pipeline
coalesces consecutive same-state GXBegin/GXEnd pairs into one batch, whereas
the oracle records each strip as a distinct draw. Class presence is what
proves the wave now draws.

## Follow-ups (out of scope for this session)

- The (1, 4, 5) SRCALPHA/INVSRCALPHA class is 678 (oracle) vs 52 (native) —
  another under-draw, unrelated to the wave. This is likely a batch-count
  discrepancy from coalescing OR real missing map draws in ph2/ph3.
- Native imm coalescer merges the 26 wave strips into fewer batches — the
  drawdiff count gap is an artifact of that, not a fidelity gap. Confirm
  by inspecting the presented PNG (turquoise sea visible in native).

## Files touched

- `native/src/scene_drive.cpp` — removed the false "master perform list
  dispatches wave" comment; call `drive_wave()` inside the OWN_GXLIST=1
  branch.

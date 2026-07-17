# `TMarDirector::direct()` draw-flow — definitive control flow & the 60fps in-between gap

Scope: resolve EXACTLY how `TMarDirector::direct()` splits calc vs draw across its
`for(;;)` loop, settle the `unk34`-vs-re-issued-set contradiction in
`perform_list_architecture.md`, and pin the single most-likely cause of the
water-surface + Mario-shadow 30 Hz flicker the user still sees on the in-between field.

Primary sources (read, line-cited; not guessed):
- `decomp/sms/src/System/MarDirectorDirect.cpp:42-185` — `direct()`
- `decomp/sms/src/System/PerformList.cpp:4-19` — `perform`/`forEachPerform`
- `decomp/sms/src/JSystem/JDrama/JDRViewObj.cpp:3-15` — `testPerform`
- `decomp/sms/src/System/MarDirectorPreEntry.cpp:14-66` — `preEntry` (builds `unk34`)
- `decomp/sms/src/System/MarDirectorSetupObjects.cpp:383-479` — list construction & names
- `decomp/sms/src/System/MarDirectorInitECT.cpp` — `initECTGft`/`initECTMir`/`initECDisp`/`setupPerformList_console`
- `decomp/sms/src/JSystem/JDrama/JDRDrawBufObj.cpp:31-48` — `TDrawBufObj::perform` (★ the key)
- `decomp/sms/src/JSystem/JDrama/JDRSmJ3DScn.cpp:19-42` — `TSmJ3DScn::perform`
- `decomp/sms/src/Player/MarioMain.cpp:67-175` — `TMario::perform` (phase convention)
- `decomp/sms/src/Player/ModelWaterManager.cpp:1541+` — `TModelWaterManager::perform`
- `decomp/sms/src/MarioUtil/DrawUtil.cpp:93-167` — `TSilhouette::perform` (marukage)
- Runtime: `runtime/overrides/interp_redraw.cpp` (`kDrawLists`), `interp_capture.cpp`

Member offsets are the decomp header `decomp/sms/include/System/MarDirector.hpp:164-176`
and they match what the runtime override indexes (`g_mardir + 0x1C` … `+0x40`):

| off  | member                   | built by |
|------|--------------------------|----------|
| 0x1C | `mPerformListGX`         | PerformLists.bin ("PerformList GX") + `initECTMir` |
| 0x20 | `mPerformListSilhouette` | PerformLists.bin ("PerformList Silhouette") |
| 0x24 | `mPerformListGXPost`     | PerformLists.bin ("PerformList GX Post") + `initECDisp` |
| 0x28 | `mPerformListMovement`   | PerformLists.bin + setupObjects push_backs (CALC) |
| 0x2C | `mPerformListCalcAnim`   | PerformLists.bin + setupObjects push_backs (CALC) |
| 0x30 | `unk30`                  | `setupPerformList_console` (2D group, CALC) |
| 0x34 | `unk34`                  | **`preEntry`** — sky/map/players/water/conductor ENTRY |
| 0x38 | `unk3C`/`unk38`          | `initECTGft` (graffiti EFB) |
| 0x3C | `unk3C`                  | `initECTGft` |
| 0x40 | `unk40`                  | `drawBufferGroup` (light/drawbuffer pre-pass) |
| 0x44 | `mShinePfLstMov`         | PerformLists.bin (CALC) |
| 0x48 | `mShinePfLstAnm`         | PerformLists.bin (CALC) |

The interp in-between (`kDrawLists`, interp_redraw.cpp:47) re-issues, with
`param_1 = 0xFFFFFFFF`: **`{0x40, 0x38, 0x3C, 0x1C, 0x20, 0x24}`**. It does **NOT**
re-issue `0x34` (unk34), `0x30`, `0x28`, `0x2C`, `0x44`, `0x48`, nor the inline
`movement()`.

---

## 1. The `for(;;)` loop, walked exactly

`unk54` is the time budget. Per displayed frame `direct()` adds `vsyncRate` to it
(`MarDirectorDirect.cpp:64`) and then loops, each iteration spending 5 units
(`unk54 -= 5`, line 72). The branch key is **`unk4C & 0x4000`**.

State of `unk4C` on entry to a fresh displayed frame: `0x4000` and `0x2000` are CLEAR,
because the previous frame's final action was `unk4C &= ~0x6000` (line 180). So the
**first** iteration always enters **Branch A** (the `!(unk4C & 0x4000)` block, lines
68-165).

### Iteration 1 … N-1 — Branch A, calc-only (no draw)
- line 69-71: `++i`; on `i==1` set `unk4C |= 0x2000` (the "first sub-step" marker).
- line 72-74: `unk54 -= 5`; if it underflows (`< 5`) set `unk4C |= 0x4000`. This is how
  the LAST calc sub-step is identified — it becomes the draw step too.
- lines 77-100: build `uVar8` (suppression mask). On a non-final sub-step `uVar8 |= 2`
  (line 83): bit2 = "suppress draw-calc/anim". Paused/menu states add bits.
- lines 138-159: perform the CALC lists:
  - `mShinePfLstMov->perform(uVar4)` (0x44, shine movement; lines 145/147 — both
    branches identical),
  - `unk30->perform(~uVar44)` (0x30, 2D group; line 152),
  - `movement()` (inline game-object movement; line 153),
  - `mPerformListCalcAnim->perform(uVar11)` (0x2C; line 156) **only when `!(uVar8&2)`** —
    i.e. NOT on a non-final sub-step (since `uVar8` has bit2 there). So CalcAnim runs
    only on the final sub-step.
- line 161: `if (unk4C & 0x4000)` — FALSE on non-final sub-steps → fall through to
  bottom: `changeState()` (line 179), `unk4C &= ~0x6000` (line 180) **— which CLEARS
  0x4000 again** — and loop.

> ⚠ Consequence of line 180 clearing 0x4000 every iteration: **Branch B (the
> `else`, lines 166-178) is NEVER reached in normal play.** Once a sub-step sets 0x4000
> (line 74), the SAME iteration runs the line-161 draw tail and `break`s out of the
> loop (line 164) before the next iteration could see 0x4000 in the branch test. After
> the loop, line 180 has cleared it. Branch B only executes if some OTHER code path
> sets `0x4000` and re-enters the loop without clearing — it is effectively a dormant
> path under the steady-state frame loop. **This is the crux of the contradiction
> below.** (Flag: I could not find a path that legitimately re-enters with 0x4000 set;
> the dormant reading is what the code literally does.)

### The FINAL sub-step — Branch A draw tail (lines 161-164)
On the sub-step where `unk54` underflowed, `0x4000` is now set. The same Branch-A
iteration runs all the calc work above (this time `uVar8` lacks bit2, so CalcAnim
runs), then:
```
local_140.unk2 = 0;
unk34->perform(0xffffffff, &local_140);   // line 163  ── THE ONLY DRAW CALL on this frame
break;                                      // line 164  ── leave for(;;)
```
Then after the loop: `changeState()` (179), `unk4C &= ~0x6000` (180) — but the `break`
already left, so 179/180 run once post-loop, and `direct()` returns (line 184).

### How many calc iterations
`unk54 += vsyncRate` (=`600/SMSGetVSyncTimesPerSec()`; e.g. 20 at 30 Hz vsync), minus 5
per sub-step → typically **a few calc sub-steps then one draw**. The exact count is the
fixed-timestep accumulator; it does not matter for the draw split. What matters: **the
real frame draws exactly once, through `unk34->perform(0xffffffff)` at line 163.**

`0x4000`/`0x2000` are both cleared at line 180 each iteration. `0x2000` (set line 71 on
`i==1`) marks the first sub-step; `0x4000` (set line 74) marks the last/draw sub-step.

---

## 2. Resolving the contradiction — `unk34` is NOT a separate visible pass; it is a
## DEFERRED-BUFFER COLLECTION pass, and the SUBMISSION happens in the re-issued lists

`perform_list_architecture.md` says "the main scene is `unk34->perform(0xffffffff)`,
run right before `break`" — that statement is **literally true** (it is the only draw
call on the real field). But the inference that "re-issuing only {0x40,0x38,0x3C,0x1C,
0x20,0x24} must therefore be missing the whole scene" is **WRONG**, and the user's
observation (full scene renders on the in-between) is correct. Here is why.

**SMS does not draw geometry immediately when a model performs.** It uses J3D deferred
draw buffers (`J3DDrawBuffer`). The phase bits split into TWO distinct stages:

- **ENTRY (collection):** a model performed with `0x200` calls `entry()`/`entryModels()`
  which *appends* its shapes to the J3D system's currently-bound draw buffer. NO GX
  output (`MarioMain.cpp:67-90` — `param_1 & 0x200` → `entryModels`/`entry`, nothing
  drawn). View-calc (`0x4`) and `0x480` (`TDrawBufObj` 0x400 = bind buffer + 0x80 =
  `frameInit`) likewise produce NO GX geometry.
- **SUBMISSION (the actual GX draw):** `TDrawBufObj::perform` only emits geometry under
  **bit `0x8`** — `mDrawBuffer->draw()` (`JDRDrawBufObj.cpp:44-47`). `TSmJ3DScn::perform`
  similarly draws its buffers only under `0x8` (`JDRSmJ3DScn.cpp:25-41`).

Now look at the filters in `unk34` (`preEntry`, MarDirectorPreEntry.cpp:26-65):

| pushed object                | filter     | with `0xffffffff` → bits that run | does it draw? |
|------------------------------|------------|-----------------------------------|---------------|
| `DrawBuf Sky/Map/Chr/...`    | **0x480**  | 0x400 (bind) + 0x80 (frameInit)   | **NO** (no 0x8) |
| sky/map/manager/conductor grp| 0x204      | 0x200 (entry) + 0x4 (viewcalc)    | NO — collects into buffer |
| `マップ` map                 | 0x4000200 / 0x2000200 | 0x200 entry + special   | NO — collects |
| `ライトマネージャー`          | 0x400      | 0x400 (light bind)                | NO |
| `水マネージャ` water mgr      | **0x4**    | 0x4 (calcDrawVtx/calcVMAll only)  | **NO** (no 0x8/0x80) |
| `水飛沫マネージャ` splash     | 0x4        | 0x4                               | NO |
| `プレーヤーグループ`          | 0x204 (+ 0x10000000/0x8000000) | entry + special   | NO — collects |

**Every push in `unk34` is a COLLECT/BIND/VIEW-CALC filter. None carries `0x8`.** So
`unk34->perform(0xffffffff)` fills the draw buffers but submits NOTHING to GX by itself
for the named DrawBufs. The bytes that become pixels are emitted later, by the **`0x8`
submissions in the re-issued lists**:

- `initECDisp` (→ `mPerformListGXPost`, 0x24) pushes `DrawBuf ChrOpa`/`ChrXlu` at 0x480
  THEN at **0x8** (MarDirectorInitECT.cpp:188-194), and `DrawBuf LensFlare` at 0x8
  (lines 170/177) — these are the `draw()` submissions.
- `PerformList GX` (0x1C), `Silhouette` (0x20), `GX Post` (0x24), `unk40` (0x40),
  `unk38`/`unk3C` (graffiti) are the PerformLists.bin-defined lists whose membership
  (data-driven) carries the `0x8` draw filters for sky/map/water/silhouette buffers.
  (`unk40` pushes `drawBufferGroup` at filter **8** in setupObjects.cpp:383 — an
  explicit `0x8` draw.)

**Therefore the re-issued set DOES submit the scene** — because submission lives in
those lists, not in `unk34`. The reason the in-between renders the full scene without
re-issuing `unk34` is that **`unk34` only re-fills buffers that are still populated from
the real field** (the real field already entered the geometry; the buffers weren't
cleared between the real present and the in-between re-issue), and the re-issued `0x8`
submissions re-`draw()` those still-populated buffers.

> **CONCLUSION OF §2:** `unk34` is a SEPARATE pass the in-between is missing, BUT it is a
> *collection/view-calc* pass, not a *draw* pass — so skipping it does not blank the
> scene. What it DOES skip are the `0x4` view-calc and `0x200` entry steps for objects
> whose draw is in the re-issued lists. Any object that (a) computes per-field draw
> state ONLY in its `0x4`/`0x200`/`0x1` phase (which runs only in `unk34` or the calc
> lists) **and** (b) draws in a re-issued list, will **re-draw on the in-between using
> the previous field's state** — and if that state is the gate for whether it draws at
> all, it BLINKS. That is the flicker.

---

## 3. Per-list inventory: what each contains and whether the in-between replicates it

| list (off) | re-issued? | contains / draws | gap on in-between |
|------------|-----------|------------------|-------------------|
| `unk30` (0x30) | **no** | 2D group / Guide CALC (filter 3 = 0x1\|0x2). DRAW of HUD is in GXPost. | HUD anim/emit frozen; geometry still drawn via GXPost. Benign. |
| `unk34` (0x34) | **no** | **preEntry**: bind buffers (0x480), enter sky/map/players/water/conductor (0x200), view-calc (0x4). **No 0x8 → draws nothing itself.** | Re-fill skipped; buffers reused from real field. View-calc/entry NOT recomputed → see §4/§5. |
| `unk38`/`unk3C` (0x38/0x3C) | **yes** | `initECTGft`: graffiti/pollution EFB readback (TEfbCtrlTex 0x80, ortho 0x10, graffitiGroup). | Re-issued (re-reads EFB). Fine, but feeds EFB-feedback — see §6. |
| `unk40` (0x40) | **yes** | `drawBufferGroup` at **filter 8** → `mDrawBuffer->draw()` for sky/light pre-pass. | Re-submitted. OK. |
| `mPerformListGX` (0x1C) | **yes** | PerformLists.bin "GX" + `initECTMir` (mirror EFB). The map/sky/water `0x8` draw submissions live here (data-driven). | Re-submitted. OK. |
| `mPerformListSilhouette` (0x20) | **yes, CONDITIONAL** | PerformLists.bin "Silhouette": the **marukage** (`TSilhouette`) and water-surface silhouette draws. Re-issued by `direct()` AND interp ONLY when `gpSilhouetteManager->unk48 > 0 \|\| gpCamera->unk2C8 != -1` (direct.cpp:172-175). | **★ THE FLICKER.** See §4. |
| `mPerformListGXPost` (0x24) | **yes** | `initECDisp`: `stageDisp` EFB copy/display (0x80/0x8), char DrawBuf draws (0x8), lensflare/glow/sheen (0x204), 2D overlays/Guide (0x8). | Re-submitted. Lens-flare/glow re-draw with stale screen-pos (lag, not blink). |
| 0x28/0x2C/0x44/0x48 + `movement()` | **no** | game movement, calc-anim, shine sprite calc. | Game state NOT advanced (correct — 30 Hz sim; that is the design). |

---

## 4. The marukage shadow + water-surface silhouette — the flicker, precisely

`TSilhouette::perform` (DrawUtil.cpp:93-167) splits by bit:
- `0x1` (movement, **calc list only**): `unk48 += unk4C * ((occluded?unk50:0) - unk48)`
  — the occluded-alpha chase (lines 95-99). This is the ONLY place `unk48` changes.
- `0x8`/`0x80`: silhouette / sun lighting setup (draw).
- `0x10` (draw): the actual round-shadow GX draw, projector matrix built from
  **`gpMarioPos`** (lines 116-166), gated on `gpPollution->getJointModelNum()`.

`gpSilhouetteManager` (the `TSilhouette`) is in `mPerformListSilhouette` (0x20,
data-driven). Two compounding facts make it flicker:

1. **The re-issue GATE reads `unk48`, which the in-between never refreshes.** Both
   `direct()` (line 172) and the interp override (interp_redraw.cpp re-issuing 0x20)
   only perform the silhouette list when `gpSilhouetteManager->unk48 > 0.0f` (or a
   demo-cam condition). `unk48` is advanced exclusively in the `0x1` phase, which runs
   only in the calc lists — **not** in any re-issued list and **not** in `unk34`. So:
   - On the real field, the calc pass moved `unk48`. If it is at/near 0 (Mario not
     occluded) the silhouette list is skipped on the real field; if positive it draws.
   - On the in-between, `unk48` holds whatever the last calc pass left. If the value
     straddles 0 across frames, the in-between's gate and the real field's gate
     disagree → **shadow draws on one field, not the next = the on/off blink.**
   - The probe already instruments exactly this:
     `interp_redraw.cpp` reads `sil_before`/`sil_after` (mgr+0x48) and reports "in-between
     MUTATED occlusion alpha" / "marukage draws on REAL but NOT in-between".

2. **Even when both fields draw, the shadow uses `gpMarioPos` (a global Vec), not a
   draw matrix.** The registry blend (mode 3) only touches `mDrawMtxBuf` of registered
   J3DModels; `gpMarioPos` is read raw in `TSilhouette::perform` (DrawUtil.cpp:71,127).
   On the in-between Mario's *model* interpolates (its draw matrices are blended) but the
   shadow projector snaps to tick-N `gpMarioPos` → shadow detaches from the feet every
   other field. interp_redraw.cpp ALREADY blends `gpMarioPos` toward N-1 (the
   `shadow_blend` path) to address this — but that fixes detach, not the §4.1 gate blink.

**Water-surface silhouette shares the same `unk48` gate.**
`TModelWaterManager::drawSilhouette` (ModelWaterManager.cpp:920) early-returns unless
`gpSilhouetteManager->isUnk48Positive()` (line 922). Its `0x8` draw
(`drawSilhouette`/`drawWaterVolume`, ModelWaterManager.cpp:1541 §`param_1 & 8`) is in a
re-issued list (data-driven GX/Silhouette membership), but it is gated by the SAME
unrefreshed `unk48`. So the water surface and the shadow blink **together, in sync** —
exactly the "water surface + Mario's shadow flicker" the user reports.

Note also `TModelWaterManager`'s `0x1` move + `0x4` calcVMAll run only in calc/`unk34`,
so even when drawn the water particles are frozen for the in-between (stutter, secondary
to the blink).

---

## 5. `param_1 = 0xFFFFFFFF` on the in-between — is it safe?

`TPerformList::forEachPerform` (PerformList.cpp:9-12) dispatches
`it->unk4->testPerform(param_4 & it->unk8, g)` — **the incoming mask is AND-ed with each
link's per-membership filter `unk8` first.** `testPerform` (JDRViewObj.cpp:3-15) then
clears the object's `unkC` disable bits and runs `perform` only on the survivors.

So passing `0xFFFFFFFF` does NOT make an object run phases it isn't registered for in a
given list: `0xFFFFFFFF & unk8 == unk8`. Each re-issued list runs **exactly the phases
its own membership filters select** — i.e. the same phases the real field's Branch-B/
draw-tail pass would have run with `0xffffffff` (the game itself passes `0xffffffff` to
every one of these lists: direct.cpp:163,168-176). **The in-between uses the identical
mask the game uses for these lists.**

Could an object double-advance? Only if a *re-issued* list contains a membership with a
**movement/anim bit (0x1/0x2)**. The re-issued lists are all draw lists; their
PerformLists.bin/`initEC*` memberships use draw/entry/light filters (0x4, 0x8, 0x10,
0x80, 0x200, 0x204, 0x400, 0x480, high bits) — **no 0x1, no 0x2** (those live in
0x28/0x2C/0x30/0x44/0x48, which are NOT re-issued). The one nuance: filter **0x4**
(view-calc) objects re-run their view-calc on the in-between (e.g. water `calcVMAll`
would, if water were in a re-issued list at 0x4 — but in `unk34` it's 0x4 and `unk34`
is NOT re-issued; in the GX list its draw membership is 0x8). Re-running view-calc is
idempotent on stale inputs and is actually desirable for interpolation.

> **CONCLUSION OF §5:** `0xFFFFFFFF` is SAFE — it is the same mask the game passes, and
> the per-link `unk8` AND-filter guarantees no object runs a movement/anim phase it is
> not registered for in a draw list. The flicker is **not** a `0xffffffff`
> double-advance. (One real risk that 0xffffffff carries: it INCLUDES `0x80`, so every
> re-issue re-runs the EFB-feedback/`frameInit`/`drawRefracAndSpec` post phase — see §6.)

---

## 6. EFB feedback (why the two failed fixes failed) — context, not the root cause

- Failed fix (a) reused the real field's EFB screen-texture → ghosted Mario into the
  reflection. That is because Mario's GEOMETRY is at the interpolated pose on the
  in-between while the reused capture has him at tick N → duplicate. Confirms the
  reflection samples `スクリーンテクスチャ` (the EFB copy in GXPost/stageDisp 0x80).
- Failed fix (b) suppressed the draw-sync EFB-feedback callbacks → still flickers.
  That is expected: the EFB feedback is the *refraction/reflection sampling*, a
  SECONDARY effect. The PRIMARY flicker is the §4 `unk48`-gate on/off of the silhouette
  list — freezing EFB feedback does nothing to the gate decision.

Both confirm the real defect is upstream of EFB: it is **whether the silhouette/water
draw runs at all on the in-between**, governed by `unk48`.

---

## CONCLUSION (single most-likely cause)

**The water-surface silhouette and Mario's round shadow (marukage) both draw in
`mPerformListSilhouette` (0x20), whose re-issue is GATED on
`gpSilhouetteManager->unk48 > 0` (`MarDirectorDirect.cpp:172`, mirrored in
interp_redraw.cpp). `unk48` is the Mario-occlusion alpha and is advanced ONLY in the
`0x1` movement phase — which runs in the calc lists (`mPerformListMovement`/the inline
`movement()`/`unk34`'s `0x1` path), NONE of which the in-between re-issues. So the
in-between evaluates the gate against a stale `unk48` (and the water draw shares the same
`gpSilhouetteManager->isUnk48Positive()` gate, ModelWaterManager.cpp:922). When `unk48`
straddles 0 between frames, the silhouette list runs on the real field but is skipped on
the in-between (or vice-versa) → the water surface + shadow appear on one field and are
missing on the next = the in-sync 30 Hz flicker.**

This is "draw X (silhouette list: marukage + water silhouette/volume) runs gated on a
calc-only value the in-between never replicates" — **not** an object double-advancing on
`0xffffffff` (§5 rules that out), and **not** an EFB-reuse problem (§6, the failed fixes
confirm it is upstream of EFB).

Secondary/contributing (lower severity, real but not the primary blink):
- Even when both fields draw, `gpMarioPos` snaps to tick N for the shadow projector
  (DrawUtil.cpp:127) → detach; interp_redraw already blends it (`shadow_blend`).
- Water particles (`move`/`calcVMAll`, 0x1/0x4) are frozen on the in-between → stutter.
- Lens flare/glow re-draw with last field's screen position (calc in 0x1/0x2) → 1-field
  lag, not blink.

### Recommended fix direction (design only — no code in this doc)
Make the in-between's silhouette/water-shadow decision MATCH the real field instead of
re-evaluating a stale gate. Two faithful options:
1. **Snapshot the real field's gate result** (`unk48`, and the demo-cam condition
   `gpCamera->unk2C8`) at the end of the real field's draw and re-use that exact boolean
   for the in-between's `mPerformListSilhouette` re-issue — so both fields make the same
   draw/skip choice. (Lowest-risk; matches what the real field actually rendered.)
- This does NOT require running the `0x1` calc phase on the in-between (that would
  double-advance the alpha chase, an actual `unk48` over-step). Mirror the value, do not
  re-run the phase.

### Honest gaps
- The exact PerformLists.bin membership/filters of `PerformList GX/Silhouette/GX Post`
  (where `TSilhouette` and `水マネージャ`'s 0x8/0x80 draws are registered) is binary
  data, confirmed by behavior (the silhouette list re-issue gate references
  `gpSilhouetteManager`; the water silhouette early-returns on `isUnk48Positive`) but
  not enumerable from source. To make it ground truth, dump `/data/PerformLists.bin`
  (loaded at setupObjects.cpp:406-422) or walk each `TPerformList` at runtime printing
  each `TPerformLink.unk4` name + `unk8` filter.
- Branch B (direct.cpp:166-178) is dormant in the steady-state loop (line 180 clears
  0x4000 before it can be re-tested); I could not find a re-entry path that exercises it.
  The real-field draw is the line-163 `unk34` tail only. If a future reading finds a
  legitimate Branch-B entry, revisit — but it does not change the conclusion (those
  lists are the re-issued ones and carry the same gate).

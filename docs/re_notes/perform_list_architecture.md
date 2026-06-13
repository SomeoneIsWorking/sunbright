# Perform-list architecture (SMS) — what draws in which list / which phase

Goal of this doc: give the 60fps interpolation path a precise map of **which visual
effect draws in which TPerformList, gated on which perform-phase flag bit**, so we can
say exactly what an "in-between" field re-issue (draw-only lists, no calc/movement) is
missing or double-processing.

Primary sources (read, not guessed):
- `reference/sms/src/System/MarDirectorDirect.cpp` — `TMarDirector::direct()` frame loop
- `reference/sms/src/System/PerformList.cpp` — `TPerformList::perform` / `forEachPerform`
- `reference/sms/src/JSystem/JDrama/JDRViewObj.cpp` — `TViewObj::testPerform`
- `reference/sms/src/System/MarDirectorSetupObjects.cpp` — list construction
- `reference/sms/src/System/MarDirectorInitECT.cpp` — `initECTGft` / `initECTMir` / `initECDisp`
- `reference/sms/src/System/MarDirectorPreEntry.cpp` — `preEntry` (the MAIN 3D scene list)
- `reference/sms/src/System/MarDirectorSetup2.cpp` — `setup2`
- `reference/sms/src/MarioUtil/DrawUtil.cpp` — `TSilhouette` (the marukage round shadow)
- `reference/sms/src/Player/ModelWaterManager.cpp` — `TModelWaterManager` (Mario water FX)
- `reference/sms/src/Camera/lensglow.cpp`, `lensflare.cpp` — sun glow / lens flare
- `reference/sms/src/Player/MarioMain.cpp` — `TMario::perform` (same flag convention)

> ⚠ MEMBER-OFFSET CORRECTION vs the task framing. The decomp header
> `reference/sms/include/System/MarDirector.hpp` (lines 165–176) gives the **actual**
> offsets, which differ from the offsets named in the task prompt:
>
> | off  | member                  | role |
> |------|-------------------------|------|
> | 0x1C | `mPerformListGX`        | mirror-pass GX (initECTMir) — NOT the main scene |
> | 0x20 | `mPerformListSilhouette`| silhouette pass (re-issued conditionally) |
> | 0x24 | `mPerformListGXPost`    | post / 2D / lensflare-chr / HUD (initECDisp) |
> | 0x28 | `mPerformListMovement`  | movement/calc list |
> | 0x2C | `mPerformListCalcAnim`  | calc-anim list |
> | 0x30 | `unk30`                 | 2D group (setupPerformList_console): Group 2D / 2D 2 / Guide |
> | 0x34 | `unk34`                 | **THE MAIN 3D SCENE DRAW** (preEntry): sky, map, players, water, conductor |
> | 0x38 | `unk38`                 | graffiti EFB-readback pass (initECTGft) |
> | 0x3C | `unk3C`                 | graffiti EFB-readback pass (initECTGft) |
> | 0x40 | `unk40`                 | pre-pass: DrawBufferGroup + DrawBuf Sky (lightmgr) |
> | 0x44 | `mShinePfLstMov`        | shine-sprite movement list |
> | 0x48 | `mShinePfLstAnm`        | shine-sprite anim list |
>
> The single most important consequence: **the bulk of the 3D scene (map, sky, players,
> Mario's water FX, the conductor/objects) is performed through `unk34` (0x34), built by
> `preEntry()`, NOT through `mPerformListGX` (0x1C).** `mPerformListGX` is the mirror
> pass. If the interpolation path re-issues "mPerformListGX/Silhouette/GXPost" by those
> names and skips `unk34`, it is skipping essentially the entire scene — so in practice
> the interp path must already be re-issuing `unk34`. This doc assumes the re-issued set
> is "the draw-branch lists" (unk40/unk38/unk3C/mPerformListGX/Silhouette/GXPost) **plus
> unk34**, and the NON-re-issued set is the calc lists (unk30 + Movement + CalcAnim +
> Shine Mov/Anm) — that is what `direct()` actually splits on. Confirm against the
> runtime override before trusting the conclusion list.

---

## 1. `TMarDirector::direct()` — the two-phase per-frame loop

File: `reference/sms/src/System/MarDirectorDirect.cpp:42–185`.

> Address note: `direct()` is declared `virtual int direct()`
> (`MarDirector.hpp:101`) but is **not separately symbolized** in
> `reference/sms_gmse01_funcs.txt` (the MarDirectorDirect.cpp TU funcs — direct /
> changeState / updateGameMode / moveStage — are not individually listed; the named
> MarDirector cluster jumps from `movement_game__12TMarDirector 8029a788` straight to
> `setupPerformList_console 8029ad3c`). Cite the source file; the runtime entry is the
> `TMarDirector` vtable `direct` slot.

### The loop shape

`direct()` runs an **infinite `for(;;)`** that `break`s out only on the draw pass. Each
outer iteration is one **calc sub-step** (multiple per displayed frame, to keep game
logic at a fixed rate when vsync rate varies — `vsyncRate = 600 / SMSGetVSyncTimesPerSec()`,
`unk54 -= 5` each step, set `0x4000` when the time budget for this frame is spent).

Two branches, keyed on `unk4C & 0x4000`:

#### Branch A — `!(unk4C & 0x4000)` : the CALC / MOVEMENT branch (lines 68–165)

Runs once per calc sub-step. It:
- on first sub-step sets `unk4C |= 0x2000` (line 71) — `0x2000` means "first sub-step of frame".
- decrements `unk54`; if it underflows sets `unk4C |= 0x4000` (line 74) → this becomes the LAST sub-step, which will then ALSO run the draw tail at 161–164.
- builds `uVar8` (line 77+): `uVar8 |= 2` when NOT the draw step (line 83); plus `|= 1` and `|= 2` for paused/menu states (lines 88–100). `uVar8` is the **suppression mask**: bit1 = "suppress movement", bit2 = "suppress draw-calc/anim".
- updates pads / rumble / hit-check.
- builds `local_140.unk2` (the TGraphics field) from `0x2000`/`0x4000` → bits 1/2 (lines 121–126).
- performs the calc lists (see flag derivation below).

Flag derivation in branch A:
```
uVar11 = ~uVar8                       // line 138  (full mask minus suppressed bits)
uVar4  = uVar11
if (unk58 & 1) uVar4 &= ~0x100        // line 141  (alt-frame: drop 0x100)
if (unk58 & 2) uVar4 &=  0x200        // line 143  (alt-frame: keep only 0x200)
mShinePfLstMov->perform(uVar4, ...)   // line 145/147  (0x44)

uVar44 = (!(unk4C&0x4000)) ? 2 : 0
unk30->perform(~uVar44, ...)          // line 152  (0x30: 2D group)
movement()                            // line 153  (TMarDirector::movement — game obj movement)
if (!(uVar8 & 2))                     // line 154  (only when draw-calc not suppressed)
    mPerformListCalcAnim->perform(uVar11, ...)   // line 156 (0x2C)  [if unk4E&1]
    else mShinePfLstAnm->perform(uVar11, ...)    // line 158 (0x48)
```
So on a normal calc step `uVar8 = 2`, `uVar11 = ~2 = 0xFFFFFFFD`. That mask has bit1
SET, bit2 CLEAR. i.e. calc lists run with **0x1 set (movement), 0x2 clear (no
matrix/anim-calc-draw)** — except the line-154 guard further gates CalcAnim.

#### Branch A tail — the DRAW pass (lines 161–165), only when `unk4C & 0x4000`

On the FINAL sub-step the same branch-A iteration also does:
```
local_140.unk2 = 0;
unk34->perform(0xffffffff, &local_140);   // line 163  — THE MAIN 3D SCENE
break;                                     // leave the for(;;)
```
**This is the main scene draw.** `unk34` is the `preEntry()` list (sky/map/players/
water/conductor/graffiti/indirect). It is performed with **all flag bits set**
(`0xffffffff`), so every object's draw-phase bits (0x4 calc-vtx, 0x8 draw, 0x10
tex/shadow, 0x80 post, 0x200 entry, 0x400 lightmgr, 0x480 drawbuf) fire in this one call.

#### Branch B — `(unk4C & 0x4000)` : the pure GX DRAW branch (lines 166–178)

NOTE: this branch is reached on the **NEXT** outer iteration after `0x4000` was set —
but the `break` at line 164 leaves the loop first. Re-reading: line 161 runs the
`unk34` draw then `break`s; so branch B (166–178) executes on iterations where `0x4000`
was already set coming IN (it is cleared at line 180 `unk4C &= ~0x6000` after
`changeState`, so on a fresh frame `0x4000` is clear and branch A runs). In practice the
draw work is split: the EFB-prep / mirror / silhouette / post passes are branch B; the
main scene is the branch-A tail. Both happen within one displayed frame. Branch B:
```
local_140.unk2 = 0;
unk40->perform(0xffffffff, ...)   // 0x40  pre-pass: DrawBufferGroup + Sky drawbufs + lightmgr
unk38->perform(0xffffffff, ...)   // 0x38  graffiti EFB pass
unk3C->perform(0xffffffff, ...)   // 0x3C  graffiti EFB pass
mPerformListGX->perform(0xffffffff, ...)        // 0x1C  MIRROR pass (initECTMir)
if (gpSilhouetteManager->unk48 > 0 || gpCamera->unk2C8 != -1)
    mPerformListSilhouette->perform(0xffffffff, ...)   // 0x20  silhouette pass
mPerformListGXPost->perform(0xffffffff, ...)    // 0x24  post / 2D / HUD / lensflare-chr
GXInvalidateTexAll();             // 0x80360400
```

### Phase-flag bit dictionary (the param_1 convention)

These bits are a **global convention** shared by every `TViewObj::perform` in the game
(confirmed in `TMario::perform` MarioMain.cpp:67–170, `TModelWaterManager::perform`
ModelWaterManager.cpp:1541, `TSilhouette::perform` DrawUtil.cpp:93, lens glow/flare).
A perform body tests `param_1 & BIT` and only does that phase's work if set.

| bit       | meaning (phase) | typical work |
|-----------|-----------------|--------------|
| `0x1`     | **MOVEMENT**    | game-logic move/think, position update, particle move (calc list) |
| `0x2`     | **CALC-ANIM**   | animation advance, joint/matrix calc, `calcAnim`, `viewCalc` prep (calc list) |
| `0x4`     | **VIEW-CALC / calc-draw-vtx** | view-matrix-dependent calc: `calcView`, water `calcVMAll`, lens `viewCalc` |
| `0x8`     | **DRAW (opaque pass)** | immediate-mode GX draw / drawbuf opaque entry; silhouette lighting setup; water silhouette+volume |
| `0x10`    | **DRAW (2nd / tex pass)** | ortho/tex pass; **marukage round-shadow draw** (TSilhouette 0x10); graffiti ortho |
| `0x80`    | **POST / EFB**  | EFB readback (TEfbCtrlTex), water `drawRefracAndSpec`, silhouette sun pass |
| `0x100`   | **(alt-frame draw gate)** | dropped on `unk58&1` frames (line 141) — interlace/alt-field draw control |
| `0x200`   | **ENTRY (drawbuf submit)** | `J3DDrawBuffer` entry / model entry into sort buffer (the real geometry submit) |
| `0x400`   | **LIGHT MANAGER** | `ライトマネージャー` light-buffer pass |
| `0x480`   | = 0x400\|0x80   | the `DrawBuf *` objects are pushed with 0x480 (light + EFB) |
| `0x204`   | = 0x200\|0x4    | groups pushed with 0x204 = entry + view-calc (the standard "draw this group") |
| `0x2000`  | first-substep-of-frame marker (in `unk4C`) | not a perform phase per se |
| `0x4000`  | last-substep / draw-frame marker (in `unk4C`) | gates the draw branch |
| high bits (`0x1000000`,`0x2000000`,`0x4000000`,`0x8000000`,`0x10000000`,`0x40000000`) | per-object special sub-passes | graffiti layer index/select; Mario cap/trouble drawbuf swap; indirect; player Xlu |

The `0x1000`/`0x2000` bits also have a SECOND meaning inside `testPerform` (per-object
visibility), see §2.

---

## 2. Dispatch: `TPerformList::perform` → `TViewObj::testPerform` → `perform`

File: `reference/sms/src/System/PerformList.cpp`. Address `802a4e28`
(`perform__12TPerformListFUlPQ26JDrama9TGraphics`).

```cpp
void TPerformList::perform(u32 param_1, TGraphics* g) {
    forEachPerform(begin(), end(), g, param_1);
}
void TPerformList::forEachPerform(it b, it e, TGraphics* g, u32 param_4) {
    for (it = b; it != e; ++it)
        it->unk4->testPerform(param_4 & it->unk8, g);   // <-- per-link AND mask
}
```

Each list entry is a `TPerformLink { TViewObj* unk4; u32 unk8; }` (PerformList.hpp:7).
`unk8` is the **per-membership filter mask** set at `push_back(obj, filter)`. The frame's
incoming `param_1` is **AND-ed with the link's filter** before dispatch. So the same
object can be in several lists with different filters, and only the phases that survive
`param_1 & filter` run.

`TViewObj::testPerform` (JDRViewObj.cpp:3, addr `802fcc94`):
```cpp
void TViewObj::testPerform(u32 param_1, TGraphics* g) {
    if ((param_1 & 0x1000) && unkC.check(0x1000)) param_1 &= ~0x1;  // hide-movement flag
    if ((param_1 & 0x2000) && unkC.check(0x2000)) param_1 &= ~0x1;  // hide flag #2
    param_1 &= ~unkC.get();      // unkC = per-object DISABLE mask (TFlagT<u16> at +0xC)
    if (param_1) perform(param_1, g);
}
```
So an object's `unkC` (a `TFlagT<u16>`, JDRViewObj.hpp:23) is a **disable mask**: any bit
set in `unkC` is cleared from the incoming phase mask. This is how the game hides a
group (e.g. `mConsole->unkC.off/on(0xB)` toggles the 2D console; `Group 2D`/`Guide`
unkC.on/off(0xB) in MarDirectorDirect.cpp `currentStateFinalize`). If `unkC` masks out
all the requested phases, `perform` is skipped entirely. **Effects can therefore be
present one field and absent the next purely from an `unkC` toggle, independent of which
list is re-issued.**

How a TViewObj decides CALC vs DRAW: it doesn't "decide" — the caller passes the phase
mask, the link filter narrows it, `testPerform` removes disabled bits, and the `perform`
body runs whichever `param_1 & BIT` branches survive. A calc-only invocation arrives with
0x1/0x2/0x4 set and 0x8/0x10/0x200 clear; a draw invocation arrives with 0xffffffff
(everything). The SAME perform body handles both depending on bits.

---

## 3. Perform-list membership (who is in which list, with what filter)

### 3a. `/data/PerformLists.bin` is DATA-DRIVEN (not in source)

`setupObjects()` (MarDirectorSetupObjects.cpp:406–423) loads `/data/PerformLists.bin`,
`genObject`s it, and `load`s it. `TPerformList::load` (PerformList.cpp) reads
`name\0` + `u32 filter` pairs and `push_back`s the named TViewObj with that filter
(applying `value |= 0x3000` when `value & 1`). **The membership of
`PerformList Movement / CalcAnim / GX / Silhouette / GX Post / Shine PfLst Mov / Anm`
is therefore defined by the binary file, NOT visible in the decomp.** We can see the
named lists are then fetched (MarDirectorSetupObjects.cpp:425–438) and AUGMENTED with the
hard-coded push_backs below. **Anything that draws ONLY via PerformLists.bin (e.g. the
TSilhouette object's own registration, map water in the Silhouette list, etc.) cannot be
fully enumerated from source — flagged explicitly where relevant.**

### 3b. Hard-coded augmentation in `setupObjects` (MarDirectorSetupObjects.cpp:442–477)

Pushed into **`mPerformListMovement` (0x28, a CALC list)** and **`mPerformListCalcAnim`
(0x2C, a CALC list)** — these are the lists that do NOT get re-issued on an in-between
field:

| object (JP name)            | Movement filter | CalcAnim filter | notes |
|-----------------------------|-----------------|-----------------|-------|
| `合成3` (composite3)         | 1 (line 443)    | —               | |
| `スペキュラシーン` specular sheen | 1 (line 447) | 2 (line 468) | **SPECULAR SHEEN — calc lists only here** |
| `太陽遮蔽物グロー` sun-occ glow (TLensGlow) | 1 (line 455) | 2 (line 471) | movement+anim here; DRAW is in initECDisp/GXPost |
| `レンズフレア` lens flare (TLensFlare) | 1 (line 456) | 2 (line 470) | movement+anim here; DRAW is in initECDisp/GXPost |
| `会話カーソル` dialogue cursor | 1 (line 464)   | 2 (464/474)     | |
| `ターゲット矢印` target arrow | —               | 2 (line 475)    | |

Then `mPerformListGXPost->push_back(drawInit, 0x100)` (line 477) and
`preEntry(unk34)` (line 478), `setup2()` (line 479).

### 3c. `preEntry(unk34)` — the MAIN 3D SCENE list (MarDirectorPreEntry.cpp:14–66)

`unk34` (0x34). Performed with `0xffffffff` in the branch-A draw tail (direct.cpp:163).
This is the meat of the rendered frame:

| pushed object              | filter      | what it is |
|----------------------------|-------------|-----------|
| `camera 1`                 | 0x10        | view setup |
| `J3D System Set View Mtx`  | 0x4         | view-mtx upload |
| `DrawBuf Sky Opa/Xlu`      | 0x480       | sky drawbufs |
| `空グループ` sky group       | 0x204       | sky entry+viewcalc |
| `DrawBuf MapOpa/MapXlu`    | 0x480       | map drawbufs |
| `マップグループ` map group   | 0x204       | map entry |
| `鏡表示モデル管理` mirror-display ctrl | 0x204 | |
| `DrawBuf Map 半透明優先 (opa/xlu)` | 0x480 | priority-xlu map drawbufs |
| `マップ` map                | 0x4000200   | map (special pass A) |
| `DrawBuf Map 半透明優先2 (opa/xlu)`| 0x480 | |
| `マップ` map                | 0x2000200   | map (special pass B) |
| `DrawBuf Graffito`         | 0x480       | graffiti drawbuf |
| `落書きグループ` graffiti grp | 0x204       | graffiti entry |
| `ライトマネージャー` light mgr | 0x400      | light buffer |
| `DrawBuf ChrOpa/ChrXlu`    | 0x480       | character drawbufs |
| `マネージャーグループ` manager grp | 0x204  | object managers |
| `コンダクター` conductor     | 0x204       | enemy/actor conductor entry |
| `vp WParticle 2` viewport  | 0x8         | |
| `camera 1`                 | 0x10        | |
| **`水マネージャ` water mgr (TModelWaterManager)** | **0x4** | **Mario's spray water FX — VIEW-CALC ONLY (0x4) in this list** |
| `水飛沫マネージャ` splash mgr | 0x4        | splash — view-calc only here |
| `クエッションマネージャ` question mgr | 0x4 | |
| `DrawBuf Indirect` (if indirect) | 0x480 | |
| `インダイレクトシーン` indirect sheen | 0x40000204 | indirect/heat-haze |
| `camera 1`                 | 0x10        | |
| `J3D System Set View Mtx`  | 0x4         | |
| `プレーヤーグループ` player group | 0x10000000 | Mario cap/trouble drawbuf swap (MarioMain 0x10000000) |
| `プレーヤーグループ` player group | 0x204     | Mario main entry (move/anim already done in calc lists; 0x204 = entry+viewcalc) |
| `プレーヤーグループ` player group | 0x8000000  | Mario player Xlu pass |

> KEY: `水マネージャ` is pushed here with filter **0x4 only**. So in the `unk34`
> 0xffffffff pass, the water manager runs only its `param_1 & 4` branch =
> `calcDrawVtx` + `calcVMAll` (ModelWaterManager.cpp:1550–1559) — building the per-
> particle matrices. Its ACTUAL draw (`drawSilhouette`/`drawWaterVolume` under 0x8,
> `drawRefracAndSpec` under 0x80) is NOT triggered from `unk34`. Those must come from a
> different list/filter — almost certainly a `PerformLists.bin` entry for `水マネージャ`
> in the GX / Silhouette / GXPost lists with 0x8 / 0x80 set. **Could not confirm the 0x8
> and 0x80 water-draw membership from source (data-driven). Flagged.**

### 3d. `initECDisp(mPerformListGXPost, …)` — post / 2D / lens-flare-chr / HUD
File MarDirectorInitECT.cpp:98–218. Targets **`mPerformListGXPost` (0x24)**, performed
0xffffffff in branch B (direct.cpp:176). Highlights:

| object                       | filter | phase |
|------------------------------|--------|-------|
| `stageDisp` (TEfbCtrlDisp)   | 0x80   | EFB copy/display |
| `Screen 2D` viewport         | 0x8    | |
| `合成3` composite3            | 0x8    | |
| `DrawBuf LensFlare`          | 0x480  | lens-flare drawbuf |
| `スペキュラシーン` specular sheen | 0x204 | **specular sheen DRAW (entry) — IF present** |
| `太陽遮蔽物グロー` lens glow   | 0x204  | **sun-occlusion glow DRAW (entry)** |
| `SMS Draw Init`              | 0x8    | |
| `DrawBuf LensFlare`          | 0x8    | flush |
| `camera 1`, set-view-mtx     | 0x10/0x4 | |
| `レンズフレア` lens flare      | 0x204  | **lens flare DRAW (entry)** |
| `DrawBuf ChrOpa/ChrXlu`      | 0x480  | character drawbufs (HUD chars) |
| `会話カーソル` dialogue cursor | 0x204  | |
| `ターゲット矢印` target arrow  | 0x204  | |
| `Group 2D 2`, `Group 2D`, `Guide` | 0x8 | 2D overlays |
| `stageDisp`                  | 0x8    | final EFB display |

So **specular sheen / lens glow / lens flare**: their MOVEMENT (0x1) + CALC-ANIM (0x2)
run in the calc lists (§3b), and their DRAW (0x204 = entry) runs in **mPerformListGXPost**
(a re-issued draw list, branch B). Their geometry is therefore re-issued on an
in-between field, but the **view-dependent state they computed in 0x1/0x2 (screen
position of sun, hidden-ratio fade, material TEV alpha — lensglow.cpp:93+, lensflare.cpp:42+)
is NOT recomputed**, so on the in-between field they re-draw with the previous field's
position/alpha. Visible result: stale (not missing) flare/glow — a 1-field lag, not a
flicker. The marukage and water are different (below).

### 3e. `initECTMir(mPerformListGX, …)` — mirror pass (MarDirectorInitECT.cpp:75–94)
Targets **`mPerformListGX` (0x1C)** (the MIRROR list, performed 0xffffffff branch B,
direct.cpp:171). Sets up `鏡描画ステージ` (mirror EFB tex) + `鏡カメラ` (mirror camera).
The mirrored scene geometry membership is again **data-driven (PerformLists.bin)** —
not enumerable from source.

### 3f. `initECTGft(unk38, unk3C, …)` — graffiti EFB readback (MarDirectorInitECT.cpp:19–73)
Targets **`unk38`/`unk3C`** (graffiti passes, performed 0xffffffff branch B,
direct.cpp:169–170). EFB-tex readback of pollution/graffiti layers (TEfbCtrlTex 0x80,
viewport 0x8, ortho 0x10, graffitiGroup with per-layer index in high bits). Pure EFB
plumbing for the dirt/graffiti system.

### 3g. `setupPerformList_console(unk30)` — 2D group (MarDirectorInitECT.cpp:222–233)
Targets **`unk30` (0x30)**, performed EVERY calc sub-step with `~uVar44`
(direct.cpp:152). Pushes `Group 2D` (+ a `TEmitterViewObj`), `Group 2D 2`, `Guide`, each
filter 3 (=0x1|0x2 → movement+calc-anim). This is the 2D HUD/particle **calc**; its DRAW
is in mPerformListGXPost (§3d). `unk30` is a CALC list — NOT re-issued on in-between.

### 3h. `unk40` (0x40) — pre-pass
Built in setupObjects (line 383): `unk40->push_back(drawBufferGroup, 8)`. Plus DrawBuf
Sky entries via the light manager (`gpLightManager->addChildGroupObj(drawBufferGroup)`).
Performed 0xffffffff branch B (direct.cpp:168). Light/drawbuffer pre-pass.

---

## 4. TSilhouette (the marukage round shadow) — DrawUtil.cpp:93

`TSilhouette::perform(param_1, g)` does, per bit:
- `0x1` (movement): chase Mario's occluded alpha `unk48` (DrawUtil.cpp:95–99). **CALC.**
- `0x8` (draw): silhouette lighting setup `setting()` (101–107). DRAW.
- `0x80` (post): sun-pass lighting setup (109–113). DRAW.
- `0x10` (draw, 2nd pass): **the actual marukage round-shadow GX draw** — loads
  `H_marukage_xlu_i8.bti` (the round shadow texture, set in `loadAfter` DrawUtil.cpp:61),
  builds the projector matrix from `gpMarioPos`, draws via 2-stage TEV
  (DrawUtil.cpp:114–168). DRAW.

`gpSilhouetteManager` is this object. Its list membership is **data-driven
(PerformLists.bin)** — almost certainly in `mPerformListSilhouette` (0x20) and/or
`mPerformListGXPost`. `direct()` re-issues `mPerformListSilhouette` only when
`gpSilhouetteManager->unk48 > 0 || gpCamera->unk2C8 != -1` (direct.cpp:172–175). `unk48`
(the occluded-alpha) is updated ONLY in the **0x1 movement phase** (DrawUtil.cpp:95–99),
which runs in the calc lists, NOT in the re-issued draw branch.

> **The marukage's `0x1` movement-phase alpha chase is a CALC-list update.** On an
> in-between field that skips the calc lists, `unk48` is not advanced. The DRAW of the
> shadow (0x10) uses `gpMarioPos` for its matrix — if the interp path also doesn't
> update `gpMarioPos` for the in-between, the shadow re-draws at the previous position.
> And the re-issue **gating condition** itself (`unk48 > 0`) is read from a value the
> in-between never refreshed. If `unk48` was 0 on the real field, the silhouette list is
> never re-issued at all on the in-between → shadow simply absent that field = flicker.

Note also `TModelWaterManager::drawSilhouette` (ModelWaterManager.cpp:920) and the map
water draw the silhouette/water-shadow under the SAME `gpSilhouetteManager->unk48` gate
(it early-returns if `!isUnk48Positive()`, line 922). So water-surface silhouette shadow
shares the marukage's fate.

---

## 5. Water — where each water draw actually happens

`TModelWaterManager::perform` (ModelWaterManager.cpp:1541), bits:
- `0x1` move: `move()` + `calcWorldMinMax()` — particle physics. **CALC.**
- `0x4` view-calc: `calcDrawVtx` + `calcVMAll` — build per-particle matrices. **CALC/VIEW.**
  This is the bit set in `preEntry(unk34)` (filter 0x4, §3c).
- `0x8` draw: `drawSilhouette` + `drawWaterVolume` + (cond) `drawMirror` + shine-shadow.
- `0x80` post: `drawRefracAndSpec` + (cond) shine-shadow-volume. **The refraction/specular
  water-surface effect.**

So the water DRAW (0x8 silhouette/volume, 0x80 refrac+spec) is NOT in `unk34` (which has
it at 0x4 only). The draw membership for `水マネージャ` at 0x8/0x80 must be a
**PerformLists.bin** entry in the GX/Silhouette/GXPost lists — **not confirmable from
source.** The `0x4` matrix-build it DOES get in `unk34` is gated behind nothing but
needs `move()`'s output from the `0x1` calc step. If the in-between skips `0x1` (calc),
the particle positions/lifetimes are stale, but `calcVMAll` still runs on stale data →
water particles freeze for one field (not missing — frozen). The `drawRefracAndSpec`
(0x80) screen-space refraction reads the screen texture (`スクリーンテクスチャ`,
ModelWaterManager.cpp:194) captured that field; if the EFB-capture pass (stageDisp 0x80
in GXPost) is re-issued but the underlying scene differs, the refraction can sample a
mismatched capture.

---

## 6. CONCLUSION — what an in-between (draw-only) field misses / double-processes

Definitions for this section:
- **Re-issued on in-between** = the draw-branch lists: `unk40`(0x40), `unk38`/`unk3C`
  (0x38/0x3C), `mPerformListGX`(0x1C mirror), `mPerformListSilhouette`(0x20, conditional),
  `mPerformListGXPost`(0x24), and the main scene `unk34`(0x34). (Confirm the interp path
  actually re-issues `unk34`; if it doesn't, the whole scene is missing, see §0 warning.)
- **NOT re-issued (calc only)** = `unk30`(0x30 2D calc), `mPerformListMovement`(0x28),
  `mPerformListCalcAnim`(0x2C), `mShinePfLstMov`(0x44), `mShinePfLstAnm`(0x48), plus the
  in-line `movement()` (direct.cpp:153) and the `0x1`/`0x2`/`0x4` calc branches inside
  every object's `perform`.

### MISSING or STALE on an in-between field

1. **Marukage round shadow (TSilhouette, DrawUtil.cpp:93–168, draw bit 0x10).**
   - Its occluded-alpha `unk48` is advanced ONLY in the `0x1` movement phase (calc list).
   - The re-issue **gate** for `mPerformListSilhouette` (`unk48 > 0`, direct.cpp:172) reads
     that un-refreshed value. If `unk48` is at/near 0 on alternating evaluations, the
     silhouette list is conditionally skipped → shadow blinks in/out = the reported
     flicker. Even when re-issued, the projector matrix uses `gpMarioPos`; without an
     interpolated Mario position the shadow lags. **Most likely root of the "Mario's
     shadow" flicker.**

2. **Water-surface silhouette / water-shadow** (`TModelWaterManager::drawSilhouette`,
   ModelWaterManager.cpp:920; map water under the same `gpSilhouetteManager->unk48` gate,
   line 922). Shares the marukage's `unk48` gate — same blink. **Most likely root of the
   "water surface effects" flicker.**

3. **Water particle physics frozen** (`move()`, bit 0x1, ModelWaterManager.cpp:1545–1549).
   Not strictly "missing", but the in-between re-draws the SAME particle set/positions →
   spray water FX stutters at 30fps inside a 60fps scene. `calcVMAll` (0x4 in `unk34`)
   re-runs on stale particle data.

4. **Water refraction/specular** (`drawRefracAndSpec`, bit 0x80). If its draw membership
   (data-driven, unconfirmed §5) lands in a re-issued list it re-draws, but it samples
   `スクリーンテクスチャ` (the EFB capture). A mismatch between the in-between's capture
   and the re-issued geometry can produce a 1-field shimmer.

5. **Lens flare / sun-occlusion glow / specular sheen** — DRAW (0x204) is re-issued via
   `mPerformListGXPost`/initECDisp, but the screen-space sun position, hidden-ratio fade,
   and TEV material alpha are computed in `0x1`/`0x2` (calc lists, lensglow.cpp:93+,
   lensflare.cpp:42+, §3b). On the in-between they re-draw with **last field's** position
   and alpha → 1-field lag (stale, not absent). Lower-severity than the shadow blink.

6. **2D HUD / dialogue / particle-emitter calc** (`unk30`, filter 3 = 0x1|0x2; emitter
   `TEmitterViewObj`). Calc-only list; DRAW is in GXPost (re-issued). HUD geometry
   re-draws but its animation/emission doesn't advance on the in-between (acceptable; HUD
   is 2D and usually static between fields).

7. **Shine-sprite (star) movement/anim** (`mShinePfLstMov` 0x44, `mShinePfLstAnm` 0x48):
   calc-only. Star sprites freeze for one field. Note the special alt-frame masking on
   `mShinePfLstMov` (direct.cpp:141/143, bits 0x100/0x200 toggled by `unk58&1/2`) — the
   shine list is ALREADY interlaced across fields by the original game, so naive 60fps
   re-issue here is especially fragile.

### DOUBLE-PROCESSED on an in-between field

8. **Any draw-branch object whose `perform` body ALSO does calc work under a draw bit.**
   The clearest case: **`水マネージャ` at filter 0x4 in `unk34`** — `unk34` is re-issued
   with 0xffffffff each draw, so `calcVMAll` (matrix rebuild) runs **again** on the
   in-between even though particle physics (`move()`, 0x1) did not. Not harmful per se
   (idempotent on stale data) but it IS redundant compute, and if `gpMarioPos`/view-mtx
   were interpolated for the in-between it would rebuild matrices to the interpolated pose
   — which is actually what you WANT for interpolation, so this one is benign-to-helpful.

9. **`drawInit`/`SMS Draw Init`/EFB-control objects** appear multiple times across the
   re-issued lists (GXPost pushes `stageDisp` at both 0x80 and 0x8; preEntry/initEC push
   `camera 1` and `J3D System Set View Mtx` repeatedly). Re-issuing the whole draw set
   re-runs all these GX state resets — correct (they're meant to run each draw) but be
   aware the in-between pays full GX-state-setup cost, not just geometry.

### Honest gaps (could NOT determine from source)

- The exact membership/filters of the **named** lists `PerformList GX / Silhouette /
  GX Post / Movement / CalcAnim / Shine PfLst Mov / Anm` are in **`/data/PerformLists.bin`**
  (binary), augmented but not fully defined in source. In particular: **where TSilhouette,
  `水マネージャ`'s 0x8/0x80 draw, and the map-water draw are registered** is data-driven
  and unconfirmed. To resolve, dump `/data/PerformLists.bin` at runtime (it is `load`ed in
  setupObjects.cpp:406–423; each entry is `name\0` + `u32 filter`) and list the entries —
  that is the only way to get ground truth for the data-driven half.
- `direct()` itself is not separately symbolized in `sms_gmse01_funcs.txt`; verify the
  runtime split (which lists the interp override actually re-issues) against
  `runtime/overrides/interp_capture.cpp` rather than assuming the names above.

### Recommended next probe
Add a one-shot dump of `/data/PerformLists.bin` (or walk each `TPerformList`'s
`TSingleLinkList<TPerformLink>` at runtime printing `unk4`'s TNameRef name + `unk8`
filter) to fill gaps #1. That converts every "data-driven, unconfirmed" above into fact,
and will definitively show which list TSilhouette and the water draws live in — the two
effects the user reports flickering.

# Mario's cast shadow — the EFB destination-alpha stencil chain (60 fps blink, deeper RE)

Scope: this continues `real_shadow_bindmgr.md`. That doc pinned the blink to the
**request/draw-array** being empty on the in-between field and proposed restoring the built
draw array (`this+0x1C` / count `this+0x20`). **That fix was implemented
(`runtime/overrides/shadow_interp.cpp`) and DID NOT stop the blink.** This doc REs the actual
*render technique* of the cast shadow (which `real_shadow_bindmgr.md` never characterised — it
only RE'd the data plumbing) and explains why a geometry-only restore is insufficient.

> Source caveat: `reference/sms/src/MarioUtil/ShadowUtil.cpp` is empty in the decomp. Everything
> below is from GMSE01 disassembly via
> `./build/sunbright-recomp "$SUNBRIGHT_ROM" --disasm <addr> <n>`, cross-checked against
> `reference/sms_gmse01_funcs.txt`, the class header, and `MarDirectorDirect.cpp` / `JDREfbCtrl.cpp`.

---

## 0. TL;DR

- The cast shadow is **NOT** a projected/copied texture. There is **no `GXCopyTex`,
  `GXSetTexCopySrc/Dst`, or texture load anywhere in the entire chain** (verified: grep over
  `drawShadow` + `drawShadowGD` + `drawShadowVolume` resolves zero copy/texcopy calls).
- It is a classic **EFB destination-alpha "stencil shadow volume"**: render the shadow VOLUME
  with **color writes OFF / alpha writes ON** to stamp a per-pixel mask into the **EFB alpha
  channel**, then composite a **screen-space dark quad** with the **destination-alpha test
  enabled** so it darkens *only* the pixels the volume marked. The "dynamic texture on top of an
  existing object" in the user's hint **is the EFB alpha channel** (the dynamic resource) read
  back by the composite quad to darken the ground/Mario geometry already sitting in the EFB.
- Both `drawShadow` (immediate `GXBegin`) and `drawShadowGD` (`GXCallDisplayList` of pre-built
  state lists) implement the SAME multi-pass stencil; `drawShadowGD` is the path SMS uses
  (`[r13-0x60f7]` true).
- The effect is **single-frame and self-contained in the EFB**: ReInitializeGX at the top, full
  GX state set internally, the stencil is written and consumed before the frame's EFB→XFB copy.
- **Why the geometry-restore failed:** the shadow's visibility is a function of the **EFB pass
  ORDER**, not just the geometry. It must run *in the +0x20 silhouette list slot — after the
  +0x1C 3D scene fills EFB color/depth/alpha, and BEFORE the +0x24 GXPost list issues the
  EFB→XFB copy and (re)programs color/alpha/Z update masks*. The existing override (a) suppresses
  the in-between's whole +0x20 perform and (b) replays the shadow with an **explicit
  `sb_interp60_draw_shadow` call that fires AFTER all lists including +0x24**. Running it after
  +0x24 means: the GXPost `TEfbCtrlDisp::perform &0x80` already reprogrammed
  `GXSetColorUpdate/AlphaUpdate/ZMode` and set the display rect, and (worse) the in-between's
  copy path may already have consumed the EFB — so the stencil/composite the replay draws is out
  of phase with the EFB state it assumes. The fix is to draw the shadow **in order, at the +0x20
  slot**, not as a trailing replay.

---

## 1. The render chain & addresses (GMSE01)

| symbol | address |
|---|---|
| `TMBindShadowManager::perform(u32, TGraphics*)` | `0x80231108` |
| `TMBindShadowManager::drawShadow(u32, TGraphics*)`   (immediate path) | `0x8022f014` |
| `TMBindShadowManager::drawShadowGD(u32, TGraphics*)` (GD path SMS uses) | `0x8022fa40` |
| `TMBindShadowManager::drawShadowVolume(bool, TAlphaShadowQuad*)` | `0x802305dc` |
| `TMBindShadowManager::calcVtx()` | `0x8022e0cc` |
| `conectCubeSame(TAlphaShadowBlendQuad*, TAlphaShadowBlendQuad*)` | `0x80230e68` |
| `conectCubeDiffer(...)` | `0x80230fac` |
| `ReInitializeGX()` | `0x80233760` |
| `SMS_DrawCube(...)` | `0x80225d00` |
| `SMS_SettingDrawShape / SMS_DrawShape` | `0x80225c94` / `0x80225c30` |
| `gpBindShadowManager` | `*(0x8040E0C0)` (= `[r13-0x6100]`) |
| GD-path select flag `[r13-0x60f7]` (≠0 → GD) | global |
| GD-list-built once flags `[r13-0x60f6]` / `[r13-0x60f5]` | globals |
| static GD state lists | `0x803eb1c4` / `0x803eb1d4` / `0x803ebb40` |
| view matrix used by `calcVtx` | `0x804045dc` (j3dSys view) |

GX setter addresses seen in the chain (resolved from the `bl` targets):
`GXSetZCompLoc 0x80361fcc`, `GXSetColorUpdate 0x80361ed4`, `GXSetAlphaUpdate 0x80361f14`,
`GXSetDstAlpha 0x8036215c`, `GXSetZMode 0x80361f54`, `GXSetBlendMode 0x80361dd0`,
`GXSetCullMode 0x8035e210`, `GXBegin 0x8035df88`, `GXCallDisplayList 0x80362a50`.

---

## 2. The technique, pass by pass (RE'd from `drawShadow` 0x8022f014)

`drawShadow` begins with `ReInitializeGX()` (0x80233760) — a FULL pipeline reset (NumChans/
TexGens/TevStages, blend, color/alpha update, texobjs). So the shadow draw does **not** depend
on any inherited GX state; it builds its own. Then `GXSetZCompLoc(GX_TRUE)`.

Decoded `li`→`bl` argument pairs (the immediate constants feed the very next `bl`):

### Pass 1 — stamp the shadow VOLUME into EFB alpha (color writes OFF)
```
0x8022f10c  li r3,1   -> GXSetAlphaUpdate(GX_TRUE)        // EFB alpha WRITES on
0x8022f194  li r3,0   -> GXSetColorUpdate(GX_FALSE)       // EFB color WRITES off  <-- key
0x8022f19c  li r3,1; r4,0 -> GXSetDstAlpha(GX_ENABLE, 0)  // force dst-alpha = 0 baseline
0x8022f1a8  li r3,1; r4,7; r5,0 -> GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE)  // z-test, NO z-write
0x8022f1b8  li r3,1; r4,1; r5,1; r6,5 -> GXSetBlendMode(GX_BM_BLEND, …, GX_LO_…)
            ... GXLoadPosMtxImm; drawShadowVolume(0x802305dc) / SMS_DrawCube(0x80225d00) ...
```
The volume geometry is rendered with **color update disabled and alpha update enabled**, with a
z-test (against the scene depth already in EFB) but no z-write. Front faces / back faces of the
extruded silhouette toggle the EFB alpha bit where the shadow volume covers the scene — a
**z-pass/z-fail destination-alpha stencil**. (There are multiple `GXSetColorUpdate(0)/Dst
Alpha` volume sub-passes at 0x8022f2..0x8022f6xx for the front/back/cap faces and the
`TAlphaShadowQuad` / `TAlphaShadowBlendQuad` connective quads — `conectCubeSame/Differ` join
adjacent silhouette segments. All write only alpha.)

### Pass 2 — composite a dark screen-space quad gated by the EFB alpha stencil
```
0x8022f978  li r3,1   -> GXSetColorUpdate(GX_TRUE)        // EFB color writes back ON
0x8022f980  li r3,1   -> GXSetAlphaUpdate(GX_TRUE)
0x8022f984  li r3,1; r4,0 -> GXSetDstAlpha(GX_ENABLE, 0)  // dst-alpha TEST against the stencil
0x8022f9a8  li r3,1; r4,6; r5,7; r6,5
            -> GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA?, GX_BL_INVDSTALPHA?, …)
0x8022f9b8  li r3,0x80; r4,0; r5,4 -> GXBegin(GX_QUADS, GX_VTXFMT0, 4)  // one screen quad
            (z-mode inherited from the volume pass: z-test on, no write)
            vertex colors/positions from float consts at 0x….e9d0/e9d4/e9d8
```
The composite quad covers the shadow's screen footprint and blends a darkening color into the
EFB **only where the volume pass set EFB alpha** (dst-alpha gating). The "existing object" the
shadow lands on is whatever scene geometry already occupies those EFB pixels (ground, walls,
Mario). This is the dynamic-resource feedback the user's hint refers to: **the EFB alpha channel
is written this frame and read back this frame.**

`drawShadowGD` (0x8022fa40) is the same protocol but the state-setting GX commands are captured
ONCE into the static GD display lists (flags `[r13-0x60f6]`/`[r13-0x60f5]`, buffers
`0x803eb1xx`/`0x803ebb40`) and replayed each frame with `GXCallDisplayList`; its per-frame
geometry loop still iterates `this+0x1C` for `this+0x20` entries (stride 0x14) — verified at
`0x80230348: lwz r0,[r27+0x20]; cmp r23,r0; bc` — interleaving `drawShadowVolume` calls.

---

## 3. Draw ORDER is the load-bearing fact (the real reason it blinks)

`TMarDirector::direct` Branch B (real frame, `MarDirectorDirect.cpp:166-178`) runs the draw
lists in a fixed order:
```
unk40 (+0x40); unk38 (+0x38); unk3C (+0x3C);
mPerformListGX        (+0x1C)  -> 3D scene fills EFB color + depth + alpha
mPerformListSilhouette(+0x20)  -> TMBindShadowManager::perform -> drawShadowGD
                                  (Pass 1 stamps EFB alpha, Pass 2 composites the dark quad)
                                  (gated: gpSilhouetteManager->unk48>0 || gpCamera->unk2C8!=-1)
mPerformListGXPost    (+0x24)  -> TEfbCtrlDisp::perform:
                                   &0x80: IssueGXPixelFormatSetting + GXSetColorUpdate/
                                          AlphaUpdate/ZMode + setDisplayRect (REPROGRAMS the
                                          update masks the shadow relied on)
                                   &0x08: IssueGXCopyDisp(EFB -> XFB)   (consumes the EFB)
GXInvalidateTexAll()
```

So the cast shadow MUST be composited **after +0x1C (so EFB has the scene) and before +0x24
(which reprograms the update masks and copies EFB→XFB)**. Its visibility is a property of *the
EFB at that exact phase*, not of the geometry alone.

What the current `interp60` in-between does (`interp_redraw.cpp`):
1. Re-issues `{0x40,0x38,0x3C,0x1C,0x20,0x24}`. For +0x20, `shadow_interp.cpp`'s perform
   override **early-returns** when `g_interp60_in_redraw` — so the shadow does **NOT** draw at
   its correct +0x20 slot (between scene and GXPost).
2. After the whole list loop (i.e. **after +0x24 already ran** `TEfbCtrlDisp::perform`,
   reprogramming color/alpha/Z update and issuing the EFB copy), it calls
   `sb_interp60_draw_shadow`, which restores `+0x20` and calls `perform &8` (drawShadowGD).

The trailing replay therefore stamps and composites the EFB-alpha stencil **out of phase**: the
+0x24 list already (a) changed `GXSetColorUpdate/AlphaUpdate/ZMode` via `TEfbCtrl::perform &0x80`
and (b) issued `GXCopyDisp` of the EFB to XFB. Even though `drawShadow`/`drawShadowGD` calls
`ReInitializeGX` and sets its own blend/update state, the **EFB content it composites into and
the copy-out it should precede are no longer aligned** with a normal frame's +0x20 phase — the
shadow lands on an EFB that's already been (or is about to be) copied with the in-between's
distinct ALT XFB steering in `endRendering`. Net: the in-between's shadow either composites into
an EFB that won't be presented as expected, or its alpha stencil interacts with the GXPost
pixel-format/update reprogramming → it does not appear → on/off blink.

This also explains why **restoring the geometry array did nothing**: the geometry was never the
missing ingredient at the trailing-replay site; the *phase* was.

---

## 4. The concrete, faithful fix

Draw the cast shadow **in its real +0x20 slot on the in-between**, exactly as a real frame does,
instead of suppressing it and replaying it after +0x24.

### 4a. What the in-between must do
- **Do NOT suppress the +0x20 silhouette list perform.** Let `mPerformListSilhouette` run.
- For the bind-shadow manager's perform during the in-between, pass a mask that **draws but does
  not rebuild or finalize**:
  - keep `&0x8`  (DRAW → drawShadowGD: stamp + composite),
  - drop `&0x4`  (RESET/`calcVtx` → would rebuild the draw array to empty, since no requests
    were queued at 60 Hz),
  - drop `&0x20000000` (FINALIZE → would zero `+0x14/+0x20/+0x40` and discard the array we want
    to keep for the next real-field's own cycle — and we must not perturb the request bookkeeping).
  Concretely, intercept `TMBindShadowManager::perform` (0x80231108): when `g_interp60_in_redraw`,
  `cpu.gpr[4] = (cpu.gpr[4] & ~0x4u & ~0x20000000u) | 0x8u;` and ensure `+0x20` (drawCount)
  holds the real field's captured value before calling the real perform (capture it in a tee on
  `drawShadowGD` on the real field, as the existing code already does for `g_draw_count`).
- **Remove the trailing `sb_interp60_draw_shadow` replay** (or make it a no-op when the in-list
  draw is active) — the shadow is now drawn at the correct phase by the list itself.

This makes the in-between's EFB pass order identical to a real frame:
`…+0x1C scene → +0x20 shadow stencil+composite → +0x24 GXPost copy`, so the dst-alpha stencil is
written and consumed against the right EFB, before the copy, every field.

### 4b. Position interpolation (polish, after the on/off fix)
`calcVtx` is skipped on the in-between (we drop `&0x4`), so the drawn geometry holds the real
field's positions for both fields. The drop shadow's silhouette is built from
`TCircleShadowRequest.unk0` world positions transformed by the j3dSys view (`0x804045dc`).
`interp_redraw.cpp` already blends `gpMarioPos` and the j3dSys view matrix during the redraw, but
because we skip `calcVtx`, the *built* shadow geometry won't follow that blend. Two faithful
options once the blink is fixed:
- (preferred, lower risk) leave geometry at tick N for both fields — the cast shadow held one
  field is far less visible than the on/off blink; OR
- re-run only `calcVtx` on the in-between (pass `&0x4`) so it rebuilds the silhouette against the
  **blended** view + blended `gpMarioPos` — but this only works if the request array is also
  preserved (it is request-driven; with no requests it rebuilds empty). That requires snapshotting
  the request array (`+0x10`/`+0x14`, stride 0x24) too — the (a) option in `real_shadow_bindmgr.md`
  §4 — and is more invasive. Ship 4a first.

### 4c. Gating note
The real-field +0x20 list only runs when `gpSilhouetteManager->unk48 > 0 || gpCamera->unk2C8 != -1`
(`MarDirectorDirect.cpp:172-173`). The in-between re-issues +0x20 unconditionally; that's fine —
if the gate is false the manager simply has nothing requested, and with `&0x4` dropped it won't
rebuild. No extra gating needed.

---

## 5. Why this is faithful, not a bandaid
It reproduces the game's own per-frame shadow pass at the game's own draw-order phase, with the
same EFB destination-alpha stencil technique, differing only in suppressing the in-between's
RESET/FINALIZE (which exist to recycle a 30 Hz request queue we intentionally do not refill at
60 Hz). No fabricated geometry, no magic constants, no texture hacks. It corrects a *phase* error
in the existing override (drawing after the EFB copy) — the same class of fix as keeping the
other draw lists in order.

---

## 6. Verification plan (user, headed)
- `SUNBRIGHT_INTERP60=1` + the new override (default-OFF env flag, see
  `runtime/overrides/cast_shadow_interp.cpp`) toggled on: the cast shadow is present on BOTH
  fields (no on/off), at 30 Hz position (held), correct shape/darkness.
- A/B: flag off → blink returns; oracle (`SUNBRIGHT_DISABLE_RECOMP`, 30 fps) for shape/darkness
  parity.
- If 4b polish is added later: shadow position tracks Mario smoothly at 60 Hz.

### Negative results (do NOT re-try)
- Restoring/rebuilding the draw geometry array (`this+0x1C` / count `this+0x20`) — DONE in
  `shadow_interp.cpp`, did NOT fix the blink. The geometry was never the missing ingredient at
  the trailing-replay site.
- Forcing immediate (`drawShadow`) vs GD (`drawShadowGD`) path — neither changed the blink; both
  implement the same out-of-phase stencil when replayed after +0x24.
- There is NO `GXCopyTex` / dynamic texture in the chain — do not go looking for a copied
  shadow texture to preserve; the dynamic resource is the EFB alpha channel, consumed in-frame.

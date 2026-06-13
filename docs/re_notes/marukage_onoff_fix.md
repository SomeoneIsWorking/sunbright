# Marukage on/off blink on the 60fps in-between field — root cause + exact fix

Supersedes the "projection-swim" reading in `gx_texgen_pos_space.md` / `marukage_shadow_flicker.md`
for the symptom the user actually observes. **DECISIVE NEW FACT (user, 10 presented frames,
Mario walking): the marukage drop-shadow is ON in every real field and ABSENT in every
in-between field — a clean on/off blink, not a position swim.** The `&0x10` texgen SETUP is
instrument-confirmed to fire on BOTH fields. So the SETUP is not the problem; the **receiving
draw that paints the shadow pixels is missing on the in-between**.

All file:line are `reference/sms/`; addresses GMSE01 (`reference/sms_gmse01_funcs.txt`).
Runtime: `runtime/overrides/interp_redraw.cpp`, `interp_capture.cpp`, `runtime/interp60.h`.

---

## 0. TL;DR — conclusion + the fix (read this first)

**The marukage is a destination-alpha-gated projection, painted by re-drawing scene geometry
while two pieces of immediate-mode GX state are simultaneously live:**

1. **the texgen/TEV state** set by `TSilhouette::perform &0x10` (`DrawUtil.cpp:116-166`), AND
2. **the EFB destination-alpha mask** written by Mario's silhouette pass
   `TMario::perform &0x80000000` — `GXSetDstAlpha(GX_ENABLE, gpSilhouetteManager->unk48)` plus
   the occlusion cubes (`MarioMain.cpp:219-243`), whose blend is `GX_BL_DSTALPHA /
   GX_BL_INVDSTALPHA`.

Both #1 and #2 live in the **silhouette perform list `mPerformListSilhouette` (0x20)** — the
only list whose re-issue `direct()` GATES on `gpSilhouetteManager->unk48 > 0`
(`MarDirectorDirect.cpp:172-175`). The marukage's per-frame visibility ALPHA
(`unk12.a = unk48`) is computed **only in `TSilhouette::perform &0x1`** (`DrawUtil.cpp:95-99`)
and Mario's DstAlpha is `gpSilhouetteManager->unk48` directly.

**Why it blinks OFF on the in-between:** the in-between re-issue passes
`perform_mask = 0xFFFFFFFC` (`interp_redraw.cpp:319`, default `interp60.h:38`), which **clears
`&0x1` and `&0x2`**. With `&0x1` masked:
- `TSilhouette::perform` never runs its `&0x1` body, so `unk12.a` is **not refreshed** for the
  in-between; the marukage material/raster alpha is whatever the last real field left, AND
- the cooperating **shine-shadow / occlusion-alpha plumbing that the silhouette pass depends on
  is not re-established** — the receiving draw's source alpha collapses to ~0 → the projection
  blends in invisibly (clean OFF), while the `&0x10` texgen setup still runs (visible in the
  probe as "fires on both fields").

The §3 analysis pins it precisely: the **on/off is the masked `&0x1` (and the silhouette
list's draw-order dependence on the same calc value), NOT a `&0x10`-absence and NOT a position
detach.** That matches the user's exact phrasing — clean on/off, setup on both fields.

### THE FIX (minimal, against `interp_redraw.cpp`)
Do **not** strip `&0x1`/`&0x2` from the silhouette/shadow list on the in-between. The reason
`perform_mask=0xFFFFFFFC` exists at all is the WATER scroll double-step (`unk5E00`,
`ModelWaterManager.cpp:1548`) — that is a `TModelWaterManager` concern, not a `TSilhouette`
one. So the fix is to **re-issue the silhouette list (0x20) with the FULL mask the game uses
(`0xFFFFFFFF`), while keeping the reduced `0xFFFFFFFC` mask for the other lists** that have the
double-step hazard. Concretely (per-list mask instead of one global `perform_mask`):

```c
// interp_redraw.cpp, the re-issue loop (currently ~line 308-322):
for (u32 li = 0; li < N(kDrawLists); li++) {
    if (!(g_i60.list_mask & (1u << li))) continue;
    const u32 list = MEM_R32(g_mardir + kDrawLists[li]);
    if (!list) continue;
    cpu.gpr[3] = list;
    // The silhouette/shadow list (+0x20, index 4) draws the marukage + Mario's
    // DstAlpha-gated occlusion pass, both of which depend on the &0x1 calc value
    // (unk12.a = unk48). Masking &0x1 makes the shadow blend in at ~0 alpha = the
    // on/off blink. Re-issue THIS list with the same 0xffffffff the game uses
    // (MarDirectorDirect.cpp:174); keep the reduced mask only where the &0x1
    // double-step is a hazard (water scroll counter, etc).
    cpu.gpr[4] = (kDrawLists[li] == 0x20) ? 0xFFFFFFFFu : g_i60.perform_mask;
    cpu.gpr[5] = gfx;
    call_ppc(cpu, PERFORM_LIST_PERFORM);
}
```

This costs one extra `&0x1` advance of `unk48` on the in-between (a tiny exponential-chase
step, `unk4C ≈ 0.01`, `DrawUtil.cpp:97`) — negligible drift, and it can be undone exactly the
way the redraw already snapshots/restores `gpMarioPos` and the j3dSys view (snapshot `unk48`
before, restore after; the probe already reads `sil_before`/`sil_after` at
`interp_redraw.cpp:289,325`). If even that one-step advance is unwanted, the cleaner faithful
variant is **(A2)**: keep `0xFFFFFFFC` for 0x20 too, but **explicitly write
`unk12.a = unk48` (mgr+0x12 alpha = mgr+0x48) before the silhouette list re-issues** so the
`&0x10` draw uses the live alpha without running the `&0x1` chase. Both make the in-between's
marukage identical to the real field.

**Verify** with `/interp60`: after the fix the marukage must be present on the in-between in the
present-ring A/B; `sil_after == sil_before` if you use (A2), or `sil_after` advanced by one
chase step if you use the full-mask form (acceptable; restore it to match the real field).

---

## 1. Which draw paints the marukage pixels (verified)

`TSilhouette::perform &0x10` (`DrawUtil.cpp:116-166`) emits **NO geometry** — it ends at
`GXSetZMode` with no `GXBegin`/`GXDraw`/display-list call. It only SETS UP immediate-mode state:
`GXLoadTexMtxImm(...,0x1e,...)` (:133), `GXSetNumTexGens(2)` (:134), two `GXSetTexCoordGen2`
(:135-137; texgen1 = `GX_TG_POS` through texmtx `0x1e`), binds `unk40`→TEXMAP0 and the marukage
texture `unk44`→TEXMAP1 (:138-139), `GXSetNumChans(1)`, a 2-stage TEV (:149-160),
`GXSetBlendMode(SRCALPHA,INVSRCALPHA)` (:162), `GXSetZCompLoc(GX_TRUE)` (:164),
`GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE)` (:165). (Confirmed: instrument
`ov_silhouette_perform`, `interp_redraw.cpp:120-130`, counts `&0x10` per field.)

**The shadow is therefore painted by whatever geometry is rendered NEXT while that state is
live, with no intervening `GXSetNumTexGens`/`GXSetNumTevStages` reset.** texmtx `0x1e` is used
in exactly two TUs (`grep`-verified): `DrawUtil.cpp` (this setup) and `ModelWaterManager.cpp`
(:1230 `drawMirror`, :1454/1459 `drawRefracAndSpec`). So the receiving draws are:

- **GROUND/MAP:** the map/pollution ground geometry re-drawn in the silhouette pass picks up the
  projected `GX_TG_POS`/texmtx-`0x1e` shadow texture (the on-ground marukage). The shadow's UV is
  camera-independent object/world space (rigorously confirmed in
  `gx_texgen_pos_space.md` against `VertexShaderGen.cpp:643`).
- **WATER:** `TModelWaterManager::drawMirror` (`ModelWaterManager.cpp:1161-1260`,
  texgen `GX_TG_POS`/`0x1e` at :1230) draws the on-water marukage, gated by `&0x8`
  (`perform :1562-1570`) and `isUnk48Positive()` (`drawSilhouette :922`).

The marukage is **destination-alpha gated**: Mario's `&0x80000000` pass writes the EFB DstAlpha
(`GXSetDstAlpha(GX_ENABLE, gpSilhouetteManager->unk48)`, `MarioMain.cpp:228`; cubes blended
`GX_BL_DSTALPHA/INVDSTALPHA`, :236) — i.e. how dark the shadow lands is governed by `unk48`.

**Which list / phase:** the receiving draws and Mario's DstAlpha pass are in
`mPerformListSilhouette` (0x20) — membership is data-driven (`PerformLists.bin`, "PerformList
Silhouette", `MarDirectorSetupObjects.cpp:431-432`) so not source-enumerable, but it is the only
list `direct()` gates on `gpSilhouetteManager->unk48` (`MarDirectorDirect.cpp:172-175`) and the
only one that early-returns water draws on `isUnk48Positive()` — proving `TSilhouette` and the
water silhouette draw both live there. The phase bits in play: `&0x1` (the `unk48`/`unk12.a`
chase — CALC), `&0x10` (texgen setup — DRAW), `&0x8` (water silhouette/mirror DRAW),
`&0x80000000` (Mario DstAlpha DRAW).

---

## 2. Real-field order — when the setup runs vs the receiving draw, and is the state still live?

`direct()`'s `for(;;)` (`MarDirectorDirect.cpp:67-181`) — **important correction to
`direct_draw_flow.md`, which wrongly called Branch B "dormant":**

- Each `direct()` call adds `vsyncRate` to `unk54` (:64) and loops. On the final calc sub-step,
  line 74 sets `unk4C |= 0x4000`; line 161 then runs `unk34->perform(0xffffffff)` (the
  ENTRY/collect pass — fills J3D draw buffers, draws nothing itself; every `unk34` filter is a
  collect/bind/view-calc bit, no `&0x8`) and `break`s (:164) **before** the bottom-of-loop
  `unk4C &= ~0x6000` (:180). So **`0x4000` stays set on exit.**
- On the NEXT `direct()` call, the very first iteration takes **Branch B** (:166-178), the real
  DRAW: `unk40, unk38, unk3C, mPerformListGX(0x1C), mPerformListSilhouette(0x20),
  mPerformListGXPost(0x24)`, each `perform(0xffffffff)`, then `GXInvalidateTexAll()` (:177).
  Then :180 clears `0x4000` and the loop falls into Branch A for the new frame's calc.

So Branch B IS the real per-frame draw (it runs frame N's collected buffers at the head of
direct() for frame N+1). The interp re-issue set `{0x40,0x38,0x3C,0x1C,0x20,0x24}`
(`interp_redraw.cpp:47`) is exactly Branch B — correct.

**Within `mPerformListSilhouette->perform(0xffffffff)` on the real field:** `TSilhouette::perform`
is called with all bits set, so its body runs in source order — `&0x1` FIRST
(`unk48 += ...; unk12.a = unk48`, `DrawUtil.cpp:95-99`) THEN `&0x10` (the texgen setup uses the
just-refreshed `unk12`, :145-146). Mario's `&0x80000000` DstAlpha pass also sees the live
`unk48`. The texgen/TEV/DstAlpha state is established and the receiving ground/water draws follow
in the same list with the state still active (no NumTexGens/NumTevStages reset between them; the
only reset is `GXInvalidateTexAll` at the END of the whole Branch B, :177). **The state is live
at the receiving draw on the real field.**

---

## 3. In-between — why the shadow is absent (the masked-`&0x1` cause)

`interp_redraw.cpp:299-322` re-issues the same lists, but with `cpu.gpr[4] = g_i60.perform_mask`
(`= 0xFFFFFFFC`, `interp60.h:38`). `TPerformList::forEachPerform` AND-s this with each link's
filter (`PerformList.cpp:9-12`), so `0xFFFFFFFC` **clears `&0x1` and `&0x2` for every object in
every re-issued list, including the silhouette list.**

Walking the candidate explanations from the task:

- **(a) receiving draw not in the re-issued set?** NO — the silhouette list (0x20) IS re-issued
  (`kDrawLists` index 4); the ground/map opaque is re-submitted in 0x1C; the player DstAlpha
  pass and the water mirror are in 0x20. All re-issued. So the geometry is drawn.
- **(b) clearing `&0x1`/`&0x2` skips the marukage's needed phase — ✅ THIS IS IT.** The marukage's
  per-frame visibility is `unk12.a = unk48`, written ONLY in `TSilhouette::perform &0x1`
  (`DrawUtil.cpp:98`). With `&0x1` masked on the in-between, the `&0x10` texgen setup still runs
  (probe: fires on both fields) but the **raster/material alpha feeding the shadow's
  `SRCALPHA/INVSRCALPHA` blend is not (re)established for this field**, and Mario's DstAlpha mask
  (which is `unk48`-derived and set in the silhouette pass) likewise depends on the silhouette
  object's `&0x1`-maintained state. The net effect at the receiving draw is the projection
  blending in at ~0 effective alpha → the shadow paints nothing visible → **clean OFF**, exactly
  while the setup is still observed to run. (This is the data-flow analogue of the
  `direct_draw_flow.md` §4 "`unk48` gate" finding, refined by the new fact: it is not the
  list-skip GATE that blinks — the list IS re-issued — it is the masked `&0x1` inside the list
  that starves the receiving draw's alpha.)
- **(c) re-issue order runs the receiver before the setup?** NO — order within 0x20 is the list's
  own membership order, identical to the real field (same `perform(list, mask)` call). Setup
  precedes receiver on both.
- **(d) `GXInvalidateTexAll`/TEV reset between setup and receiver?** NO — interp issues
  `GXInvalidateTexAll` only AFTER the whole list loop (`interp_redraw.cpp:324`), same position as
  the real field's :177. No mid-list reset.
- **(e) drawn during a CALC phase the in-between never runs?** Partially: the `unk34` ENTRY pass
  is not re-issued, but that only fills buffers (no `&0x8` draw) and the buffers persist from the
  real field — so the geometry is still submittable. The relevant CALC the in-between drops is the
  `&0x1` inside the SILHOUETTE list (cause (b)), not a separate calc list.

**Conclusion of §3: the absence is cause (b) — `perform_mask=0xFFFFFFFC` strips `&0x1` from the
silhouette list, starving the marukage's `unk12.a = unk48` (and the DstAlpha gate) so the
re-issued receiving draw blends the shadow in at zero alpha = clean OFF, while `&0x10` setup
still runs (matches "fires on both fields").** The prime suspect named in the task — that
`perform_mask=0xFFFFFFFC` drops a needed phase — is correct, and it is the `&0x1` phase of the
silhouette list specifically.

---

## 4. The fix (concrete, against `interp_redraw.cpp`)

Per-list mask: re-issue the silhouette list (0x20) with `0xFFFFFFFF` (the mask the game itself
uses, `MarDirectorDirect.cpp:174`), keep `0xFFFFFFFC` for the rest. Code in §0. Snapshot/restore
`unk48` (mgr+0x48) around the redraw so the one extra `&0x1` chase step does not accumulate — the
probe already has `sil_mgr`/`sil_before`/`sil_after` wired (`interp_redraw.cpp:284-290,325`); add
the restore the same way `gpMarioPos`/j3dSys-view are restored (:333,329-331).

Cheaper alternative **(A2)** if you want zero state mutation: keep `0xFFFFFFFC` for 0x20, but
**write `unk12.a = unk48` (copy mgr+0x48 → the alpha byte of the `GXColor` at mgr+0x12) just
before the silhouette list re-issues**, so `&0x10` reads the live alpha without running `&0x1`.
(`unk12` is the `GXColor` at `DrawUtil.cpp:24/78/98`; `.a` is its 4th byte.)

### Do NOT
- Do NOT keep `perform_mask=0xFFFFFFFC` globally and hope `&0x10` alone draws the shadow — it
  cannot; the alpha that makes the projection visible is the masked `&0x1` value.
- Do NOT interpolate `gpMarioPos` alone (proven strictly worse, `marukage_shadow_flicker.md` §3)
  — orthogonal to this on/off and harmful.
- Do NOT add a per-field UV/alpha magic offset (bandaid).

---

## 5. Flagged / honest gaps
- **Exact `PerformLists.bin` membership of "PerformList Silhouette" (0x20)** — that
  `TSilhouette`, the player `0x80000000` DstAlpha pass, and the map/water shadow receivers are
  all in this list is inferred from: the `unk48` re-issue gate referencing `gpSilhouetteManager`
  (`MarDirectorDirect.cpp:172`), the water silhouette `isUnk48Positive()` early-return
  (`ModelWaterManager.cpp:922`), and texmtx `0x1e` being shared only by `TSilhouette` +
  `TModelWaterManager`. It is NOT source-enumerable. To make it ground truth, dump
  `/data/PerformLists.bin` (loaded `MarDirectorSetupObjects.cpp:406-422`; entries are
  `name\0`+`u32 filter`) or walk each `TPerformList` at runtime printing `TPerformLink.unk4`
  name + `unk8`. **Recommended before landing**, to confirm the player `0x80000000` membership
  and that the ground receiver's filter survives `0xFFFFFFFC` (it should — it's a `&0x8` draw).
- **Whether the visible-alpha collapse is via `unk12.a` (raster), via Mario's `unk48`-DstAlpha,
  or both** — the §3(b) reasoning shows `&0x1` feeds both. If a runtime A/B with the §0 fix shows
  the shadow returns, that confirms cause (b) regardless of which alpha term dominated; if it
  does NOT fully return, the residual is the DstAlpha mask and the player `0x80000000` pass's own
  dependency — re-check that the player group is registered in 0x20 (not only `unk34`) so its
  DstAlpha write is actually re-issued. (preEntry/`unk34` registers the player only at
  `0x10000000`/`0x204`/`0x8000000`, `MarDirectorPreEntry.cpp:63-65` — NOT `0x80000000`,
  confirming the `0x80000000` silhouette pass is elsewhere, i.e. the 0x20 list.)
- The `direct_draw_flow.md` "Branch B is dormant" claim is **falsified** here (§2): Branch B is
  the real draw, run at the head of the next `direct()` because line 161's `break` skips the
  `0x4000`-clear. This does not change the fix (the interp set already matches Branch B) but the
  note should be corrected.

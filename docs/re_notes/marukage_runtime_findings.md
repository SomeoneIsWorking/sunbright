# Marukage on/off blink — RUNTIME ground-truth plan (decomp theories exhausted)

Status: every pure-decomp theory has FAILED a real test (projection-swim
`marukage_shadow_flicker.md`; masked-`&0x1` `marukage_onoff_fix.md`; `unk48` gate-straddle
`direct_draw_flow.md`/`perform_list_architecture.md`). The user confirms (3×, 10 consecutive
frames) the symptom is UNCHANGED: marukage drop-shadow ON every real field (2n), ABSENT every
in-between field (2n+1) — a clean on/off. `TSilhouette::perform &0x10` (texgen SETUP) is
instrument-confirmed to fire on BOTH fields, and the user's listmask test RULES OUT the +0x20
silhouette list as the shadow's home. So the question is no longer "what does the decomp say"
— it is **what the live GX state and live perform lists actually do on the in-between field.**

This doc is a step-by-step plan for the MAIN session (which can build/run). It uses two
instruments: the NEW `/perflists` probe (this commit, `runtime/overrides/perflist_dump.cpp`)
and the EXISTING GX capture (`runtime/gx_stream.*` / `runtime/gx_parse.*`, already wired into
`interp_redraw.cpp`).

All file:line refer to `decomp/sms/`; addresses GMSE01 (`reference/sms_gmse01_funcs.txt`),
data globals from GMSJ (`reference/sms_gmsj01_symbols.txt`, US deltas possible).

---

## 0. Key live addresses / offsets to read

| what | address / offset | source |
|---|---|---|
| `gpMarDirector` (TMarDirector*) | `0x8040A2A8` (JP) — or live `g_mardir` in `interp_redraw.cpp` | sym map / runtime |
| `gpSilhouetteManager` (TSilhouette*) | `0x8040A208` (JP) | sym map |
| `gpMarioPos` (Vec*) | `0x8040A39C` (JP) | sym map |
| TMarDirector perform-list members | `gpMarDirector + 0x1C..+0x48` (see `perflist_dump.cpp` `kListSlots`) | MarDirector.hpp:164-176 |
| `TSilhouette::unk48` (alpha CHASE / gate) | `gpSilhouetteManager + 0x48` (f32) | DrawUtil.hpp:46 |
| `TSilhouette::unk12.a` (RASTER alpha, `=unk48`, refreshed only in `&0x1`) | `gpSilhouetteManager + 0x12 + 3 = +0x15` (u8) | DrawUtil.hpp:33 + DrawUtil.cpp:98 |
| `TSilhouette::unk40` (ground/pollution tex -> TEXMAP0) | `gpSilhouetteManager + 0x40` (JUTTexture*) | DrawUtil.hpp:44 |
| `TSilhouette::unk44` (H_marukage_xlu_i8 -> TEXMAP1) | `gpSilhouetteManager + 0x44` (JUTTexture*) | DrawUtil.hpp:45 |
| texgen matrix slot used by the marukage | texmtx **0x1e** (`GXLoadTexMtxImm(...,0x1e,...)`, `GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, 0x1e,...)`) | DrawUtil.cpp:133,137 |

`gpSilhouetteManager` is GP-relative on US; if `0x8040A208` reads garbage, do NOT trust it —
the `/perflists` walk FINDS the TSilhouette object by walking the lists (it tags the `<==`
row), so the live object pointer comes out of the dump regardless of the constant.

---

## 1. Run the `/perflists` dump (membership ground truth)

1. Wire the seam (see the `==== REQUIRED SEAM ====` block at the bottom of
   `runtime/overrides/perflist_dump.cpp`) into `runtime/probe_server.cpp` — ONE `/perflists`
   endpoint, nothing else. Then `cmake -B build` **reconfigure** (NEW file → file(GLOB) is
   configure-time) and `/build`.
2. Run headless with the probe + interp60:
   `SUNBRIGHT_HEADLESS=1 SUNBRIGHT_INTERP60=1 SUNBRIGHT_PROBE=1 ./build/sunbright` (drive to a
   gameplay scene where Mario casts a shadow on the plaza floor — use `/pad` to walk).
3. `curl 'http://127.0.0.1:17654/perflists'`.

**Read out of the dump:**
- The list (`+0x1C..+0x48`) whose rows include the `<== gpSilhouetteManager (TSilhouette)`
  tag. This is the DEFINITIVE answer to "which list contains TSilhouette" — the user's
  listmask test says it is NOT +0x20, so EXPECT the tag in a DIFFERENT slot (likely +0x24
  GXPost or +0x34 unk34). **If it is not +0x20, the entire `marukage_onoff_fix.md` /
  `direct_draw_flow.md` premise (silhouette lives in +0x20) is falsified at the data level —
  record which slot it really is.**
- The link DIRECTLY AFTER the TSilhouette row IN THE SAME LIST whose filter has a draw bit
  (`0x8`/`0x10`/`0x200`/`0x204`): that object's `vtable->class` is the ground/map (and/or
  water) RECEIVER that consumes texmtx 0x1e. Note its class + filter + `unkC` disable mask.
- Whether that receiver list is in the interp re-issue set `kDrawLists = {0x40,0x38,0x3C,
  0x1C,0x20,0x24}` (`interp_redraw.cpp:47`). **If the receiver's list is NOT in that set, the
  receiver simply is not drawn on the in-between → clean OFF.** That alone could be the whole
  bug, and it is the first thing to confirm because it is the cheapest explanation the decomp
  could not see (membership is data-driven).

---

## 2. GX trace: does the receiver draw RUN on the in-between, and with what state?

Use the existing capture (`gx_stream`/`gx_parse`, already included by `interp_redraw.cpp`).
Capture ONE real field and the immediately-following in-between field for the same scene.

Answer, for the in-between field specifically:

A. **Does the receiving ground/map draw run at all?** Find the draw command(s) for the
   receiver class identified in §1 (match by the bound TEXMAP / vtable / vertex count). If the
   draw is ABSENT on the in-between but PRESENT on the real field → cause is membership/gate
   (go to §1's "list not in re-issue set" or a runtime `unkC` disable, §4). If PRESENT → the
   draw runs but produces no visible shadow → cause is GX STATE (B/C/D below).

B. **Is texmtx 0x1e still the ACTIVE texgen when the receiver draws?** In the in-between
   stream, between the `&0x10` setup (`GXLoadTexMtxImm slot=0x1e`, `GXSetNumTexGens(2)`,
   `GXSetTexCoordGen2 TEXCOORD1 ... 0x1e`) and the receiver's draw command, check for an
   intervening `GXSetNumTexGens` (reset to 1) or a different `GXSetTexCoordGen2` on TEXCOORD1,
   or a `GXLoadTexMtxImm` overwriting slot 0x1e. The re-issue order differs from the real
   field's order (interp re-issues `{0x40,0x38,0x3C,0x1C,0x20,0x24}` then ONE
   `GXInvalidateTexAll`, `interp_redraw.cpp:324`) — a different interleaving could place the
   setup and the receiver in different lists so the setup is CLOBBERED before the receiver
   draws. **This is the single most likely runtime cause the decomp cannot reach: order of
   list re-issue vs the real field's `direct()` order.** Compare the two streams' command
   ORDER around slot-0x1e, not just presence.

C. **Is `unk12.a` (the marukage raster alpha) nonzero at draw time on the in-between?** The
   TEV stage 0 reads `GX_CC_RASC`/`GX_CA_RASA` from `GX_COLOR0A0` whose mat color is `unk12`
   (`GXSetChanMatColor(GX_COLOR0A0, unk12)`, DrawUtil.cpp:146). `unk12.a` is written ONLY in
   `&0x1` (`unk12.a = unk48`, DrawUtil.cpp:98). Read `gpSilhouetteManager + 0x15` (u8) and
   `+0x48` (f32) live via `/r?a=...` on BOTH fields:
   - If `unk12.a`/`unk48` are the SAME nonzero value on both fields → alpha is NOT the cause
     (falsifies the `marukage_onoff_fix.md` masked-`&0x1` theory at runtime, consistent with
     the user's report that the bug is unchanged after that "fix").
   - If they DIFFER (in-between near 0) → alpha IS starved → but note the user said the setup
     fires on both fields, so confirm whether the *receiver* picks up the live or stale alpha.
   Also check the in-between's `GXSetChanMatColor`/`GXSetTevColor` in the stream — the raster
   alpha actually loaded into GX is what matters, not just the member value.

D. **Is `GXInvalidateTexAll` or a `GXSetNumTexGens` reset landing BETWEEN the `&0x10` setup
   and the receiver draw on the in-between?** `interp_redraw.cpp:324` issues ONE
   `GXInvalidateTexAll` after the whole list loop — but verify in the STREAM that it is not
   landing mid-sequence on the in-between (e.g. if the receiver is in a later list than the
   setup, an inter-list GX reset, or the per-list `GXInvalidateTexAll` the real `direct()`
   does at end of Branch B, `MarDirectorDirect.cpp:177`, could fall between them). Also check
   for a `GXSetNumTevStages(1)` or `GXSetTevOp(...PASSCLR)` from a different object resetting
   the 2-stage modulate the marukage needs (DrawUtil.cpp:149-160).

---

## 3. The decisive comparison

Lay the real-field and in-between GX streams side by side and diff the window from the LAST
`GXLoadTexMtxImm slot=0x1e` to the receiver draw. The bug is whichever of these the
in-between has that the real field does not:
- receiver draw MISSING (membership/`unkC`/gate) — §1, §4;
- texmtx 0x1e / TEXCOORD1 texgen overwritten or NumTexGens reset to 1 before the draw — §2B;
- 2-stage TEV / TEXMAP1 binding reset before the draw — §2D;
- raster alpha (`GX_COLOR0A0` mat color `.a`) loaded as 0 — §2C.

The user's facts pre-constrain the answer: setup fires on both fields (so it is NOT a missing
`&0x10`), and +0x20 is NOT the shadow's list (so the receiver is elsewhere, and the
+0x20-centric theories are moot). The most probable remaining runtime cause is **§2B: on the
in-between the receiver draws in a list re-issued in a DIFFERENT order than `direct()` uses, so
the slot-0x1e texgen set up in one list is no longer active (overwritten / NumTexGens reset)
by the time the receiver draws in another.** The GX-stream order diff settles it directly.

---

## 4. Secondary live reads (if §2A shows the draw is missing)

- `unkC` per-object disable mask: each `/perflists` row prints `unkC` for the receiver. If the
  receiver's `unkC` masks its draw bit on the in-between (toggled by some other system), the
  draw is skipped (`testPerform` clears `param_1 & ~unkC`, JDRViewObj.cpp:3-15). Diff `unkC`
  between fields with `/r?a=<obj+0xC>`.
- Re-issue set membership: if the receiver's list is not in `kDrawLists`, it is structurally
  absent on the in-between — the fix is to add that list (or that object) to the re-issue,
  matching the order `direct()` draws it (so §2B does not bite).

---

## 5. What NOT to do (rejected / falsified)

- Do NOT re-apply the masked-`&0x1` "fix" (`marukage_onoff_fix.md` §0) — tested, FAILED.
- Do NOT interpolate `gpMarioPos` alone — proven strictly worse (`marukage_shadow_flicker.md`
  §3).
- Do NOT add a per-field UV/alpha magic offset — bandaid, drifts with camera.
- Do NOT trust the +0x20 "silhouette list holds the receiver" premise — the user's listmask
  test ruled +0x20 out; the `/perflists` walk gives the real slot.

---

## 6. Deliverable of the run

Record in this file, under a new "## RUNTIME RESULTS" heading: (1) which list slot holds
TSilhouette; (2) the receiver class + its list + filter; (3) whether the receiver draw runs on
the in-between; (4) the GX-stream order diff around slot-0x1e (the §3 decisive line). That
converts the last data-driven unknowns into fact and pins the cause to ONE of §3's bullets.

# cU-dispatch gap — the actors that program cU=FALSE on GC and don't on native (2026-07-02)

## CORRECTION 2026-07-02 (same day, later)

**The causal link "missing cU=FALSE bracket → visible overbright" claimed below is
FALSE.** Diagnostically verified: adding `GXSetColorUpdate(GX_FALSE)` at drawShadow's
exit (mirroring GC's exit state per Ghidra `scratch/decomp_shadow/8022f014.c:226`)
yielded ZERO change in `title_overbright.py` mean|Δ| — baseline 35.4 with and without
the added call, identical to 3 decimal places, identical 4×4 region grid. So the
shadow port's cU-discipline gap is a real port-fidelity gap, but it does NOT
materialize as visible overbright.

The overbright is a **ph1+ph4 scene double-draw** — a render-to-texture composite
gap already correctly identified in `debug_journal/2026-06-30_fileselect_overbright_
is_efb_target_structure.md` and refined in `debug_journal/2026-07-02_overbright_
stopgap_is_dead_code.md`: on GC the ph1 unk40 pre-pass renders OFF-SCREEN to a
texture that later passes sample; native composites both ph1 pre-pass AND ph4 main
scene into the visible framebuffer → double-draw. Per-actor cU discipline is
orthogonal to this.

The cU-writer probe (`SUNBRIGHT_LOG_CU_WRITERS`) itself is still useful — it names
port-fidelity gaps in specific actors — but it should NOT be treated as the
overbright-fix roadmap. The "Fix priority" list below is retained for the actor
port-fidelity work but reordered: those ports may not move overbright at all.

Real overbright fix path is task #8 (segmented snapshot+resample renderer or
targeted per-actor render-to-texture redirect for the ph1 unk40 pre-pass).

---

## What this journal entry establishes

At the settled title screen (stage 15, scenario 0), Dolphin+GX (the
`build/sunbright` oracle) programs `GXSetColorUpdate(FALSE)` around specific
scene actors. Native (`build-native/sms-boot`) never programs cU=FALSE around
a shape draw — locked earlier by `SB_CU_ENTRY_TRACE` (400k J3DShape::draw
entries, 100% cU=TRUE across every phase). This is why the file-select
overbright wash exists: map materials paint at every phase (ph1/ph4/ph6) as
if cU=1, so per-phase lighting variation on the same shape becomes visible
and ph6 (brightest) wins.

The dispatch gap is per-actor, not per-phase. Fixing the file-select
overbright requires restoring the per-actor depth-only prepass discipline
(cU=FALSE → drawGeom → cU=TRUE) inside the specific ported functions listed
below.

## The probe (SUNBRIGHT_LOG_CU_WRITERS)

Commits: `843b124` (initial), `885ed62` (mask-aware + backchain hoist).

Every BPMEM_BLENDMODE write is checked for a cU (bit 3) transition, applying
Dolphin's BPMEM_BP_MASK semantics — GDSetBlendMode's `0xFE001FE3` mask
preamble preserves cU/aU, so raw-value comparison over-counts phantom flips
(measured: 17 phantom + real per frame → 10 real per frame after mask
awareness). Each real transition is correlated to the nearest preceding
gather-flush FlushMark and its guest back-chain is walked to find the
innermost specific-actor perform/draw frame (skipping the outer
`TPerformList::perform` dispatcher).

Run recipe (headless):
```
SUNBRIGHT_HEADLESS=1 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_SCENARIO=0 \
SUNBRIGHT_NGX_PRESENT=0 SUNBRIGHT_PROBE=1 SUNBRIGHT_TURBO=1 \
SUNBRIGHT_PARITY_DUMP=scratch/oracle/parity.jsonl \
SUNBRIGHT_GX_ATTRIB=1 SUNBRIGHT_DBG_GXAT=200 SUNBRIGHT_DBG_GXAT_WIDTH=8 \
SUNBRIGHT_LOG_CU_WRITERS=1 build/sunbright
# then curl /pad?do=start x2 with a 55s settle, harvest [cU-writer] lines
```

## The oracle-side cU=FALSE writers (settled title)

Histogram over ~8 settled frames, cU=0 transitions only:

  5 drawShadow__19TMBindShadowManager+0x370
  5 calcDrawVtx__18TModelWaterManager+0x2c0
  2 SMS_SettingDrawShape+0x34
  2 SMS_InitPacket_OneTevColor+0x930
  2 SMS_InitPacket_OneTevColor+0x5cc
  2 SMS_FillScreenAlpha+0xdc
  2 perform__6TMario+0x6b4
  2 loadVtxArray__8J3DShape+0x2c
  (+ WriteMTXPS4x3/GXBegin/GXLoadPosMtxImm/__GXXfVtxSpecs when the walk
   couldn't reach a specific actor within 16 frames)

Confirmed on the GC binary via `sunbright-recomp --callees` of the reported
functions:

  drawShadow (0x8022f014):
    0x8022f198  bl GXSetColorUpdate    ← cU=0 for depth-only silhouette prepass
    0x8022f1a4  bl GXSetDstAlpha
    0x8022f1b4  bl GXSetZMode          ← Z write on, colour off
    0x8022f1c8  bl GXSetBlendMode
    …geometry emits…
    0x8022f32c  bl GXSetColorUpdate    ← cU=1 restore

## Where native diverges

`native/platform/gx_impl.cpp:752`: `GXCallDisplayList` is a no-op stub.
Material display lists baked by J3DMaterial::makeDisplayList are silently
dropped. But per-material cU state isn't the issue: GDSetBlendMode uses a
0x001FE3 mask preamble that PRESERVES bit 3, so material DLs don't change cU
anyway. The material-DL drop is a separate correctness gap, not the cU-
dispatch gap.

The real cU gap is in the **actor-level ports**:

**TMBindShadowManager::drawShadow** — `reference/sms/src/MarioUtil/ShadowUtil.cpp:155`.
Native port is a single-pass GX_TRIANGLES disc emit with cU=TRUE the whole
time. The GC version has a **depth-only prepass**: cU=FALSE + GXSetDstAlpha
+ GXSetZMode(write=on) + geometry, then cU=TRUE for the visible pass. The
port collapsed both passes into one → no shadow silhouette in the depth
buffer, and the cU discipline that the rest of the frame expected is gone.

**TModelWaterManager::calcDrawVtx / drawWaterVolume / drawMirror / drawSilhouette**
— all four write cU=FALSE on GC. Native's TModelWaterManager is PARKED
(memory `delfino-lighting-wash`) — nothing runs. At stage 15 the reflective
sea + mirror path is expected to fire; nothing programs cU=FALSE for it, so
the mirror prepass never gates colour writes and the composite ends up
double-flushed with brightest lighting winning.

**SMS_FillScreenAlpha** — `reference/sms/src/MarioUtil/ScreenUtil.cpp:236`.
The GC version calls GXSetColorUpdate(FALSE) at line 252, emits the alpha
fill, GXSetColorUpdate(TRUE) at line 270. Need to check whether the native
callers (that reach this via the perform-list, transitively) run through
the C++ body or a stub.

**TMario::perform** — the +0x6b4 offset in the GC binary is inside a
per-frame draw path. Whether this fires on native depends on whether Mario
is actually being drawn at stage 15 (he IS visible on the title screen);
its cU=FALSE effect on rest-of-frame state matters even if the specific
draw itself is minor.

**SMS_SettingDrawShape / SMS_InitPacket_OneTevColor / loadVtxArray__8J3DShape**
— J3D helper-level writes that are called from within TMario::perform,
drawShadow, etc. They program the cU bit as part of packet setup. If the
higher-level actor's port skips them, the cU discipline is missing.

## Practical fix path

Do NOT patch the renderer. Do NOT special-case the material key. The fix is
to restore the per-actor cU=FALSE/cU=TRUE bracket in each of the ported
functions above, matching the GC C++ 1:1.

Priority order for stage-15 impact:
1. TMBindShadowManager::drawShadow — full 2-pass port (depth prepass +
   colour pass) matching the GC callee list.
2. TModelWaterManager — un-park drawMirror / drawWaterVolume / calcDrawVtx
   / drawSilhouette; program cU=FALSE for the prepass volumes.
3. SMS_FillScreenAlpha — port faithfully with the cU bracket.
4. Any other file-select-visible callers surfaced by the probe on later
   scenes.

Verification: rerun `SUNBRIGHT_LOG_CU_WRITERS=1` on the oracle to confirm
the caller list is unchanged, then run sms-boot with `SB_COLUPD_ALL=1` and
diff caller symbols. Once the ported actors ALSO show up as cU=FALSE
writers on native, the `SB_CU_ENTRY_TRACE` cU=1/cU=0 ratio at J3DShape::draw
should shift from 100/0 toward the oracle's ~4k depth-only per frame — and
the overbright wash should collapse.

The GXCallDisplayList stub is a separate gap; keep it in mind but don't
attribute the overbright to it. Its role is limited to material state
programming (GDSetBlendMode/GDSetAlphaCompare/GDSetZMode/…), all of which
either preserve cU (via BP_MASK) or don't touch cU at all.

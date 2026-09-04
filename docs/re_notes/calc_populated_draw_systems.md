# Calc-populated → draw → finalize systems (the 60 fps in-between blink class)

Generalization of the `TMBindShadowManager` cast-shadow fix (`shadow_interp.cpp`,
`real_shadow_bindmgr.md`). The 60 fps interpolator (`interp_redraw.cpp`) re-issues ONLY
the draw-branch perform lists on the in-between field — `kDrawLists = {0x40, 0x38, 0x3C,
0x1C, 0x20, 0x24}` with `perform_mask = 0xFFFFFFFC` (clears &1 movement / &2 calc-anim;
KEEPS &4 view-calc, &8 draw, &0x10, &0x80). It does NOT run the calc/movement lists
(`mPerformListMovement` 0x28, `mPerformListCalcAnim` 0x2C, `unk30` 2D-calc, `mShinePfLst
Mov/Anm` 0x44/0x48) nor the inline `movement()`, nor the main-scene `unk34` (0x34).

This doc finds every OTHER system that, like the bind shadow, **renders empty/stale on
the in-between** because its on-screen output depends on per-frame state produced in a
phase the in-between skips.

> Sources, all read (not guessed): `decomp/sms` decomp + `reference/sms_gmsj01_symbols.txt`
> (JP map — the US build is GMSE01; .sbss deltas are NON-uniform, see §4) +
> `docs/re_notes/perform_list_architecture.md` (which effect draws in which list/phase) +
> `docs/re_notes/real_shadow_bindmgr.md` (the template) + the live runtime
> (`interp_redraw.cpp`, `perflist_dump.cpp`).

---

## 0. THE TWO SUB-CLASSES (this is the whole insight)

A system whose populate runs in calc and whose draw runs in the re-issued draw lists is
NOT automatically a blinker. It blinks ONLY if its draw input is **rebuilt-and-zeroed
per frame**. There are two distinct behaviours:

| sub-class | populate source | per-frame reset? | in-between result |
|---|---|---|---|
| **A. REQUEST-DRIVEN (BLINKS)** | a per-frame request array/count, filled by actors in the &4 calc phase, **zeroed by the manager's own finalize/&4-reset** | YES — count zeroed every real field | the in-between's &4 rebuild reads a 0 count → **empty draw = clean on/off blink** |
| **B. PERSISTENT-STATE (does NOT blink)** | a long-lived particle/object list that survives across frames; calc only ADVANCES it | NO | the in-between's draw reads the persistent list → **frozen for one field (30 Hz stutter), not on/off** |

Class A is the shadow's class and the only class the snapshot/restore fix is FOR.
Class B needs interpolation (position blend), not snapshot — a different, lower-priority
problem, and double-ticking its calc would corrupt it (forbidden by the interp design).

**The discriminator:** does the manager's `perform` zero a count / flip a draw buffer to
empty in the SAME phase it rebuilds, with the producer being a per-frame request queue
(class A) — versus iterating a persistent linked list / SOA that only `move()` mutates
(class B)?

---

## 1. CONFIRMED CLASS A — request-driven, BLINKS (apply snapshot/restore)

### 1.1 TMBindShadowManager — Mario's real cast shadow  ✅ ALREADY FIXED

The template. `shadow_interp.cpp`. mgr `MEM_R32(0x8040E0C0)` (US). Request queued by
`TLiveActor::requestShadow()` (`liveactor.cpp:295`, addr 0x80218020) in the &4 phase;
built by `calcVtx` (0x8022e0cc) into `mgr+0x1C`/count `mgr+0x20` (0x14-byte entries);
drawn by `drawShadow` (0x8022f014)/`drawShadowGD` (0x8022fa40); zeroed by the &0x20000000
FINALIZE in `perform` (0x80231108). Snapshot offsets: base `+0x1C`, count `+0x20`, entry
stride 0x14. See `real_shadow_bindmgr.md` for the full RE.

### 1.2 TQuestionManager — the floating `?` / target indicator over actors  ⬅ NEW, SAME BUG

The single clearest other instance of the EXACT shadow pattern.

- **Class & globals:** `TQuestionManager : JDrama::TViewObj`, `gpQuestionManager`
  (JP .sbss `0x8040A368`; US ≈ see §4 — resolve live). Header
  `decomp/sms/include/Strategic/question.hpp`.
- **POPULATED (calc &4 phase, via actors):** `TQuestionManager::request()`
  (`question.cpp:25`) appends to `unk1C[unk12]` and `++unk12` (count `unk12`, u16 @ +0x12;
  cap 0x20; request array `unk1C` @ +0x1C, 0x10-byte `TQuestionRequest` entries). Callers
  are all in the &4 phase — **in the SAME `TLiveActor::requestShadow()` body** as the bind
  shadow (`liveactor.cpp:323`: `gpQuestionManager->request(mPosition, mScaledBodyRadius)`),
  plus `Item.cpp:295`, `hinokuri2.cpp:1043`, `tamaNoko.cpp:542`, `Yoshi.cpp:1133`.
- **DRAW BUFFER built + FINALIZED in one &4 block** (`TQuestionManager::perform`,
  `question.cpp:160`):
  ```
  if (param_1 & 4) {
      if (gpSilhouetteManager->isUnk48Positive()) {
          unk20->reset();        // TDLTexQuad::reset(): unk8=0; unk4 = 1-unk4 (FLIP buffer)
          makeDL(param_2);       // iterate unk12 requests -> unk20 quad buffer; setEnd()
          unk10 |= 2;
      } else unk10 &= ~2;
      unk12 = 0;                  // <-- FINALIZE: zero the request count
  }
  if ((param_1 & 8) && gpSilhouetteManager->isUnk48Positive() && (unk10 & 2))
      draw();                    // unk20->draw()
  ```
- **WHY IT BLINKS:** `makeDL` (`question.cpp:43`) loops `for (i=0; i<unk12; ++i)`. On the
  real field, the &4 block zeroes `unk12` AFTER building. On the in-between, no actor ran
  `request()` (calc skipped), so `unk12 == 0`; the in-between's &4 runs `reset()`
  (flips `unk20` to the other buffer, count 0) + `makeDL` over 0 requests → empty buffer →
  `draw()` draws nothing → clean on/off blink, identical mechanism to the shadow.
- **EXACT STATE TO SNAPSHOT/RESTORE:** the BUILT draw buffer is the `TDLTexQuad` at
  `unk20` (+0x20). `TDLTexQuad` (`DLUtil.hpp:7`) is **double-buffered**, the live buffer
  index is `unk4` (u16 @ +0x4 of the TDLTexQuad), the write count is `unk8` (u16 @ +0x8),
  the DL bytes live in `unkC[unk4]` (u8* @ +0xC[0/1]) and the pos floats in
  `unk14[unk4]` (f32* @ +0x14[0/1]), byte length `unk1C` (u32 @ +0x1C). Cleanest restore
  (mirroring the shadow): on the in-between, **mask off &4** (so it can't `reset`/`makeDL`
  to empty) and keep &8 (draw); but because `reset()` flips `unk4`, the simplest faithful
  approach is to also restore `unk12` to the real field's pre-finalize value (snapshot it
  in a `request`/`makeDL` tee) and let &4 rebuild from the same requests — OR snapshot the
  TDLTexQuad's `unk4/unk8/unk1C` + the `unkC[unk4]`/`unk14[unk4]` bytes after the real
  field's draw and restore them, masking &4 on the in-between. Either works; masking &4 +
  restoring the count `unk12`'s pre-zero value is the smaller change (one count + an array
  re-run), exactly the shadow's "drop &4, keep &8, restore the count" shape.
- **Gating subtlety:** draw is also gated on `gpSilhouetteManager->isUnk48Positive()`
  (the marukage occlusion alpha, advanced only in the &1 calc phase — `DrawUtil.cpp:95`).
  So this system rides the SAME `unk48` gate the marukage does; if `unk48` straddles 0
  between fields the gate alone can flip it on/off. The gpMarioPos/silhouette handling
  already in `interp_redraw.cpp` is adjacent to this.

---

## 2. CONFIRMED CLASS B — persistent-state, does NOT blink (do NOT snapshot; leave or interpolate)

These were strong candidates from the task brief; each was checked and is class B
(persistent draw source) — they render FROZEN (one-field stutter) at worst, not on/off.
Listing them so the same ground isn't re-walked.

### 2.1 JPA particle systems — `TMarioParticleManager` / `TEmitterViewObj` / JPAEmitterManager
- `EmitterViewObj.cpp:25` `TEmitterViewObj::perform`: &2 → `calc()` (advance sim), &8 →
  `draw()`. `TMarioParticleManager::perform` (`EmitterViewObj.cpp:~108`): &2 calc, draw
  later. The draw reads the PERSISTENT emitter linked lists `unk44[group]`
  (`JPAEmitterManager::drawBase`, `JPAEmitterManager.cpp:121`) which are NOT cleared per
  frame — emitters live until they die. On the in-between &2 is masked off → no calc
  double-tick (correct), and draw reads the live emitter list → particles **freeze for one
  field**, not blink. This is the bulk of SMS effects (Mario dust/smoke/sweat via
  `MarioParticle.cpp` `emit*`, item-get sparkles, etc.) and `MarioEffect.cpp:167`
  (FLUDD dash spray) which delegates to `gpMarioParticleManager->emitAndBindToMtxPtr` +
  per-frame J3D anim — all persistent.
- **Verdict:** class B. NOT a snapshot target. Smoothness (de-stutter) would need a
  per-emitter position interp, which is a separate, much larger feature and explicitly
  out of scope for the blink fix.

### 2.2 Water spray particles — `TModelWaterManager`
- `ModelWaterManager.cpp:1541` `perform`: &1 → `move()` + `calcWorldMinMax()` +
  `unk5E00++`; &4 → `calcDrawVtx`/`calcVMAll` (build matrices); &8 → `drawSilhouette`/
  `drawWaterVolume`; &0x80 → `drawRefracAndSpec`. The particle array is an SOA
  (`mParticleCount` @ the manager, `mParticle*SOA[]`) that PERSISTS — `move()` compacts it
  to `nextFreeSlot` (`:404`) and adds/removes incrementally; it is NOT zeroed per frame
  (the only `mParticleCount = 0` is init, `:90`). On the in-between &1 is masked (no
  double-move), &4 rebuilds matrices from the persistent SOA → **frozen, not blink**. (The
  interp path already deliberately masks &1 here to avoid double-stepping the `unk5E00`
  texture scroll — see `interp_redraw.cpp` comment + `water_60fps_correctness.md`.)
- **Verdict:** class B. There is a `water_native.cpp` override already; the spray is
  frozen-for-a-field at worst, low severity.

### 2.3 Water splashes — `TSplashManager`  (the deceptive one)
- `SplashManager.cpp:160` `perform`: &2 → `move()`; &4 → `makeDL`; &8 → `draw`. This LOOKS
  like class A (reset → makeDL → draw, via the same double-buffered `TDLColorTexQuad`
  `unk640`). BUT `makeDL` (`:85`) iterates the **persistent** `unk118` `JSUList<TWaterSplash>`
  (modified only by `move()`/`newSplash`), NOT a per-frame count. So on the in-between the
  &4 `makeDL` re-runs `unk640->reset()` then rebuilds the SAME quads from the live
  `unk118` list → the splash buffer is correctly repopulated → **no blink** (frozen at
  worst, since positions don't advance without `move()`).
- **One caveat:** `makeDL` mutates splash state (`splash->unk10 = 0` for out-of-view
  splashes, `:99`) — a benign idempotent cull that re-runs on the in-between. Harmless.
- **Verdict:** class B (self-heals). NOT a snapshot target. This is the trap: same
  `TDLTexQuad` family as TQuestionManager, opposite outcome — the difference is the
  populate source (persistent list vs zeroed count).

### 2.4 Lens flare / sun-occlusion glow / specular sheen
- `lensflare.cpp` `TLensFlare::perform`: &1 advances the alpha chase `unk24→unk28`
  (`:14+`); `lensglow.cpp` `TLensGlow::perform`: &1 advances `unk48/unk4C/unk60`,
  computes screen-space center (`:9+`). Both store the result in **persistent member
  fields**; their DRAW (entry 0x204 in `mPerformListGXPost`, `MarDirectorInitECT.cpp` /
  perform_list doc §3d) reads those members. On the in-between calc is skipped → draws with
  LAST field's alpha/position = **1-field lag (stale), not absent**.
- **Verdict:** class B (stale-not-blink). Low severity (a flare lags one field during
  fast camera motion). Could be improved by blending the cached screen-space center, but
  it does not blink, so it is NOT a snapshot/restore target.

---

## 3. PRIORITY (by what the user would actually SEE blink at 60 fps)

1. **TQuestionManager `?`/target indicator (§1.2) — HIGH, fix now.** Clean on/off blink,
   identical class to the already-fixed shadow, appears over interactable NPCs/objects and
   enemies (very common in Delfino/stages). Apply the snapshot/restore (mask &4, keep &8,
   restore `unk12`/the `unk20` TDLTexQuad). This is the direct payoff of the generalization.
2. **TMBindShadowManager cast shadow (§1.1) — DONE.** Reference only.
3. **JPA particles frozen-for-a-field (§2.1) — MEDIUM but DIFFERENT FIX.** No blink, but a
   30 Hz stutter on dense effects (dust trails, FLUDD spray, sparkles) is visible. Needs
   position interpolation, not snapshot — defer to a particle-interp follow-up.
4. **Water spray / splashes / refraction (§2.2/§2.3) — LOW.** Frozen-for-a-field; splashes
   self-heal; refraction already has the EFB-copy investigation in `interp_redraw.cpp`.
5. **Lens flare / glow / specular sheen (§2.4) — LOW.** 1-field lag only, visible solely on
   fast camera pans toward the sun.

The only NEW system requiring the shadow's snapshot/restore fix is **TQuestionManager**.

---

## 4. UNIFORM vs PER-SYSTEM — recommendation: **PER-SYSTEM (snapshot/restore), NOT a uniform finalize-mask**

A uniform "mask the finalize bit on the in-between so all draw state persists" was the
attractive idea. It does NOT work here, for a concrete RE'd reason:

- **The finalize is not a shared global bit, and it has already run by the time the
  in-between executes.** The bind shadow's finalize is its own `&0x20000000` block; the
  question manager's "finalize" is `unk12 = 0` inside its `&4` block (no dedicated bit at
  all — it is unconditional within &4). There is no single phase bit common to both whose
  masking defers all resets. More importantly, the real field's `perform` runs reset→build
  →draw→finalize **in one call during the real field**; by the time the in-between's
  `endRendering` fires and re-issues the draw lists, the real field's finalize has ALREADY
  zeroed the counts. Masking a bit on the IN-BETWEEN cannot un-zero state the REAL field
  destroyed a moment earlier. (This is exactly option (b), rejected in
  `real_shadow_bindmgr.md` §4.)
- **The class-A managers don't even share a reset bit:** shadow uses &0x20000000; question
  uses an unconditional `unk12=0` in &4. A "mask the finalize" pass would have to know each
  manager's idiosyncratic reset, i.e. it degenerates into per-system handling anyway.
- **A uniform mask would over-broadly suppress &4** across ALL draw-list objects, breaking
  the class-B systems that REQUIRE &4 to rebuild from persistent state on the in-between
  (water `calcVMAll`, splash `makeDL`, the J3D draw-buffer entry) — those would go empty.

Therefore: **per-system snapshot-on-real-field / restore-on-in-between**, exactly the
`shadow_interp.cpp` template — capture the BUILT draw state after the real field draws
(before finalize zeroes it), restore it for the in-between, and mask off only THAT
manager's rebuild phase (&4) while keeping &8. One small override TU per confirmed class-A
system. Today that means **one new override for TQuestionManager**; the shadow already has
its own.

---

## 5. Address resolution caveat (US vs JP) — verify live, do not hardcode the computed delta

The function/global addresses for TQuestionManager are NOT in `sms_gmse01_funcs.txt`
(unsymbolized). The JP map gives `gpQuestionManager = 0x8040A368`, `gpSplashManager =
0x8040A390`. The US build (GMSE01) differs, and **the .sbss delta is non-uniform**:
gpBindShadowManager is JP 0x8040A238 → US 0x8040E0C0 (Δ 0x3E88), but gpMarioPos is JP
0x8040A39C → US 0x8040E10C (Δ 0x3D70). So do NOT compute a single US constant for
gpQuestionManager and trust it.

Resolve at runtime instead (the `perflist_dump.cpp` discipline — walk/verify live, don't
trust a constant):
- Find `gpQuestionManager` by reading the SDA slot (accept a probe override like
  `perflist_dump` does), OR locate the object by walking the perform lists for the
  TQuestionManager vtable.
- Get `TQuestionManager::perform` (the override seam) from the live object's **vptr at
  obj+0**: `perform` is the vtable slot after `~dtor`/`load` (header order: `~`, `load`,
  `perform`). Override on the resolved address, or — cleaner — tee the parent
  `TPerformList::perform` and key on the object like `interp_redraw.cpp` already does for
  the GX-list snapshot, or add a small dispatch in the existing `shadow_interp.cpp`-style
  TU once the address is confirmed via `/perflist` dump + `scratch/disppc.py`.

Verification (mirror the shadow plan): with INTERP60 on, confirm the question manager's
draw count / `unk20` buffer is > 0 on real fields and 0 on the in-between BEFORE the fix
(proves class A); after the fix, equal on both fields and the `?` indicator no longer
blinks. A/B against the 30 fps Dolphin oracle for shape/position.

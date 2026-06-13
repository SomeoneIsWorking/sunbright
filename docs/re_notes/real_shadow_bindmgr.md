# Real cast-shadow (TMBindShadowManager) — the 60 fps on/off blink

Scope: the user's A/B image confirms a clean **on/off blink** of Mario's REAL projected
cast shadow (the sun-offset silhouette, NOT the round `marukage` blob — that is
`TSilhouette`, a separate system already handled in `marukage_*`) on the 60 fps
in-between fields. This doc REs the full `TMBindShadowManager` request→draw cycle, pins
the root cause, and gives the concrete snapshot/restore fix for `interp_redraw.cpp`.

The cast-shadow system is the **bind-shadow** path: `gpBindShadowManager` /
`TMBindShadowManager`. Actors queue a `TCircleShadowRequest` each frame; the manager
accumulates them, then builds and draws the shadow geometry once per frame.

> Source caveat: `reference/sms/src/MarioUtil/ShadowUtil.cpp` is **empty** in this decomp
> (not yet decompiled). Everything below is RE'd directly from the GMSE01 DOL via
> `scratch/disppc.py` (capstone PPC), cross-checked against the symbol table
> (`reference/sms_gmse01_funcs.txt`), the class header
> (`reference/sms/include/MarioUtil/ShadowUtil.hpp`), and the callers in
> `liveactor.cpp` / `enemymanager.cpp` / `MarioMain.cpp` / `MarDirectorDirect.cpp`.

---

## 0. Addresses & globals (GMSE01)

SDA base **r13 = 0x804141C0** (confirmed across runtime overrides).

| symbol | address |
|---|---|
| `gpBindShadowManager` (pointer to the live manager) | **`*(0x8040E0C0)`** (= `[r13 - 0x6100]`) |
| `TMBindShadowManager::perform(u32, TGraphics*)` | `0x80231108` |
| `TMBindShadowManager::request(const TCircleShadowRequest&, u32)` | `0x8022ecec` |
| `TMBindShadowManager::forceRequest(...)` | `0x8022ebbc` |
| `TMBindShadowManager::drawShadow(u32, TGraphics*)` | `0x8022f014` |
| `TMBindShadowManager::drawShadowGD(u32, TGraphics*)` | `0x8022fa40` |
| `TMBindShadowManager::calcVtx()` | `0x8022e0cc` |
| `TLiveActor::requestShadow()` | `0x80218020` |

`gpBindShadowManager` is created by name ("BindShadow") in
`src/System/MarNameRefGen.cpp:48-49` and placed into a perform list by the scene's
name-ref / PerformLists.bin data (a `JDrama::TViewObj`).

---

## 1. The manager's storage layout (RE'd from request/perform/calcVtx)

Three parallel buffers live in the manager object (`this` = `gpBindShadowManager`):

| offset | field | role |
|---|---|---|
| `this+0x10` | `TCircleShadowRequest* reqArray` | base of the per-frame request array |
| `this+0x14` | `u32 reqCount` | request count (capped 0x200), entry stride **0x24 bytes** |
| `this+0x1C` | `void* drawArray` | base of the built draw/vertex array |
| `this+0x20` | `u32 drawCount` | number of draw entries (iterated by `drawShadow`), entry stride **0x14 bytes** |
| `this+0x40` | `u16 auxCount` | count of a secondary 0x14-byte array at `this+0x70` |
| `this+0x70` | `void* auxArray` | secondary array base |
| `this+0x49` | `u8` | reset/active flag |
| `this+0x2c`, `+0x65` | misc | cleared by finalize |

**`request()` (0x8022ecec):** distance-culls (camera pos at `[r13-0x7118]+0x124..`),
NaN-checks the position floats, then:
```
count = this->reqCount (+0x14); if (count >= 0x200) return;     // cap
slot  = this->reqArray (+0x10) + count*0x24;                    // 0x24-byte entry
*slot = *request (copy the 0x24-byte TCircleShadowRequest);
this->reqCount (+0x14) = count + 1;                             // bump
(parallel writes into the +0x70 / +0x40 aux array)
```
(`disppc.py 0x8022eec8`: `mulli r4,count,0x24` → stride 0x24; `addi r0,count,1; stw …+0x14`.)
`forceRequest()` is the same minus the distance/visibility cull.

---

## 2. The per-frame cycle: reset → build → draw → finalize, all in `perform`

`TMBindShadowManager::perform(u32 param /*r4*/, TGraphics* /*r5*/)` at `0x80231108`
splits into THREE phase-gated blocks (masks decoded from the `rlwinm.` at each gate):

| gate | bit | block |
|---|---|---|
| `param & 0x4`  | `rlwinm. r0,r4,0,0x1d,0x1d` | **RESET + BUILD**: `stb 0,+0x49`; call `[r13-0x610c]` reset; clear a TList at `this+0x30` (`0x8034a5d0`); **`calcVtx()` (0x8022e0cc)** |
| `param & 0x8`  | `rlwinm. r0,r4,0,0x1c,0x1c` | **DRAW**: if `[r13-0x60f7]` → `drawShadowGD` else `drawShadow` |
| `param & 0x20000000` | `rlwinm. r0,r4,0,2,2` | **FINALIZE**: zero `+0x14` (reqCount), `+0x20` (drawCount), `+0x40` (auxCount), `+0x2c`, `+0x65`; set `+0x49=1` |

The blocks execute in that order within a single `perform` call when the incoming mask
(after the per-link `~unkC` filter, `JDRViewObj.cpp:11`) has all three bits set.

**`calcVtx()` (0x8022e0cc)** reads `this->reqArray (+0x10)` and transforms the queued
`TCircleShadowRequest`s into the `this->drawArray (+0x1C)` / `drawCount (+0x20)` geometry.
So: RESET+`calcVtx` BUILDS the draw array from requests queued *earlier this frame*; DRAW
renders it; FINALIZE empties the request + draw + aux counts for next frame.

**`drawShadow()` (0x8022f014)** loop (`disppc.py 0x8022f154-0x8022f488`):
```
for (i=0; i < this->drawCount (+0x20); i++) {            // cmpw r27, [r31+0x20]
    e = this->drawArray (+0x1C) + i*0x14;                // stride 0x14
    if (e[+4]==0 || e[+0xc]==0) continue;               // null/zero entry skip
    if (!(param & e[+0])) continue;                      // per-entry phase mask
    ... emit shadow quad (drawShadowVolume @ 0x802305dc / 0x80225d00) ...
}
```
i.e. **drawShadow draws nothing when `drawCount (+0x20) == 0`.**

---

## 3. Where requests are queued — and why the in-between has none

`TLiveActor::requestShadow()` (`liveactor.cpp:295`, addr `0x80218020`) calls
`gpBindShadowManager->request()/forceRequest()`. It is invoked **only in the `param & 4`
phase** of the actor's perform:

- `TLiveActor::perform` (`liveactor.cpp:351-352`): `if (param_1 & 4) requestShadow();`
- `TSpineEnemyManager::perform` (`enemymanager.cpp:321-322`): `if (param_1 & 4) enemy->requestShadow();`
- `TMario`: `unk390->entryDrawShadow()` (`MarioMain.cpp:145`) — Mario's own bind body.

`TMarDirector::direct()` (`MarDirectorDirect.cpp:67-178`) is a fixed-step loop:
**Branch A** (`!(unk4C & 0x4000)`, lines 68-165) = the **calc/movement** sub-steps; it runs
`mShinePfLstMov`, `unk30`, `movement()`, `mPerformListCalcAnim` — the lists/phase where
actors run their `&4` phase and thus call `requestShadow()`. **Branch B** (lines 166-178)
= the **draw** pass, run ONCE at frame end with `0xffffffff`: `unk40, unk38, unk3C,
mPerformListGX, mPerformListSilhouette, mPerformListGXPost`.

So the real-field timeline is:

```
[Branch A]  actors run &4 phase → request() fills reqArray(+0x10)/reqCount(+0x14)
[Branch B]  BindShadow::perform → RESET+calcVtx (builds drawArray from the requests)
                                 → DRAW (renders drawArray)
                                 → FINALIZE (reqCount=0, drawCount=0, auxCount=0)
```

The **60 fps in-between** (`interp_redraw.cpp`) re-issues ONLY the draw-pass lists
(`kDrawLists = {0x40,0x38,0x3C,0x1C,0x20,0x24}`, i.e. NOT Movement/CalcAnim) with
`perform_mask = 0xFFFFFFFC` (clears only &1/&2; **&4, &8, &0x20000000 still reach the
manager**). On the in-between:

```
[no Branch A]   → NO requestShadow() calls → reqCount(+0x14) stays 0
[re-issue 0x1C] BindShadow::perform → RESET+calcVtx over an EMPTY reqArray → drawCount(+0x20)=0
                                    → DRAW iterates 0 entries → NOTHING DRAWN
                                    → FINALIZE (no-op)
```

**Root cause (seed CONFIRMED, mechanism refined):** the cast shadow is request-driven.
Requests are produced in the calc/movement phase (`&4`), which the in-between does not run,
and the real field's own FINALIZE phase (`perform` `&0x20000000`) already zeroed
`reqCount`/`drawCount`. So when the in-between re-runs the manager's RESET→`calcVtx`→DRAW,
it builds and draws an empty array → clean on/off blink. (The seed's "reset() between
fields" is precisely the manager's own `&0x20000000` FINALIZE block + the `&4` RESET on
re-issue, not a separate `reset()` call.)

This matches every observed fact: clean on/off (not detach), independent of `marukage`
/`TSilhouette`, independent of `list_mask`/`perform_mask` (the manager is in a re-issued
draw list, it just has nothing to draw).

---

## 4. THE FIX — snapshot the BUILT draw array, restore it for the in-between

Three options were on the table:

- **(a) Snapshot the request/draw array after the real field, restore for the in-between.**
  ✔ Clean, side-effect free, no double-tick. Recommended.
- (b) Defer the manager's FINALIZE so the queue survives. ✘ The FINALIZE runs inside the
  same real-field `perform` call as DRAW (can't separate without intercepting the manager),
  and the in-between's own RESET phase would still `calcVtx` and could mutate state; you'd
  have to also suppress RESET. More invasive than (a).
- (c) Re-issue `requestShadow` on the in-between. ✘ Re-ticks actor state (distance cull
  reads live camera, `gpQuestionManager->request`, flag mutations) — risky double-tick,
  exactly what the 60 fps design forbids.

**Recommendation: (a), snapshotting the already-BUILT draw array** (`this+0x1C`/`+0x20`),
not the raw requests. Rationale: the draw array is the direct input to `drawShadow`'s loop;
restoring it lets the in-between's manager `perform` SKIP its RESET/`calcVtx` rebuild and
draw the snapshot directly. Snapshotting raw requests instead would force re-running
`calcVtx` on the in-between (extra work, and `calcVtx` reads the live camera at `[r13-…]`).

### Mechanism (for `interp_redraw.cpp`)

1. **Resolve the manager once per frame:** `mgr = MEM_R32(0x8040E0C0)`; bail if null
   (BindShadow inactive in this scene → no shadow, nothing to do).

2. **Capture AFTER the real field's draw, BEFORE its FINALIZE empties the array.** The
   safe seam is an **override tee on `drawShadow` (0x8022f014)** (and `drawShadowGD`
   0x8022fa40 for the GD variant): in the tee, call the real function, then — only on the
   REAL field (`!g_interp60_in_redraw`) — snapshot:
   - `drawCount = MEM_R32(mgr + 0x20)`
   - `drawBase  = MEM_R32(mgr + 0x1C)`
   - copy `drawCount * 0x14` bytes from `drawBase` into a host buffer, and remember
     `drawCount`.
   (Capturing in the `drawShadow` tee guarantees the array is fully built and not yet
   zeroed by FINALIZE, which happens later in the same `perform` after the DRAW block.)

3. **On the in-between, restore the snapshot into the manager BEFORE the draw lists
   re-issue**, and **suppress the in-between's RESET/`calcVtx`** so it can't overwrite the
   restored array with an empty rebuild. Two ways:
   - **Cleanest:** in the same `drawShadow`/`drawShadowGD` tee, when
     `g_interp60_in_redraw` is true, write the snapshot back into the manager
     (`MEM_W32(mgr+0x1C, drawBase)` — the base ptr is stable across the frame, so really
     just `MEM_W32(mgr+0x20, savedDrawCount)` and restore the entry bytes at `drawBase` if
     the FINALIZE/RESET clobbered them), then call the real `drawShadow`. Because the
     manager's RESET block (`&4`) runs `calcVtx` *before* the DRAW block in the same
     `perform`, you must additionally **mask off `&4`** on the manager's in-between
     `perform` so `calcVtx` does not rebuild-to-empty first. Do this with a tee on
     `TMBindShadowManager::perform` (0x80231108): when `g_interp60_in_redraw`, set
     `cpu.gpr[4] &= ~0x4u` (drop RESET/calcVtx) and `&= ~0x20000000u` (drop FINALIZE so the
     restored counts survive), keep `&8` (DRAW), restore `drawCount`+entries beforehand,
     then call the real perform.

### Exact offsets to snapshot / restore

| what | manager offset | size |
|---|---|---|
| draw array base ptr | `mgr + 0x1C` | u32 (stable per frame; usually no need to restore) |
| draw entry count | `mgr + 0x20` | u32 |
| draw entries | `*(mgr+0x1C)` … `+ count*0x14` | `count * 0x14` bytes |

(If the secondary aux array is ever needed: base `mgr+0x70`, count `mgr+0x40` (u16),
stride 0x14 — but `drawShadow`'s loop reads only `+0x1C`/`+0x20`, so the aux array is not
required for the cast-shadow draw.)

Manager global: **`mgr = MEM_R32(0x8040E0C0)`** (`= [r13 - 0x6100]`, r13 = 0x804141C0).
Perform tee addr **0x80231108**, drawShadow tee **0x8022f014**, drawShadowGD **0x8022fa40**.

### Why this is faithful, not a bandaid

It does not fabricate or re-derive shadow geometry, run actor calc, or magic-constant
anything: it replays the EXACT draw array the real field built, at the same draw point, on
the in-between — the same principle as the existing `interp60` draw-list re-issue. The only
manager-state mutation is suppressing the in-between's own RESET/FINALIZE (which would
otherwise zero a queue we know is intentionally not refilled at 60 Hz). Optional polish:
blend the snapshot entries' world position toward N-1 (like the `gpMarioPos`/registry
blends) so the cast shadow interpolates smoothly instead of holding tick-N for both fields;
the shadow entry's position lives at the start of each 0x14-byte draw entry (Vec from
`TCircleShadowRequest.unk0`, carried through `calcVtx`). Ship the on/off fix first; add the
blend as a follow-up if the held-position step is visible.

---

## 5. Verification plan (when implementing)

- Confirm `mgr = MEM_R32(0x8040E0C0)` is non-null in gameplay (Delfino), and that
  `MEM_R32(mgr+0x20)` (drawCount) is > 0 on real fields and **0 on the in-between
  before the fix** (proves the empty-queue cause). Add to `/interp60` probe output.
- After the fix: drawCount on the in-between equals the captured real-field count; the
  cast shadow is present on both fields (no on/off).
- A/B with `SUNBRIGHT_INTERP60` off vs on, and against the `SUNBRIGHT_DISABLE_RECOMP`
  oracle (30 fps) for shadow shape/position parity.

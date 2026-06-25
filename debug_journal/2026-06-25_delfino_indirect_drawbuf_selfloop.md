# Delfino gameplay hang: `J3DDrawBuffer::drawHead` infinite self-loop (indirect draw buffer)

**Date:** 2026-06-25
**Status:** FIXED (engine no longer hangs; native gameplay loop runs continuously)
**Scope:** extending the SMS decomp into a PC-native engine — first gameplay-scene blocker.

## Symptom
The native engine (`sms-boot`) fastboots straight to **Delfino Plaza gameplay**
(`APP_STATE_GAMEPLAY`, stage 1) by default. It loaded the stage, excluded the NPC roster
(deliberate), then **hung** — the 20s watchdog fired with the running thread inside
`J3DDrawBuffer::drawHead` (`TMarDirector::direct → TPerformList::perform →
TViewObjPtrListT::perform → drawHead`). Every gameplay run hung within 20s; file-select
(stage 15) did not (no indirect effect there).

## Root cause (full chain, value-verified)
`drawHead` walks each `mBuffer[slot]` packet chain via `getNextPacket()`. The hang is a
**self-referential draw node**: `mBuffer[0] == packet` and `packet->next == packet`
(`selfloop=1`), so the inner walk never terminates.

The buffer is **"DrawBuf Indirect"** (matched via a `TDrawBufObj` name→buffer inventory),
the indirect-texture draw buffer used by Delfino's water sheen. The cyclic packet is a
`J3DMatPacket` whose `unk3C = 0xc0000000` — bit 31 set. In `J3DDrawBuffer::entryMatSort`
that takes the **no-merge "always prepend to slot 0"** branch (the hashed `else` branch
dedups via `isSame` and would have merged a re-entry harmlessly; the bit-31 branch does
**not** dedup). Bit 31 is set faithfully: `J3DMaterial::change()` does `unk18 |= 0x80000000`,
and `TShimmer::loadAfter` calls `change()` on every shimmer material; `makeDisplayList`
then propagates it (`matPacket->unk3C = unk18`).

The double-entry comes from **`TShimmer::perform`** (`Map/Shimmer.cpp`):
- `param_1 & 0x4`  → `unk48->entry()`   (J3DModel::entry → recursiveEntry → entryIn → entry)
- `param_1 & 0x200`→ `unk48->update()`  (J3DModel::update → recursiveUpdate → updateIn → entry)

The indirect scene (`インダイレクトシーン`) is pushed into the perform list with flag
**`0x40000204`** (`MarDirectorPreEntry.cpp`), which has **both** 0x4 and 0x200. The
`update()` guard `gpMarDirector->mMap == 2 || !(gpCamera->unk124.y < 0)` is true
(`mMap=1`, `unk124.y = 612.3` — the camera **eye height**, correctly above water), so
both branches run. "DrawBuf Indirect" is `frameInit`'d once (clears `mBuffer[0]`) before
the indirect scene runs, so within one frameInit window the same no-merge packet is
entered twice:
1. entry #1 (`entry()`): `mBuffer[0] = packet`, `packet->next = null`.
2. entry #2 (`update()`): `drawClear()` → `packet->next = null`, then
   `packet->next = mBuffer[0] = packet` → **self-loop**.

`drawHead` then spins on that node.

## Open question (documented honestly)
By this logic GC would also self-loop, yet the retail game does not hang — so on GC the
no-merge packet is presumably entered once per frameInit window (some state/order
difference not yet pinned; not verifiable without the GC oracle for the plaza). That is a
*fidelity* question. The hang itself is an **engine-robustness** defect independent of it.

## Fix
`J3DDrawBuffer::entryMatSort`, bit-31 (no-merge) branch, under `SMS_NATIVE_PLATFORM`:

```cpp
if (mBuffer[0] == packet)   // already the slot head -> re-prepend is a pure no-op
    return true;            // (never form a self-referential cycle)
```

Re-prepending a packet that is **already** the slot head is meaningless — it is already
entered, at the head. Skipping it keeps the model entered exactly once and makes a
self-link impossible. This is a strict draw-list invariant (a node linked to itself can
only ever produce an infinite `drawHead`/`drawTail` walk), consistent with the project
direction of a robust PC-native engine rather than bug-for-bug GC emulation.

## Verification
- Before: every gameplay run → watchdog "no forward progress" in `drawHead` within 20s.
- After: 45s continuous run, reached `APP_STATE_GAMEPLAY`, **0** hang/cycle/abort markers
  even with the opt-in `SB_DRAWBUF_CHECK=1` tripwire enabled; present frames advance
  (201→208 under `SB_FRAME_DUMP`).

## Tooling added (env-gated, OFF by default)
- `SB_DRAWBUF_CHECK=1` — Floyd cycle tripwire in `drawHead` (aborts naming buffer+packet
  if any future no-merge path re-introduces a self-link).
- `SB_INDIRECT_DBG=1` — per-call flag trace of the indirect/after-indirect `TDrawBufObj`s
  (frameInit/setBuf/draw decode).
- `SB_SHIMMER_DBG=1` — `TShimmer::perform` flag + branch-condition trace (entry/update,
  mMap, camera eye-y, FLUDD).
- `SB_DRAWBUF_INV=1` / `SB_NO_DRIVE_SCENE=1` — `scene_drive.cpp`: DrawBufObj name→buffer
  inventory, and a bisection gate to attribute draw-buffer state to the native drive vs
  the real perform list.

## NEXT (separate frontier — not the hang)
The native renderer does **not** yet capture the gameplay 3D scene: gameplay present is
`scene_verts=0 scene_batches=0` (only the J2D overlay draws) → black frame. The capture
path (`sms_boot_j3d_capture` / `scene_drive`) was built for the file-select scene. Driving
the gameplay scene capture is the next step toward a visible Delfino render. Also still
open at the gameplay boot: an early `SolidHeap OOM (0x1950)` and the unimplemented
`hx_wipe type 1` entry-transition callback (logs, no longer hangs).

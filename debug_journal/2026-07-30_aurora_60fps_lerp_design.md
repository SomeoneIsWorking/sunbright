# Interpolated 60fps in aurora — the design, and the two premises that decided it

**Status: design settled, implementation in progress.** User directive (2026-07-30): *"Direct all
focus to lerp 60fps Aurora until it is done, make sure all fullscreen effects and anything else take
lerp into account."* Aurora is our fork (`extern/aurora`, branch `sunbright`, remote `fork`), so
aurora-side changes are ordinary work here.

This supersedes the 2026-07-29 note that concluded *"aurora cannot do sound interpolated 60fps"*.
That conclusion rested on aurora being a third-party black box we could not modify. It is not.

---

## The shape of the problem

The game ticks at 30 Hz on the process main thread. Aurora is an immediate-mode GX replay: FIFO
parse → `handle_draw` → per-frame staging buffers → `end_frame` → present. Exactly one present per
tick, from `sms-recomp/overrides/native_frame.cpp` (`gxfifo_send_last(); present_and_reopen();`).

Interpolation is possible at all because **a tick contributes matrices, not geometry**. Model-space
vertices do not change between ticks; animation moves the matrices. So a 60 Hz render of a 30 Hz
simulation is the same geometry drawn twice with different transforms — not two scene submissions.

## Premise 1: the uniform block — CONFIRMED

Every GX draw's uniform block carries the **entire 10-entry pnMtx array** (pos *and* nrm) plus the
10 texture matrices, at a fixed computable offset, written into the frame's `uniforms` staging at
`DrawData.uniformRange.offset` and patchable in place while the frame is recording.

`lib/gx/shader_info.cpp:455-465`:

```cpp
for (int i = 0; i < MaxPnMtx; i++) { buf.append(g_gxState.pnMtx[i].pos); }
for (int i = 0; i < MaxTexMtx; i++) { buf.append(g_gxState.texMtxs[i]); }
for (int i = 0; i < MaxPnMtx; i++) { buf.append(g_gxState.pnMtx[i].nrm); }
```

Layout (from the append order at `shader_info.cpp:430-465`; `MaxPnMtx = MaxTexMtx = 10`,
`Mat3x4<float>` = 48 B, `MaxIndexAttr = 12`):

| offset | bytes | field |
|---|---|---|
| 0 | 4 | `vtxStart` |
| 4 | 4 | `currentPnMtx` — active PN index |
| 8 | 16 | render + logical viewport |
| 24 | 8 | pad |
| 32 | 48 | `vaRanges[12].offset` |
| 80 | 16 | lineMode block — **present only if `info.lineMode != 0`** |
| P | 64 | `proj`, P = 80 or 96 |
| **P+64** | **480** | **`pnMtx[0..9].pos`** ← lerp target |
| P+544 | 480 | `texMtxs[0..9]` ← lerp target (scrolling UVs, `J3DTexMtx` anims) |
| **P+1024** | **480** | **`pnMtx[0..9].nrm`** ← lerp target |
| P+1504 | var | animated tail, below |

`.pos` is at **144** (160 with lineMode). The offset depends on `ShaderInfo.lineMode`, which is
**not** stored in `DrawData` — a patch pass has to thread it through or reconstruct it.

All 10 slots matter: skinned J3D geometry (`J3DShapeMtxMulti`) selects among them per-vertex via
`PNMTXIDX`.

**Everything after the matrices is also per-tick animated state**, in this order, each conditional
on a `ShaderInfo` bit (so tail offsets are per-draw variable) — `shader_info.cpp:467-535`:

1. `colorRegs[i]` per `loadsTevReg` bit — **animated** (TEV colour anims, BRK/TRK)
2. `lights` (80 B × `MaxLights`) + 4 × `lightMask`, if `lightingEnabled` — **animated**; light
   positions/directions step at 30 Hz. *This is the most visible residual after matrices.*
3. `ambColor` / `matColor` per sampled channel — animated (material colour anims)
4. `kcolors[i]` per `sampledKColors` bit — **animated**
5. `ptTexMtxs[0..19]` (960 B) if `usesPTTexMtx.any()` — **animated** (post-transform texgen)
6. `Fog` if `usesFog` — rarely
7. `texCoordScales` — no
8. `indTexMtxs` if used — **animated** (indirect/warp)
9. `texture_size_bias` per sampled texture — no

None of this dangles; it is all plain value data. The failure mode is purely *"this quantity steps
at 30 Hz while geometry moves at 60 Hz"* — which is exactly what the per-cell alternation
instrument is built to localise.

Hard cap: `MaxUniformSize = 3840` (`gx.hpp:69`), enforced at `shader_info.cpp:346-348`, and also
the fixed bind-group binding size (`common.cpp:1072`).

## Premise 2: re-executing a recorded frame — DESTROYED

A recorded frame is **not** re-executable after the fact, for two independent reasons.

**(a) The CPU record and the staging mapping are torn down inside `end_frame`'s worker callback**
(`lib/gfx/common.cpp:1366-1387`): `g_stagingBuffers[stagingSlot].Unmap()` kills the CPU pointers and
`packet = {}` destroys `renderPasses`/`commands` — both *before* the frame is submitted. Any retain
scheme must copy **during recording**, never after.

**(b) `Range`s are offsets into globally shared buffers that the next frame overwrites from zero.**
There is one `g_vertexBuffer` / `g_uniformBuffer` / `g_indexBuffer` / `g_storageBuffer`
(`common.cpp:983-990`), and every frame copies its staging in starting at offset 0
(`copy_staging_buffer_range`, `common.cpp:1416-1426`). Correct today only because GPU work executes
in submission order. So a retained frame's `uniformRange.offset` is meaningless once a later frame's
copies execute — a frame is re-submittable only back-to-back within the same tick.

**Re-parsing the FIFO twice is also unsound.** The parse is not idempotent; four record-side
mutations differ on a second pass: `g_gxState.stateDirty = false` (`shader_info.cpp:536`) changes
merge decisions, `array.cachedRange` (`command_processor.cpp:4003-4012`) makes the second parse
reuse ranges instead of pushing, `arr.sizeAuto` grows monotonically (`:2954-2962`), and
`resolve_sampled_textures` short-circuits on `texObjId`/`texDataVersion` (`gx.cpp:458`). Two
differently-shaped recordings.

## The design: record once, emit twice, share the geometry upload

The obvious reading of premise 2 is "snapshot the whole staging and replay it" — a **~33 MB memcpy
per tick** on a measured Delfino frame (`common.hpp:191-204`), out of write-combined mapped memory.
That is the expensive shape and it is not necessary.

The cheap shape falls out of the same facts. Packet A (lerped matrices) and packet B (the tick's
true matrices) are the SAME scene: identical geometry, identical draw commands, **identical `Range`
values**. They differ only in the matrix bytes inside each uniform block. So:

* **B skips its vertex / index / storage staging copies entirely** and reuses what A already placed
  at exactly those offsets in the global buffers. Premise 2(b) — that a later frame stomps the same
  bytes — is not an obstacle here, it is the mechanism: B *wants* A's bytes, and nothing runs in
  between.
* **Only the uniforms are rebuilt**, because only they differ.
* **B's command lists are a plain CPU-side copy** of A's — `CommandList` is `std::vector<Command>`
  of trivially-copyable data; ~1500 draws ≈ 150 KB. Trivial.
* A normal-RAM **shadow of the uniform bytes** is appended alongside the staging write, so B's
  uniforms are memcpy'd from cacheable memory rather than read back out of write-combined staging.

Per-tick extra cost: the command-list copy plus one uniform-sized write, instead of ~33 MB.

Present order is **A (alpha 0.5, interpolated) then B (alpha 1.0, the tick's own state)** — standard
interpolation, which buys smoothness at half a tick of latency. Objects present in tick N but not in
N-1 have no previous matrices and **snap**, which is correct.

Draw merging (`command_processor.cpp:3056-3101`) requires `!g_gxState.stateDirty`, and XF matrix
loads set it — so **merged prims genuinely share one matrix set** and one uniform patch per
`DrawData` is semantically right. Merging breaks *counting*, not interpolation.

## Identity: supplied by us, never derived

Pairing tick N's draws with tick N-1's needs a stable key.

**A content hash inside aurora cannot work.** For indexed J3D geometry — essentially all scene
geometry — the bytes at `verts[vertRange]` are an **index stream**, not vertices; the attribute data
goes to `frame.storage` via `push_storage` and is reached through `vaRanges` carried in the
*uniform* (`lib/gx/shader.cpp:764-774`). Hashing `verts` hashes indices. Aurora has no existing
content hash to reuse: `pipeline_cache.cpp:436`, `shader.cpp:965`, `common.cpp:1797` all hash
*state and handles*, and `GXTexture.cpp:43`'s `content_tex_obj_id` is misnamed — it hashes the data
*pointer*, not texels.

**An ordinal cannot work at all**, and this codebase has an unusually sharp version of that trap:
`g_drawCallCount`, `g_mergedDrawCallCount`, `g_sbPushedDrawCount` and the game's own draw count are
four different numbers, and merging is *state-dependent*, so the same scene yields different draw
ordinals when a pass boundary or state write lands differently. Pairing on an ordinal each side
counts for itself would be the seventh instance of the failure mode catalogued in `CLAUDE.md`.

**The viable key is the guest `J3DShape` pointer**, composited with element and pass index — a
persistent scene-graph address that both ticks name identically. We already capture it
(`sms-recomp/overrides/j3d_capture.cpp`, override of `0x802e0390`). It reaches aurora through a new
`GX_AURORA` sub-opcode setting a pending tag, plus a `tag` field on `DrawData`; on merge the head's
tag wins. The sub-ordinal within a tag is scoped to that tag and resets at each one — it is not a
global ordinal, and it is stable because the same shape replays the same display list each tick.

## What else must take the lerp into account

Per the user's requirement that *all* full-screen effects respect it:

| element | decision | why |
|---|---|---|
| pnMtx pos/nrm | **LERP** | the whole point |
| texMtxs | **LERP** | scrolling UVs / `J3DTexMtx` anims, same cost, inside the fixed region |
| EFB copies / `resolve_pass` | **RE-RUN per emission** | each emission runs its own copies into the same destination and its later passes sample what that emission wrote — self-consistent, at double cost |
| lights | **LERP** (after matrices) | positions/directions step at 30 Hz; the most visible residual |
| TEV regs, K colours, mat/amb colours | LERP | colour anims otherwise pop at 30 Hz |
| ptTexMtxs, indTexMtxs | LERP | animated texgen / indirect warp |
| 2D / HUD / J2D | **SNAP** | ortho draws have no meaningful in-between; interpolating them smears |
| particles (JPA) | **SNAP** | immediate geometry regenerated per tick, no stable identity |
| depth snapshot (`GXPeekARGB`) | **SUPPRESS on the interpolated emission** | `depth_peek` is request-driven and rate-limited (`depth_peek.cpp:352-357`), so whichever emission runs first consumes the request — the game would read back depth from a state it never simulated |
| `observable` | no action | pure skip optimisation, identical both emissions |

## Verification

`sms-recomp/runtime/frame_smoothness.{h,cpp}` (committed `01fdeb9`), armed with `SBR_SMOOTH=1`.
Validated against **both** classes on a moving scene:

| | dupFrac | mean alternation |
|---|---|---|
| real stream (negative) | 0.018 | 0.147 |
| each frame fed twice (positive) | 0.519 | 1.000 |

The per-cell alternation grid is printed as a map, so a lerp-off and a lerp-on run diff directly: a
cell whose alternation rises only when interpolation is on is a region ignoring the lerp. That is
how the "all full-screen effects" requirement gets *checked* rather than asserted.

It measures **evenness, not correctness** — a wrong-but-even interpolation scores perfectly. Colour
correctness stays with the A/B against the oracle.

## Ladder

1. ✅ Instrument, validated both directions (`01fdeb9`).
2. Dual-emit at **identical** matrices — the in-situ duplicate control. Instrument must read
   dupFrac ≈ 0.5, alternation ≈ 1.0. Proves the double-present machinery without any lerp.
3. Draw tag plumbing + pairing-rate report (with a control that must drop it).
4. Lerp pnMtx at alpha 0.5. Instrument must flip to dupFrac ≈ 0, alternation low. **The milestone.**
5. texMtx, then lights, TEV/K colours, ptTexMtx, indTexMtx — each A/B'd on the alternation grid.
6. depth-peek suppression; EFB-copy self-consistency; 2D/particle snap rules.
7. Present pacing: half a field apart, measured with `SBR_PRESENT_TIMING` (alternating means must
   both be ~16.7 ms — back-to-back presents followed by a long gap read as 60 in a counter and 30 to
   the eye).

Default-OFF gating throughout, so with the flag off aurora renders bit-identically and remains the
parity oracle for `render_compare.cpp`.

# The dash-blur afterimage at 60fps, and where the frame budget actually goes

Two user-reported problems from playing with `SBR_60FPS=1`: the ghost/afterimage trail behind Mario
is jittery, and performance is poor. They have different causes and neither is guesswork below.

---

## The ghost is `TAfterEffect`, and interpolation corrupts its feedback loop

**What it is** (`decomp/sms/src/MarioUtil/ScreenUtil.cpp`, `TAfterEffect`): a full-screen
temporal-feedback blur. `TScreenTexture` holds an EFB copy of the frame (half render size,
`GX_TF_RGB565`), and `TAfterEffect::perform` draws an 8-vertex `GX_TRIANGLEFAN` covering the
viewport, textured with that copy, blended `GX_BL_SRCALPHA/GX_BL_INVSRCALPHA`. The previous frame is
sampled slightly scaled and offset:

```
fVar4 = unk40 * -0.5f + 0.5f + unk38;   // u0   unk38/unk3C = offset, unk40/unk44 = scale
fVar6 = unk44 * -0.5f + 0.5f + unk3C;   // v0
```

so each frame smears the last one outward — the trail. Its parameters are themselves exponentially
smoothed **once per frame**, toward targets set by the dash state:

```
unk20 += unk48 * (unk1B - unk20);   // unk48 = 0.05, alpha of the smoothing
unk38 += unk48 * (unk28 - unk38);
...
unk50 = unk50 - unk54;              // calcDashBlurValue, unk54 = 0.01 per frame
```

The geometry is `GX_DIRECT` immediate mode, in screen space, with `GXLoadPosMtxImm(viewMtx)`.

**Why it jitters under interpolated 60fps.** The effect is a *feedback* loop: this frame's output
becomes next frame's input. Our replay path presents each tick twice and, by design,
**re-runs the recorded EFB copies on both emissions** (`resolveTarget` is stored on the render pass,
and both emissions replay the same pass list). So the screen texture is written **twice per tick,
with two different images** — once from the interpolated pose at t−0.5 and once from the true pose
at t. The trail therefore alternates between two sources every present, and the feedback advances at
double rate, halving the trail's decay time.

Re-running EFB copies per emission is *correct* for intra-frame copies, where a later pass of the
same emission must sample what that emission wrote. It is *wrong* for a copy that feeds the NEXT
frame. Those are two different things sharing one mechanism.

There is already a precedent for exactly this distinction in the code: `install_replay_snapshot`
sets `captureDepthSnapshot = false` on the replay emission, because otherwise "the game could read
back depth belonging to a frame state it never simulated". The screen texture is the same class of
bug with a visible symptom.

**Fix design (not yet implemented).** The screen-texture copy must happen exactly ONCE per tick, and
it should be the tick's TRUE image, so the interpolated emission — presented first — composites the
trail as captured by the previous tick. That means suppressing *that specific* resolve on the
interpolated emission, identified by its destination (the `JUTTexture` image buffer
`gpScreenTexture->unk10` allocates), **not** by suppressing EFB copies wholesale — the sea
reflection and indirect passes are intra-frame copies that must keep running on both emissions.

Note the smoothing constants (`0.05` per frame, `0.01` per frame) do NOT need rescaling: game logic
still ticks at 30 Hz under render interpolation, which is the whole point of the design. Only the
capture rate is wrong.

## Where the frame budget goes

Measured at the frame seam, splitting the tick into the guest's own recompiled code and everything
we do to present it (`frame` channel, unpaced so no sleep is included in either):

| | guest logic | present + render | total |
|---|---|---|---|
| 60fps off | 19.2 ms | 14.7 ms | 33.9 ms |
| 60fps on | 22.5 ms | 20.6 ms | 43.1 ms |

A 30 fps tick budget is 33.3 ms. **The port is at the edge of full speed before interpolation is
involved**, and the single largest item is the recompiled PPC game logic at ~19 ms — not rendering.
Interpolation adds ~6 ms of rendering, which is what pushes it over.

**These absolute numbers are inflated**: the machine carried a load average of ~28 from unrelated
builds and another port running throughout. The RATIO is the durable finding — roughly 57% guest
logic, 43% render — and it says where optimisation has to happen. Re-measure on an idle machine
before treating any absolute figure here as a baseline.

Leads, in the order their size suggests:

1. **Guest logic ~19 ms/tick.** This is recompiler output quality, a separate arc from rendering.
   Nothing in the render path can fix it, and interpolated 60fps is valuable precisely BECAUSE it
   does not re-run it.
2. **`storage=22.4 MB` uploaded per tick** (`AURORA_REPLAY_LOG_EVERY`) — the indexed vertex
   attribute arrays. **Checked: the per-array upload cache (`array.cachedRange`) is working
   correctly and cannot help.** It is invalidated at every `end_frame`
   (`common.cpp`, the `for (auto& array : gx::g_gxState.arrays) array.cachedRange = {}` loop) and
   that invalidation is *required*: there is one global storage buffer and every frame re-copies its
   staging starting at offset 0, so a range cached from the previous frame would point at bytes the
   current frame has since overwritten. The cache is within-frame only, by construction.

   So the re-upload is architectural, not a bug. Removing it needs a PERSISTENT storage buffer for
   arrays whose backing memory has not changed, which in turn needs guest-write detection on that
   memory — deformable/animated geometry rewrites its arrays, so "the pointer is the same" is not
   sufficient to prove the contents are. That is a real piece of aurora work, not a tweak.

   The replay emission already skips this upload entirely (it pushes no verts/indices/storage, so
   `highWater > copied` is false and no copy is emitted), so interpolated 60fps does NOT double it.
3. Per-draw build work is ~4.2 ms/frame, of which `arrayUpload` is ~2 ms — consistent with (2).
4. Possibly compounding (2): the `StorageBufferSize` comment records that a redundant phase-1 "ghost
   pass" roughly DOUBLES per-frame storage, and CLAUDE.md lists that double-draw as open and
   unverified. If it is real, removing it halves both this transfer and the draw work. Worth
   settling before optimising anything downstream of it.

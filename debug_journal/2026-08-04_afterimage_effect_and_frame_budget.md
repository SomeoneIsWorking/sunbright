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

---

## The once-per-tick feedback copy was WRONG, and the title proved it

Landed, then measured, then turned off. Recorded because the reasoning was sound and the premise was
not — and the premise is the reusable part.

**The regression.** With the feedback-copy suppression active, the title screen's background
alternated between the sky and BLACK on every present. Measured with `SB_DUMP_FRAME_EVERY=1`, mean
luminance over consecutive presents:

    174.1  51.2  174.1  51.2  174.1  51.2 ...

**The controls, run before diagnosing:**

| configuration | result |
|---|---|
| doubled present, NO interpolation (`AURORA_REPLAY_PRESENT=1` alone) | 174.1 constant — clean |
| suppression ON, interpolation NEUTRAL (`AURORA_INTERP_ALPHA=1.0`) | alternates — **broken** |

So it was neither the doubled present nor the interpolation: it was the suppression itself. Without
the second control this would have been blamed on EFB non-idempotence, which the design doc predicts
and which was the obvious suspect.

**Why the premise is false.** The claim was *"whatever `TAfterEffect` samples is by definition the
cross-frame feedback texture."* It is not. The same `TScreenTexture` copy is consumed INTRA-frame by
other things — the title composites its sky through it — so dropping the copy on the interpolated
emission left the same frame's later pass sampling nothing.

Narrowing it to "only while the trail is actually drawing" does **not** rescue it: measured, the
title still alternates, because there the effect draws *and* the copy is still intra-frame.

**So identity cannot separate the two cases at all.** The only property that can is **order within
the frame**: a copy whose texture is sampled BEFORE it in the pass list is being read from the
previous frame (cross-frame, suppress on the interpolated emission); one sampled AFTER it is
intra-frame (must run on both). Aurora does not track when a texture is sampled relative to the copy
that writes it. Adding that is the real fix and the actual next step.

**Current state:** `SBR_FEEDBACK_COPY_ONCE=1` enables the suppression; it is OFF by default. The
dash trail keeps its 60fps jitter — a cosmetic defect on one effect — rather than blanking the
background of every other frame, which is not cosmetic. Verified after gating: title max adjacent
per-present difference 0.12 over 200 presents (123.01 when broken), plaza 1.14.

**The lesson worth keeping:** an EFB copy's identity says nothing about whether its consumer is this
frame or the next, and that distinction is the whole question. Any future attempt at this must
derive the answer from pass ORDER, and must be checked on the title as well as gameplay — the two
scenes use the same texture for opposite purposes.

---

## The feedback-copy theory is FALSIFIED (2026-08-05), by an instrument built to test it

The identity-based attempt above was replaced with the discriminator it named — **order within the
frame** — and that discriminator then refuted the whole diagnosis.

**The rule.** A copy is cross-frame feedback iff its result was sampled during the frame and every
such sample was in a STRICTLY EARLIER pass than the one whose resolve writes it: those samples can
only have read what the previous frame left. Sampled later, or in the SAME pass (the record has pass
granularity, so a same-pass sample cannot be shown to precede the resolve), a consumer in THIS frame
depends on it and it must run on both emissions. `is_cross_frame_feedback` in `gfx/common.cpp`.

Recorded where the information actually exists: `resolve_sampled_textures` already looks a sampled
texobj's data pointer up in the copy-texture map, so "this draw sampled the copy for dest D" is known
there — noted *before* the unchanged-bind early-out, or most samples would be missed and an
intra-frame copy would look like feedback.

**Validated on both classes** (`copy_classifier_selftest`, runs before the first real tick):
sampled-before → feedback; sampled-after → intra-frame; same-pass → intra-frame; never-sampled →
refused. All four, not just the one it is expected to find.

**And a POSITIVE CONTROL for the real code path**, because no automated run ever dashes:
`SBR_FORCE_DASHBLUR=1` forces the real `TAfterEffect` to draw every frame (state 2, enable bit set,
a dash amount that does not run down). Verified it works: **1791 real trail draws** over a plaza run
where an unforced run has zero.

**The result, with the trail genuinely drawing:**

    EFB copies over 2400 ticks: 0 suppressed (cross-frame feedback) and 7200 kept (intra-frame)

**Zero.** Not "the effect never drew" — it drew 1791 times. The copy the dash trail samples has a
consumer LATER IN THE SAME FRAME, so it is not a temporal feedback copy at all, and it was never
being "written twice per tick from two different images".

**Therefore the stated cause of the ghost jitter is wrong.** Everything downstream of it — the
once-per-tick suppression, the identity plumbing, the `drawing` gate — is deleted rather than left
as a disabled tombstone. What remains is the classifier (self-tested, currently reporting no
feedback copies in either scene, and demonstrably harmless: title max adjacent per-present
difference 0.10, plaza 1.13) and the forced-draw control.

**The jitter itself is UNDIAGNOSED again.** What is now known and should not be re-derived:

* it is not the doubled present (that control is clean at 174.1 constant);
* it is not a twice-written feedback texture (measured zero, with the effect forced on);
* the trail quad is `GX_DIRECT` immediate-mode screen-space geometry, so the matrix interpolation
  path cannot reach it — it is in the 42304 direct draws that correctly snap;
* the effect's own state advances once per TICK and needs no rescaling.

The next hypothesis to test is the one the above leaves standing: the quad is rebuilt per tick from
smoothed parameters and SNAPS while the scene around it interpolates, so the trail is a half-tick out
of step with the geometry it trails. That is a vertex-data problem, not a copy-scheduling one, and it
needs a different mechanism than anything tried here.

### And the whole-frame smoothness metric is the WRONG instrument for this

With the trail forced on vs off, mean alternation is 0.332 -> 0.345 — inside the run-to-run spread,
and the judged-cell counts differ (185 vs 174), so the two are not strictly comparable anyway.

That is **not** evidence the trail is smooth. `frame_smoothness` scores whole screen CELLS, and the
trail is a low-alpha overlay whose cells are dominated by the scene geometry underneath it. It also
carries the blind spot printed with every one of its reports: a large but CONSISTENT displacement
reads as perfectly even motion.

So the next attempt needs an instrument that looks at the TRAIL, not at the frame: sample the region
where the ghost is (with `SBR_FORCE_DASHBLUR=1` making it reproducible headlessly) and compare that
region across consecutive presents. Reusing the frame metric here would produce another confident
number about something it never measured — the failure this project has catalogued seven times.

---

## Chasing the trail produced a bigger number: ~39% of on-screen motion still snaps

The trail-specific instrument the section above asked for was built (`tools/present_evenness.py`),
and it says the trail is **not** the problem — but it quantifies something more important.

**Method.** Interpolation presents each tick twice. If everything interpolated, consecutive presents
would advance the picture equally; anything that still SNAPS moves only on the true present, so all
of its motion lands in every other step. Measure the mean pixel difference between consecutive
presents and split it even/odd. Weighting is by SCREEN AREA, which is what the eye integrates —
unlike the draw-count percentages the tag-coverage report gives, and unlike `frame_smoothness`, which
scores whole cells and cannot see a low-alpha overlay.

Scene: Delfino plaza, Mario running (`SBR_PAD_SCRIPT="150:STICK=0/-90"` — a static scene measures
nothing).

| run | even step | odd step | ratio |
|---|---|---|---|
| **control**, 60fps OFF (presents == ticks) | 0.373 | 0.366 | **1.02** |
| 60fps ON, dash trail off | 0.272 | 0.119 | **2.28** |
| 60fps ON, dash trail FORCED on | 0.271 | 0.120 | **2.26** |

**The trail changes nothing** (2.28 vs 2.26) — with 2991 real trail draws in that run. So the ghost
is not what makes the picture uneven, and the earlier hypothesis that it "snaps while the scene
interpolates" is not supported either.

**The control is 1.02**, which is what makes the rest meaningful: the scene does not pulse on its
own, so the imbalance belongs to interpolation.

**Total motion is conserved** (0.391/tick interpolated vs 0.370 control): nothing is being added or
lost, it is being split unevenly — one step gets ~70% of a tick's movement and the next ~30%.

For a snapping share `s` of on-screen motion, the ratio is `(1+s)/(1-s)`. At 2.28 that gives
**s ≈ 0.39: about 39% of what moves on screen still steps at 30 Hz.**

That is far above what the draw-count coverage suggests (65.6% of draws tagged, ~9.8% untagged
indexed perspective), and the discrepancy is the point: coverage counted DRAWS, and the draws that
snap — sea and water surfaces, particles, immediate-mode effects — cover a disproportionate share of
the SCREEN. A percentage of draws was never the quantity that determines whether motion looks smooth.

**This is now the headline number for the 60fps arc**, and it is the one to drive down. The residuals
already named (indexed perspective geometry not reaching the tag seam; the 2.9% mispairings) should
be re-prioritised by screen area rather than by draw count.

### First attribution: the 2D layer is NOT the snapping content

The obvious suspect was the 2D/HUD layer, which is *supposed* to snap (an ortho pane has no
meaningful in-between, and interpolating it slides the HUD bodily every other frame). If most of the
39% were legitimate 2D snapping, the number would be nothing to chase.

Ablated with `SB_SKIP_ORTHO=1`, same scene and window:

| run | even | odd | ratio | motion/tick |
|---|---|---|---|---|
| 60fps on, all draws | 0.272 | 0.119 | 2.28 | 0.391 |
| 60fps on, **ortho skipped** | 0.454 | 0.173 | **2.62** | 0.628 |

Removing 2D makes the imbalance WORSE, not better — the snapping share goes 39% -> 45%. And total
motion per tick rises sharply (0.391 -> 0.628), which says the ortho layer was covering a
substantial amount of moving 3D content and damping the measurement.

So the unevenness lives in the **3D scene**, and legitimate 2D snapping was if anything hiding it.
The candidates by screen area are the ones already named as untagged: sea and water surfaces
(immediate-mode geometry whose vertices are rebuilt per tick by `calcDrawVtx`), particles, and the
indexed perspective draws that never reach the tag seam.

Next: attribute by SCREEN AREA rather than by ablation guesswork — mark the draws that did not
pair-interpolate and measure the fraction of pixels they cover. Draw-count percentages have already
proved to be the wrong denominator twice in this arc.

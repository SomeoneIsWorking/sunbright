# 2026-08-07 — the dash-trail ghost: three hypotheses, all falsified by measurement

User report: with 60fps on, the dash-trail ghost judders. Three explanations were plausible enough
to act on. All three are now dead, each by a measurement rather than an argument. Recorded so none
of them is proposed a fourth time.

## 1. "The feedback copy advances once per tick while Mario moves twice" — FALSE

The interpolation suppresses cross-frame feedback copies on the interpolated emission, leaving them
to the replay emission, so a temporal effect advances once per game tick. That is real code and it
is what the previous sessions' notes blamed.

It never fires. Measured over 13,800 ticks: **0 copies suppressed, 55,198 kept**. So
`AURORA_EFB_FEEDBACK=present`, added to disable the suppression, changes **0.000% of pixels** on
every present of a matched-tick pair. Nothing was being held back.

## 2. "The classifier is broken, so feedback is never recognised" — FALSE

"0 suppressed" has two opposite causes: no copy is feedback, or nothing ever told the classifier a
copy was sampled. aurora printed only the verdict, so the two were indistinguishable. It now prints
the classifier's **input** beside it — accepted/refused sample notifications and the number of
distinct copy destinations with a recorded sample — and says outright when the input is empty that
the verdict is not about the scene.

One run then answers it: **124,725 samples accepted, 0 refused, 4 destinations recorded.** The
classifier is fed. Those copies score intra-frame because they are sampled in a LATER pass than the
one that writes them, which is the definition.

Confirmed from the other end: `AURORA_EFB_FEEDBACK=tick` forces suppression of every copy on the
interpolated emission regardless of classification, and that changes **80.1% of the pixels** of the
in-between present. The intra-frame consumers really do read what this frame wrote; starving them is
the documented failure mode and the classifier is right to keep them.

## 3. "It is a screen-space overlay getting the camera delta" — FALSE

`TAfterEffect::perform` composites immediate-mode quads and does
`GXLoadPosMtxImm(param_2->getViewMtx(), GX_PNMTX0)`, so it looked like a screen-space overlay drawn
with a 3D view matrix — which `patch_camera_only` would displace bodily on every in-between present,
because it only skips draws flagged orthographic.

The census already in the tree answers it: **`aftereffect.under2d` 39,233 of 39,233 performs.** The
effect draws entirely under an orthographic projection, so `d.ortho == 1` and the camera delta is
already skipped.

## What is left, and what would decide it

Everything about the effect that is per-TICK rather than per-present is now down to its own
parameters: `TAfterEffect::perform` advances six smoothers (`unk20`, `unk24`, `unk38`, `unk3C`,
`unk40`, `unk44`, each `x += unk48 * (target - x)`) once per tick, and the in-between emission
replays the recorded stream rather than re-running `perform`, so both presents composite with the
same zoom/offset. Those are slow smoothers and a 30 Hz step in them is a weak candidate for visible
judder, but it is the last per-tick quantity in the path.

The other possibility is that the reported "ghost" is not this effect at all — Mario has several
things that leave trails, and the screenshot cannot distinguish them.

Deciding it needs a headed A/B, which the switches now support:

    ./play.sh --60fps -- AURORA_EFB_FEEDBACK=tick     # forces once-per-tick feedback
    ./play.sh --60fps -- SBR_TAGPARTICLE=0            # disables particle interpolation
    ./play.sh --60fps -- SBR_TAGSHADOW=0              # disables shadow interpolation

If the ghost is unchanged by all three, it is not any of the mechanisms this arc has built.

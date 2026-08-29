# GX-compatibility/Aurora exact-frame A/B harness

This harness scores the retained SDL3-GPU GX compatibility renderer against Aurora's rendering of
the same recomp GX stream. It is reference tooling. It does not exercise or score the PC-native
semantic renderer owned by `native-render/`, and compatibility parity is not a G003/G004 success
condition.

## Bounded control run

Run through the guarded launcher; it supplies the required headless, muted, fastboot, capture, and
texture settings as one invariant policy:

```bash
SBR_RENDER_APPROVED=1 ./run-render.sh \
  SBR_FRAME_RATE=native-60 \
  SBR_AB=1 \
  SBR_AB_EVERY=30 \
  SBR_AB_SELFTEST=1 \
  SBR_AB_AT=3 \
  SBR_QUIT_AFTER=130 \
  SBR_LUCENT_DEBUG=ab
```

`run-render.sh` delegates policy to `tools/render/run_render.py`, which forces the compatibility
renderer, headless output, muted audio, Delfino fastboot, J3D capture, texture publication, a
60-Hz submission ceiling, and the kernel GPU watcher. It refuses to start without the explicit
`SBR_RENDER_APPROVED=1` acknowledgement.

## What makes a result valid

The Aurora frame sink publishes an exact frame ID. The join reserves that ID before closing the
Aurora packet, then accepts the compatibility baseline and delayed Aurora readback only under that
same key. Unknown, duplicate, missing, or capacity-exhausted IDs are refusals; callback order is not
used as identity.

With `SBR_AB_SELFTEST=1`, the metric must first score a non-degenerate Aurora image against itself
at edgeIoU 100% and luma correlation +1.000. Until that control passes, A/B verdicts are suppressed.
The useful per-frame lines are `[ab] #N frame=...`; compare aggregate runs only on:

```text
=== COMPARABLE @ N=... === exact Aurora frame IDs (compare runs on THIS line)
```

`SBR_AB_AT` fixes the sample count for that line. Running means at different sample counts cover
different scene frames and are not comparable.

## Operation attribution

`SBR_ABLATE=1` additionally renders one round-robin neutral-operation variant per selected frame.
Each delta is variant-minus-baseline against the same Aurora frame. The named `control:no-op`
variant must byte-match its exact-frame baseline before any attribution table is published. Rows
still sample different scene frames until their populations match, and the table names variants it
has not sampled; treat cross-row ranking as exploratory.

## Coverage limits

This instrument answers whether the project-owned GX compatibility implementation reproduces the
structure of Aurora's rendering of the same recomp command stream. It does not prove that guest game
logic emitted the right scene, does not compare recomp state with the decomp runtime, and does not
cover the semantic renderer. A field or operation absent from the sampled scene is unmeasured, not
correct. A clean launcher exit also requires the GPU watcher to report no new kernel fault.

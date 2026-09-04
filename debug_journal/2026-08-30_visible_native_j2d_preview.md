# Visible native J2D preview

## Symptom and root cause

Both runtimes were already publishing a renderer-neutral J2D stream and the shared SDL GPU pass was
rendering it into a real 640x480 texture. That texture was never shown: runtime composition always
initialized the SDL GPU platform without a window, while Aurora retained operating-system
presentation. The missing step was host ownership and presentation, not another game-layout adapter
or another shader.

## Implemented boundary

`SB_SEMANTIC_FRAME_MODE` is the one shared mode selector: `off`, `audit`, or `preview`. The old
boolean audit switch was removed so output ownership is not spread across two partially overlapping
controls. Audit mode keeps the existing device-only client and lets Aurora present. Preview mode
disables Aurora presentation, attaches the shared platform's sole presenter to Aurora's SDL window,
and encodes the semantic target's clear/blit into the same command buffer as its semantic pass.
Aurora continues rendering offscreen as the retained GX reference, and all game draw bodies still
run.

The client refuses preview initialization when the supplied window is hidden or minimized. A window
that becomes hidden after initialization does not lose or duplicate the semantic frame: the pass is
still submitted and completed, while `windowUnavailableFrames` records that no swapchain image was
available. Bounded validation requires every submitted frame to complete, a non-clear readback, and
at least one actual preview presentation.

`./run.sh --semantic-preview` is the supported entry point. It prints before launch that only the
ported 2D/UI families are visible and that 3D, particles, lights, and effects are absent. This is not
a persisted renderer choice and is not represented as a complete game renderer.

## Controls and runtime evidence

- The CPU mode control distinguishes missing/`off`, `audit`, `preview`, and malformed values.
- The guarded production GPU control first rejects preview against its hidden window, then shows the
  same window, presents a known picture, hides it again, and requires exactly one presented plus one
  unavailable frame. Its existing empty and non-clear readback controls still pass.
- A guarded historical run stopped after 130 operating-system presents with exit 0. It completed 65
  semantic simulation frames, presented all 65, reported zero unavailable frames, and observed
  286,720 pixels different from clear.
- A first guarded decomp run at 130 presents had 130 successful presentations but no non-clear
  sample, so the evidence gate aborted it. This was the correct answer: early black semantic draws
  are not useful output merely because a swapchain accepted them.
- The extended guarded decomp run stopped after 400 presents with exit 0. It completed and presented
  all 400 frames with zero unavailable frames, carried 2,790 pictures and 52 solid rectangles, and
  first observed 158,038 pixels different from clear on semantic frame 311.

## Remaining boundary

The preview proves that above-GX J2D values can own the actual window in both runtimes. It does not
prove full-frame visual correctness and it intentionally exposes the missing families as black:
J3D/other 3D scene geometry, PC-native materials and lighting, particles, and named screen effects.
The next renderer work must select one of those semantic owners rather than composite the retained GX
frame underneath this target, which would conceal the missing ownership.

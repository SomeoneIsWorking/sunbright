---
id: 22
title: Native renderer omits GXCopyTex post-copy EFB clear
status: resolved
symptom: Native texture copies resolve the EFB but leave the copied source rectangle intact when GX requests copy-and-clear
tags: render,recomp,native,efb-copy,clear,parity
created: 2026-08-27
updated: 2026-08-28
---

## Root cause

The FIFO parser received the BP `0x52` copy trigger, but the typed handoff into the native renderer
carried only destination and dimensions. It discarded trigger bit 11, BP clear AR/GB/Z, and the PE
colour/alpha/depth update masks. The backend therefore blitted the copy destination and resumed
drawing with the pre-copy EFB still present. Aurora and Dolphin independently perform the hardware
contract in the opposite order: resolve first, then clear only the copy source rectangle.

## Resolution

`NativeEfbCopyRequest` now carries the complete clear contract from the FIFO parser through the
scene ordering seam. `native_efb_copy_plan` owns BP decoding and one clipped source rectangle;
`native_efb_copy_clear_draw` turns a non-empty enabled clear into a scissored synthetic draw with
independent RGB, alpha, and depth write masks. The existing copy epoch makes that draw the first
batch after the copy barrier and prevents it from merging with the pre-copy batch.

CPU controls cover asymmetric AR/GB channel order, depth normalization, clear-disabled and
no-write-mask cases, partial clipping, the offscreen `Hx_Test5` row, colour-only depth-disable
behaviour, and the pre-copy/post-copy epoch boundary. A live native/Aurora parity run is still
required before attributing any whole-frame score change to this fix.

### Reopened (2026-08-28)
2026-08-28 live run: Vulkan validation VUID-vkCmdDraw-None-04007 on every post-copy synthetic clear. Root cause is broader than the clear batch: render_pass binds g_vbuf once, then GXCopyTex ends the render pass and begins a resumed pass without rebinding vertex input state. The clear batch became the first draw after that resume and exposed the invalid command. Reopen until every render-pass begin shares one binding path and a guarded live control is clean.

### Resolution (2026-08-28)
The typed GXCopyTex clear path and render-pass rebind fix pass CPU controls. A guarded live native A/B run on 2026-08-28 exercised ordered EFB copies through 130 presents, produced no VUID-vkCmdDraw-None-04007 or other Vulkan validation error, and shut down GPU-clean.

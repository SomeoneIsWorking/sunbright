---
id: 30
title: Decomp Delfino retained GX path aborts on texture wrap mode 3
status: open
symptom: A bounded decomp stage-1 run can abort in Aurora while building the DrawBuf Indirect sampler because texture map 0 contains GX wrap-S value 3, which is not a valid GXTexWrapMode
state_items: S002
tags: decomp,aurora,gx,texture,crash,stage1
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

Unknown. The first invalid state is currently observed only at sampler construction; the producer
of the texture mode word has not been traced.

## Grounded evidence

The 2026-08-30 core stops in `aurora::gfx::TextureBind::get_descriptor` while processing marker
`DrawBuf Indirect`. Aurora texture map 0 contains `mode0=0x800001d7`, whose wrap-S bits are 3, for a
64x64 I4 texture at a valid game allocation. The active TEV stage uses texture map 1 while the
indirect stage samples map 0. The same fatal line and surrounding retained draw path were captured
in two runs dated 2026-08-05, before the semantic-window work.

The semantic client had completed 105 frames with 56 operations and zero `J2DWindow` submissions
when the 2026-08-30 retained GX path aborted. This rules the new window adapter out as the producer;
it does not explain which older path supplied the bad mode word.

## Next step

Trace the BP mode-0 write or originating JUT/J3D texture object that supplied `0x1d7` and establish
why its wrap bits are invalid. Do not clamp mode 3 to a supported sampler mode.

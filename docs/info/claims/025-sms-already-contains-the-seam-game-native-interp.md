---
id: C025
kind: claim
status: holds
created: 2026-08-05
tags: 60fps
depends: decomp/sms/src/Strategic/liveactor.cpp#performOnlyDraw
---

## Claim

SMS already contains the seam game-native interpolation needs: TLiveActor::performOnlyDraw (virtual, shipping, called from enemyAttachment/smallEnemy/NpcBase) is requestShadow+calcRootMatrix+calc+viewCalc+drawObject with NO moveObject and NO frameUpdate — recompute-and-draw without advancing state. TLiveActor::perform's bit 0x2 mixes animation advance (frameUpdate) with matrix computation (calcRootMatrix/calc), which is the only real obstacle to running a draw pass twice, and performOnlyDraw already splits it.

## Evidence

decomp/sms/src/Strategic/liveactor.cpp:353 (perform) and :386 (performOnlyDraw); LiveActor.hpp:70 declares it virtual; callers in enemyAttachment.cpp:179, smallEnemy.cpp:959, NpcBase.cpp:679. Frame rate source: Application.cpp:270 new JDrama::TDisplay(2,...) -> waitForRetrace -> sb_frame_present(2). debug_journal/2026-08-05_game_native_interpolation_design.md

## What would falsify it

a draw-side perform list (DrawBufGroup/Graffito/Pollution/GX/Silhouette/GXPost) proves to mutate state when run twice, making the sub-frame pass double-step something

---
id: C001
kind: claim
status: holds
created: 2026-07-28
tags: native-render
---

## Claim

The recomp's GX FIFO parse matches aurora's derived state on 99.55% of draws (stage counts, per-stage texmap/texcoord/enable, per-unit bound texture)

## Evidence

SBR_STATE_DIFF=<n> per-draw oracle vs aurora's live g_gxState, paired by STREAM BYTE OFFSET; 134 of 29,492 draws differ and 133 of those have the same unit id within 4 draws on aurora's side. debug_journal/2026-07-23_native_texgen_and_texmap_bisect.md

## What would falsify it

if draws are ever paired by ORDINAL again the number reverts to a meaningless 2.8%; also void if aurora's texObj.texObjId stops being the TX_SETIMAGE3 address the recomp sends

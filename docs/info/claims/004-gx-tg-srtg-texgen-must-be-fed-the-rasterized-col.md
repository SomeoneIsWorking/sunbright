---
id: C004
kind: claim
status: holds
created: 2026-07-28
tags: native-render
---

## Claim

GX_TG_SRTG texgen must be fed the RASTERIZED colour channel, not the vertex's stored CLR0

## Evidence

GXSetTexCoordGen2's GX_TG_SRTG arm forces source row 2 and takes the channel output; feeding stored CLR0 pinned every such coordinate to (1,1) for the whole scene, which was the 'one flat colour' frame

## What would falsify it

if a mesh with no stored CLR0 ever produces a varying SRTG coordinate without this fix

# Delfino wash — EFB peek localizes it: PER-FRAGMENT (not copy/present). Built /efbpeek. (session 14c)

Builds on `2026-06-18_floor_wash_mips_FALSIFIED.md`. The user pushed back: stop chasing by hand, make
the TOOLING narrow it down. Did that — built a pipeline-stage localizer and got a decisive answer.

## NEW TOOL: /efbpeek?x=&y=  (runtime/probe_server.cpp)
Peeks Dolphin's **EFB (pre-copy)** at a native EFB pixel (x 0..639, y 0..527) via
`g_efb_interface->PeekColor` (forces `bEFBAccessEnable`). Run under `SUNBRIGHT_NGX_PRESENT=0` (GX fills
the real EFB). This splits any "ngx too bright/dark" bug into PER-FRAGMENT (EFB already that value) vs
POST-FRAGMENT copy/present (EFB bright, XFB dark) — in ONE call, no hand-decoding.
FLAKY: PeekColor waits on the video thread (AsyncRequests blocking event); our paced headless loop
doesn't pump it reliably, so only the FIRST peek per process is dependable (later curls can hang).
HARDEN next: peek from the GXCopyDisp tee (gx_stream_own.cpp 0x8035ecec, runs at copy time, GPU
active) or pump the GPU between peeks. One peek/run still localizes a stage.

## DECISIVE RESULT
EFB floor (320,420) pre-copy = **RGB (81,85,101)** — already DARK, ≈ the XFB/oracle floor (101,106,112).
(Another point read (0,0,0), so the tool reads real varying EFB data, not a stuck value.)
=> The darkening is **PER-FRAGMENT, written into the EFB by GX's 3D draw — NOT the EFB→XFB copy and NOT
ngx_present.** This KILLS the "scene-wide copy/gamma" lead from the prior handoff.

## What this means (re-focus)
`frag = texture(bright ~235) × rasColor`, and GX's EFB fragment is ~(81,85,101) dark-blue. So GX's
**rasColor (color-channel output) for the floor is dark-blue ~0.35**, NOT white. ngx computes it WHITE
(unlit, matSrc=VTX, vtxColor idx1=white) → renders bright. So the bug is in the COLOR-CHANNEL OUTPUT:
GX darkens it, ngx doesn't. This VINDICATES the original `delfino-lighting-wash` (ambient) thread and
the `SUNBRIGHT_NGX_AMBMUL` probe (×ambient ~0.5 dropped ngx floor 235→128≈105). The block reads CLOF
en=0 (unlit) per ngx, yet the EFB proves the channel output is darkened — so EITHER:
  (a) GX renders these "LightOff" materials LIT with the inherited global ambient (~0.35 blue), no
      diffuse (no normals) → output = matColor(white) × ambient. ngx must apply that ambient. OR
  (b) ngx reads the WRONG color block / wrong rasColor for these floor shapes (the real material is
      lit / has a dark channel). Verify shape 8112bfdc's ACTUAL material→colorBlock vtable directly.
NEXT: use /efbpeek as ground truth. Resolve (a)vs(b): read the floor shape's real color block + the
GXSetChanAmbColor the game last programmed before the floor draw (sync). Re-add AMBMUL to A/B. The
fix is to make ngx's rasColor for these shapes match the EFB (~0.35 blue), via the correct
per-draw-group ambient/lighting — verified by oracle_ab.sh 14 (floor delta must drop) AND /efbpeek
parity (ngx's rendered floor must hit ~(81,85,101)).

## Caveat
One EFB reading (not a grid) underlies "per-fragment" — strong (a bright EFB was required for the copy
hypothesis; we got dark) but harden /efbpeek and grid the floor to be fully airtight. Also confirm a
KNOWN-bright spot peeks bright (tool sanity).

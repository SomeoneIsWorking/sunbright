# Native 60 budget and indexed-quad interpolation — 2026-08-22

User report: Native 60 slows down; interpolated 60 still has jittering elements.

## Native 60: the tick misses its budget

`./run-safe.sh SBR_FRAME_RATE=native-60 SBR_STAGE=1 SBR_QUIT_AFTER=700
SBR_LUCENT_DEBUG=frame SB_RUN_SECS=90` exited clean with no kernel GPU events. During settled heavy
intervals it reported about 14.6 ms of guest/FIFO work plus 7.7 ms of render work per tick and an
observed rate around 45–52 Hz. A native 60 Hz tick has 16.67 ms total, so the full tick is over
budget before pacing can help.

`SB_PROFILE_GFX=60` attributed roughly 3.1–3.3 ms per frame to Aurora's per-draw command-building
work alone: array upload 1.14–1.31 ms, shader-info construction 0.51–0.56 ms, uniform build
0.48–0.54 ms, pipeline lookup 0.33–0.39 ms, bind groups 0.21–0.24 ms, command push 0.14–0.16 ms,
and resolve 0.09–0.20 ms. This names a renderer/FIFO performance arc; it does not justify skipping
draws or retuning the game's timers.

`SB_PROFILE_DRAWPRIM` is not valid evidence on the recomp path yet. Its report is wired to Aurora's
live `fifo::drain`, while the recomp invokes `aurora_fifo_replay` directly. The counters can collect
without their report being emitted, so a zero/missing summary there says nothing.

## Interpolated 60: two draws carry motion in indexed arrays

`tools/gfx/graphics_db.py next` identified the only two remaining unexamined rows with a measured
`camera-only` verdict:

- primitive `0x80224f98`, `TDLColorTexQuad::draw+0x8c`;
- primitive `0x802254fc`, `TDLTexQuad::draw+0xf4`.

Ghidra decompilation of function entries `0x80224f0c` and `0x80225408`, cross-checked with
`decomp/sms/src/MarioUtil/DLUtil.cpp`, shows that both bind an indexed XYZ-f32 position array and
issue one display-list draw. The color variant also binds color; both bind texture coordinates.
Their callers rebuild the position arrays once per simulation tick for question marks, splash
droplets, and water-spray refraction. Their position matrix is identity, so matrix interpolation
has no motion to interpolate.

The recomp now brackets each active draw with a stable `(object, quad-kind)` tag and a one-shot
reserved `GX_AURORA_DRAW_TAG_INDEXED_DEFORM` control payload, followed immediately by the real tag.
Aurora validates that the marked draw really uses an indexed, big-endian XYZ-f32 POS array,
snapshots its current bytes, and retains previous positions by tag.
For an in-between emission it writes interpolated XYZ into fresh storage and changes only that
draw's POS `array_start`; the retained exact emission is untouched. First sightings, stale history,
and size changes fall back to camera-only and cannot fall through to misleading identity-matrix
pairing.

The arithmetic control uses a 16-byte record whose XYZ moves 0/10/20 → 20/30/40. Alpha .25/.5/.75
must yield the first coordinate 5/10/15, the midpoint must be 10/20/30, and bytes 12–15 must remain
unchanged. It passes inside the shipping interpolation call path.

## Verification boundary

A 720-present stage-1 interpolated run completed 360 simulation ticks and 360 in-between presents,
reported 98.2% ordinary tagged-draw pairing, and exited with zero GPU faults. It also reported:

`indexed position interpolation: 0 paired ... NO MARKED INDEXED DRAW REACHED THE CAPTURE`

That scene did not generate a live question/splash/spray TDL batch, so it validates that the new
path is inert for unrelated draws and that its known-motion control runs, but not the command-to-GPU
seam on real TDL data. The two graphics-registry rows intentionally remain `camera-only` until a
known-positive scene exercises them. Promoting them on a zero-arrival run would repeat the project's
most expensive instrumentation failure mode.

# 2026-07-10 — Phase-4 DOES emit: uncapped one-frame draw dump falsifies the "all-ortho" capture

## Question

`J3DDrawBuffer::drawHead` logs `[dbhead] packets=N` at BOTH phase-1 and phase-4 for
Sky Xlu (6 packets) / MapOpa (7 packets) every title frame — but every stream-level
capture so far had only ever shown **ortho-bound** (phase-1) Sky/Map draws. Suspicion:
a gate between drawHead's log and its per-packet draw loop silently skips the phase-4
flush, meaning ALL backdrop emission happens under stale ortho (which would fully
explain the black backdrop).

## Gate candidates named from code BEFORE looking at data

`reference/sms/src/JSystem/J3D/J3DGraphBase/J3DDrawBuffer.cpp:594-599` — the loop after
the `[dbhead]` print is **unconditional**:

```cpp
for (u32 i = 0; i < mSize; i++)
    for (J3DPacket* packet = mBuffer[i]; packet != nullptr; packet = packet->getNextPacket())
        packet->draw();
```

The only per-packet skips live one level down (`J3DPacket.cpp`):

- `J3DMatPacket::draw()` (line 185): `if (!checkThing(unk34))` — skips the whole material
  packet only if **every** shape packet has `unk30 == 0`.
- `J3DShapePacket::draw()` (line 213): `if ((unk14 != 0) && (unk30 != 0))` — skips if the
  shape is null or the enable flag is clear.

No phase check, no mViewNumber/j3dSys check, no entryNum/drawn-flag between log and loop.

## Instrument

`SB_DRAW_DUMP_FRAME=<retraceCount>` (aurora `0710ab0`, lib/gx/command_processor.cpp):
dumps EVERY draw whose `VIGetRetraceCount()` equals the target — uncapped — one line
each: index, prim, verts, proj O/P, marker. Combined with `SB_DRAW_DUMP` it also lifts
the old 200-draw cap on the full-detail dump.

## Run

Paced `SB_HEADLESS=1 SB_STAGE=15 SB_SCENARIO=0 SB_DRAW_DUMP_FRAME=2000 SB_DBHEAD_DBG=1
SB_TRACE_SEQ=1` → `scratch/logs/gate_probe_2026-07-10.log`. The retrace-2000 frame
drains **293 draws** inside one `aurora-end-frame`; the matching dbhead flushes are
seq=108991..109046 (phase-1 → phase-4 → phase-6).

## Emission table (one frame, stream order)

| stream idx | proj | marker | draws | verts | dbhead flush |
|---|---|---|---|---|---|
| 0-8 | O | DrawBuf Sky Xlu | 9 | 238 | ph1 packets=6 |
| 9-26 | O | DrawBuf MapOpa | 18 | 146 | ph1 packets=7 |
| 27-28 | O | DrawBuf MapXlu | 2 | 11 | ph1 packets=2 |
| 29-99 | O | DrawBuf Mirror Opa | 71 | 592 | ph1 packets=14 |
| 100-101 | O | DrawBuf Mirror Xlu | 2 | 23 | ph1 packets=2 |
| 102-112 | O | DrawBuf LensFlare | 11 | 44 | ph1 packets=11 |
| 113-124 | O | TLightDrawBuffer::Opa | 12 | 93 | ph1 packets=6 |
| 125-195 | P | DrawBuf Mirror Opa | 71 | 592 | ph4 packets=14 |
| 196-197 | P | DrawBuf Mirror Xlu | 2 | 23 | ph4 packets=2 |
| 198-206 | **P** | **DrawBuf Sky Xlu** | **9** | 238 | **ph4 packets=6** |
| 207-225 | **P** | **DrawBuf MapOpa** | **19** | 182 | **ph4 packets=7** |
| 226-237 | P | TLightDrawBuffer::Opa | 12 | 93 | ph4 packets=6 |
| 238-242 | P | StaticMapObj ShadowOpa | 5 | 72 | ph4 |
| 243-245 | P | DrawBuf MapXlu | 3 | 15 | ph6 packets=2 |
| 246 | P | buf? | 1 | 8 | — |
| 247 | P | AfterIndirect Xlu | 1 | 52 | — |
| 248-249 | O | DrawBuf LensFlare | 2 | 8 | ph6 packets=2 |
| 250-260 | P | DrawBuf LensFlare | 11 | 44 | ph6 packets=11 |
| 261-292 | O | DrawBuf ChrXlu | 32 | 128 | 2D/menu pass |

## Answers

- **DrawBuf Sky Xlu emits TWICE**: 9 draws ortho-bound (phase-1) + 9 draws
  perspective-bound (phase-4). Not 6 vs 12 — packets≠draws (6 mat packets fan out to
  9 shape draws).
- **DrawBuf MapOpa emits TWICE**: 18 draws O (ph1) + 19 draws P (ph4).
- **NO gate exists.** Neither dbhead flush emits zero; the per-packet loop after the
  log is unconditional and both phases push real GX draws.

## Why phase-4 was "never observed" — the capture was the bug

`SB_DRAW_DUMP_AFTER` caps at **200 draws**. The perspective phase-4 Sky/MapOpa block
occupies stream positions **198-225** — the cap truncates exactly at the boundary
(Sky P draws 198-199 barely visible, MapOpa P at 207+ never). Every "only ortho draws
in the stream" conclusion was a windowing artifact, including the
`2026-07-10_title_backdrop_black_verdict.md` claim that "every single 3D draw this
frame shares proj=ORTHOGRAPHIC" — **that claim is CORRECTED by this entry**: it
described only the first ~200 draws (the phase-1 ghost pass); the phase-4 pass behind
the cap is perspective-bound.

## What this means for the black backdrop

"All backdrop emission happens under stale ortho" is **falsified**. The phase-4
perspective draws exist in the stream; the black screen must instead come from the
state those P draws carry (viewport/scissor, blend, TEV, matrices) or from what
overdraws them — chase the phase-4 P block's full-detail dump
(`SB_DRAW_DUMP=1 SB_DRAW_DUMP_AFTER=2000 SB_DRAW_DUMP_FRAME=2000` now dumps all 293)
rather than a phantom emission gate. Note the phase-1 ortho ghost pass still draws the
same geometry earlier in the frame — whether that pass should exist at all on GC is a
separate (still open) question.

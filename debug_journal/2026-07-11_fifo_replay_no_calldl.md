# 2026-07-11 — FIFO replay CALL_DL risk RESOLVED: SMS title GX stream has ZERO display-list calls

Resolves the one open risk named in the FIFO parity-harness plan: whether
`GX_CMD_CALL_DL` (0x40) opcodes in the oracle `.dff` captures carry load-bearing
display-list bodies that aurora skips (aurora logs-and-ignores nested CALL_DL,
`command_processor.cpp:559`).

## Finding: there are NO CALL_DL opcodes in the SMS title GX stream

`tools/oracle/calldl_audit.py` (new) walks each frame's command stream **in sync**
via the parser's `decode_frame` (not a raw byte histogram) and counts CALL_DL
opcodes:

- `title_press_start_vi_stable.dff` (3 frames): **0 CALL_DL** in all 3 frames.
- `title_ENTRY_transition.dff` (150 frames): **0 CALL_DL** in all 150 frames.

## How the "2936/frame CALL_DL" figure was wrong

The earlier byte-histogram analysis (parse summary) reported `0x40: 2936` and
read it as "2936 CALL_DL opcodes." That was a **byte-frequency misread**: the `0x40`
byte appears 2936× as a *payload data byte* inside BP/XF register values and
primitive vertex streams, not as a standalone opcode. The synced opcode walk
(which sizes each opcode by its real payload length and advances correctly)
finds zero actual `0x40` opcodes. Lesson reaffirmed: a byte histogram is NOT an
opcode histogram; only the synced walk knows which bytes are opcodes vs payload.

## Implication for the FIFO replay harness

The command stream is **flat and self-contained** — no nested display lists to
expand. The only out-of-band data a replay must reconstruct is the per-frame
`memoryUpdates` stream (vertex/texture/XF data, which the plan already handles).
The replay design needs no adjustment for CALL_DL; the risk is closed.

This also confirms retail SMS's title renders inline (no `GXCallDisplayList`
macros in the hot path), which is consistent with the decomp source — J3D/J2D
emit commands directly into the FIFO rather than building reusable DLs for the
per-frame draw.

## Audit tool

`tools/oracle/calldl_audit.py` — run on any `.dff` to confirm CALL_DL presence
and (addr, size) clustering. Reusable for future captures.

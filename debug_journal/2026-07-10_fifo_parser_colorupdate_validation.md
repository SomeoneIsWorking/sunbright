# 2026-07-10 — FIFO parser color-write decode: validated correct, prior "black EFB" read was an analysis error

Task: the .dff FIFO log for the stable title (`scratch/oracle/fifo/title_press_start_vi_stable.dff`)
replays in Dolphin's FIFO Player to the COMPLETE title (full blue sky, clouds, sun-flare, sea-textured
logo — `scratch/screenshots/dff_replay_render.png`, mean RGB ~0.51/0.67/0.78). A prior ad hoc analysis
(`scratch/oracle/fifo/extract_entry_paint_dome.py`, run against `title_ENTRY_transition.dff`) had
concluded: the 202-vertex "dome" draw has `color_update=0`, and the frame's final EFB-copy clear color
is `(0,0,0,0)` — which reads as "nothing paints color, screen clears to black." That contradicts the
replay. Needed to determine: is `tools/oracle/parse_fifo_dff.py`'s BP-register bit decode wrong, or was
the analysis wrong about which draws paint the sky?

## Bit-layout cross-check vs aurora (known-good reference)

`extern/aurora/lib/gx/command_processor.cpp`'s `handle_bp()` is the SAME decoder the native runtime
uses and is known to render the UI/logo correctly, so it's the reference for BP bit positions.

| Register | Field | aurora (`bp_get(value, size, shift)`) | parser (`bits(value, lo, width)`) | match |
|---|---|---|---|---|
| 0x40 ZMode | test_enable | `bp_get(v,1,0)` | `bits(zmode,0,1)` | yes |
| | func | `bp_get(v,3,1)` | `bits(zmode,1,3)` | yes |
| | update_enable | `bp_get(v,1,4)` | `bits(zmode,4,1)` | yes |
| 0x41 CMode0 | blend_enable | `bp_get(v,1,0)` | `bits(cmode0,0,1)` | yes |
| | logic_op_enable | `bp_get(v,1,1)` | `bits(cmode0,1,1)` | yes |
| | color_update | `bp_get(v,1,3)` | `bits(cmode0,3,1)` | yes |
| | alpha_update | `bp_get(v,1,4)` | `bits(cmode0,4,1)` | yes |
| | dst_factor | `bp_get(v,3,5)` | `bits(cmode0,5,3)` | yes |
| | src_factor | `bp_get(v,3,8)` | `bits(cmode0,8,3)` | yes |
| | subtract | `bp_get(v,1,11)` | `bits(cmode0,11,1)` | yes |
| 0x4F clear r/a | r / a | bits 0-7 / 8-15 | (newly added, see below) | yes |
| 0x50 clear b/g | b / g | bits 0-7 / 8-15 | (newly added) | yes |
| 0x51 clear z | depth | bits 0-23 | (newly added) | yes |
| 0x52 copy exec | clear_enable / display_copy | bit 11 / bit 14 | (newly added) | yes |

**No decode-bug in any field the parser already had.** The only real gap: `tools/oracle/parse_fifo_dff.py`
had NO decode at all for BP 0x4F/0x50/0x51/0x52 — that decode only existed in the throwaway
`scratch/oracle/fifo/extract_entry_paint_dome.py` script, untracked and unvalidated against a full
color-write census.

## Validation: does the parser find ANY color-writing sky draw?

Ran the tracked parser against frame 0 of `title_press_start_vi_stable.dff`:

- 1258 total draws.
- **71 draws with `color_update=1`.**
- **All 71** have viewport=640x448 and scissor covering the full (0,0)-(639,447) EFB — i.e. every
  color-writing draw this frame is a full-screen candidate. Vertex-count buckets: 4 (×39), 8 (×1),
  10 (×4), 24 (×1), 52 (×26).
- The 202-vertex "dome" draws (seq 5936/5937) genuinely have `color_update=0` — confirmed, not a
  decode bug (same seq numbers, same result in both the old ad hoc script and the tracked parser).
- The 3 BP-0x52 copy-execute events in frame 0: copy#1 `clear_enable=1 display_copy=0
  clear_rgba=(0,0,0,0)`, copy#2 `clear_enable=0`, copy#3 (the display copy) `clear_enable=1
  display_copy=1 clear_rgba=(0,0,0,0)`.

## Verdict: ANALYSIS ERROR, not a parser bug

The parser's decode is correct and matches aurora's known-good bit layout field-for-field. The prior
conclusion ("dome is color_update=0 + EFB clears to (0,0,0,0) ⇒ black screen") was wrong for two
independent reasons:

1. **Wrong draw picked as "the sky painter."** The dome (202v) is real but is a Z/mask-only pass
   (`color_update=0`, `alpha_update=0`) — not the color source. The 71 fullscreen `color_update=1`
   draws (52v gradient-sky quads/strips, 4v/8v/10v/24v smaller shapes — sun-flare and cloud-adjacent
   geometry per vertex count) are the actual color painters, and the parser found them the whole time;
   the earlier analysis simply never looked past the dome.
2. **"Clears to black" was read as "stays black."** `clear_rgba=(0,0,0,0)` on the EFB-copy trigger is
   just the clear step before the frame's draws run — it says nothing about what gets painted
   afterward. A clear-to-black followed by 71 full-screen color-writing draws is completely ordinary
   compositing, not evidence the final image is black.

## Fix landed

Added the missing BP 0x4F/0x50/0x51/0x52 decode to `tools/oracle/parse_fifo_dff.py` itself
(`BPRasterState.clear_ra/clear_bg/clear_z/copy_events`, bit positions copied 1:1 from aurora's
`handle_bp()`), so this class of question no longer needs a one-off untracked script. Added
`--assert-sky-paint [--frame N]`: decodes the given frame (default 0) and REQUIRES at least one
`color_update=1` draw whose viewport+scissor cover the full 640x448 EFB, printing the copy-execute
events for context; exits nonzero with a named reason if none is found. Run against frame 0 of
`title_press_start_vi_stable.dff`: **PASS — 71 full-screen color-writing draws, vertex counts
[4, 8, 10, 24, 52].** This is the durable guard against silently re-deriving "nothing paints the sky"
from one draw's `color_update` bit in a future session.

## Dead end noted

`scratch/oracle/fifo/extract_entry_paint_dome.py`'s STEP 3 output (on `title_ENTRY_transition.dff`,
a different capture) is not itself wrong data, but its framing ("ANSWER copy #3: clear_rgba=(0,0,0,0)")
invites exactly this misread without also reporting the color-writing-draw census. Do not re-run that
script's narrow framing as a stand-in for "does anything paint the sky" — use
`parse_fifo_dff.py --assert-sky-paint` instead.

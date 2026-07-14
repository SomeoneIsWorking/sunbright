# 2026-07-14 — FIFO replay renders its first coherent title frame + oracle capture tooling rebuilt

## Context: the oracle FIFO cache was LOST

`scratch/` (gitignored) was wiped between sessions — every cached `.dff`
(`title_press_start_vi_stable.dff`, `title_ENTRY_transition.dff`) and the oracle MANIFEST
are gone. The FIFO parity harness (SB_FIFO_REPLAY, translator layers 1-3 committed 89e4e69)
had no input data. WORKFLOW-FIRST: rebuilt the capture capability as durable tooling instead
of a one-off.

## New tooling: headless Dolphin FIFO recording (GUI-only feature, automated)

Dolphin 2503's FifoRecorder has NO CLI/hotkey path — it's reachable only through the Qt
FIFO Player window (Tools → FIFO Player → Record/Save). Automated it:

- **`tools/oracle/xdrive.py`** (new, committed): minimal X11 driver — XTest synthetic
  keys/chords/typing/clicks, window list/activate — for driving Qt GUIs on a headless
  Xvfb display. Generic; pairs with ImageMagick `import -window root` for screenshot
  feedback.
- Recipe (works today, coordinates for 1280x800 Xvfb :99, 800x600 Dolphin window at 0,0
  with RenderToMain): launch `dolphin-emu-x11 -v OGL` (Vulkan has no WSI on Xvfb; GLX =
  llvmpipe works), open Tools(274,15) → FIFO Player(302,106), move the FIFO window to
  (730,-400) so only its button row is visible (Record=(1128,91), Save=(1038,91)),
  then click Record (auto-stops after "Frames to Record", default 3), Save, type an
  ABSOLUTE path into the KDE file dialog, Return.
- **Validation is the parse, not the screenshot**: color-threshold "is this the title?"
  heuristics false-fired repeatedly (intro movies are warm/blue too). The reliable loop is
  record blind → `parse_fifo_dff.py` → accept iff draws/frame > 1000 (title scene = 1258;
  THP movie quad = 1), else delete and retry. Screenshots only pre-filter timing.
- Traps hit: no WM on Xvfb → moved windows leave stale ghost pixels (harmless, but
  screenshots lie); XTest pad input (GCPad Start=Return) needs input focus on the render
  child window (walk query_pointer to the deepest child, set_input_focus) — and a START
  pressed during boot/GC-logo gets latched and can skip STRAIGHT past the title into
  file-select (SMS file-select has NO path back to title; Emulation→Reset is the only
  recovery; toolbar "Refresh" is the game-list refresh, NOT reset).
- A leftover Ship of Harkinian window from another session lives on the shared Xvfb :99 —
  don't kill it (not ours); restack Dolphin Above it.

Recaptured (all GMSE01, EmulationSpeed=0, OGL/llvmpipe, 3 frames each, in
`scratch/oracle/fifo/`): `title_settled.dff` (title scene mid-fly-in, 1258 draws/frame,
278KB fifo/frame, 0 CALL_DL — the translator test vector), `capture_try1.dff` /
`title_stable.dff` (attract THP movie frames, draws=1 — minimal texture test vector).
A settled PRESS-START capture (`title_press_start.dff`) is still being fished for with the
parse-validated loop; the mid-fly-in one is sufficient for translator work.

## Replay translator: two root-caused fixes → first coherent render

Symptom chain, each diagnosed mechanically vs the draw-dump instrument:

1. **Empty black frame** (mean RGB ≈ 7): SB_DUMP_FRAME queues an async buffer map that the
   replay destroyed on exit ("Buffer was destroyed before mapping was resolved"). Fix:
   `replay()` pumps 2 empty aurora frames after the last replay frame. Also: the dump's
   default 60-present arm never fires in a 3-frame replay — use `SB_DUMP_FRAME_AFTER=2`.
2. **Garbage position matrices** (`posmtx` full of 1e24-scale floats in draw-dump, while
   in-stream projection was sane): the translator marked matrix arrays (attr 12-15) `le=true`
   ("host-computed") — TRUE for the native runtime, WRONG for replay where ALL arrays live in
   the GC-RAM shadow (raw big-endian Dolphin memory). `le=false` for every synthesized array
   base. After the fix posmtx is orthonormal+plausible. NOTE: this makes the replay the ONLY
   exerciser of aurora's BE vertex-fetch path (in-shader bswap) — native runtime always
   feeds LE arrays.
3. **Full-screen shredded blue/white stripes**: texture binding was synthesized from a
   "most recently configured texmap" heuristic fired once per TextureMap memupdate — wrong
   slot association AND no per-draw rebind dynamics. Aurora ignores BP image regs for data
   (native GDSetTexObj seam supplies LOAD_TEXOBJ), so the translator must mirror EVERY bind:
   track image0 (w/h/fmt) + image3 (base<<5) per texmap from the BP stream and emit
   LOAD_TEXOBJ (host ptr into shadow, texObjId=GC base addr, version bumped per memupdate)
   whenever both halves are known. Memupdates now only fill the shadow.

Result: replay of `title_settled.dff` renders the title's sun + light-ray pass in the
correct screen position (matches Dolphin's live view at record time). Remaining defect:
scene far too dark — sky-dome/backdrop color mostly absent (next arc; suspects: TLUT-less
CI textures (loud warn added), EFB "display copy" stub (aurora logs it 3×/frame),
preload_state BP/XF gaps, vertex color (CLR0) path).

## Non-defects ruled out (don't re-chase)

- **585 native draws vs 1258 oracle draws**: aurora MERGES consecutive same-state draws
  (`g_mergedDrawCallCount`, command_processor.cpp:2173). Per-draw vert-count diffs vs the
  parser are merge artifacts, not translator bugs.
- **byte-histogram ≠ opcode histogram** (reaffirmed): only the synced walk counts opcodes.
- `.dff` header `*Size` fields for bpMem/cpMem/xfMem/xfRegs are u32 ELEMENT COUNTS (byte
  size = count*4); texMem is bytes. Loader fixed accordingly (fifo_player.h comment).

# 2026-07-16 — file-select create-file-dialog oracle captured; A-press-advance is an OPEN question

Follow-on from the shadow-direction verification (2026-07-16_drawshadow_port_landed.md
iteration 6). Was looking for a same-state, same-camera Dolphin oracle to verify the
file-select "Select data" menu landing against. Findings:

## Oracle-state inventory (which oracle shows what)

- `scratch/oracle/loadstate_probe/png/fsel_dolphin_end.png` — BARE landing (blocks +
  Mario, no menu panel), WIDER cam. A transient captured during the camera settle
  (30-frame window after loading `fsel_settled.sav`).
- `scratch/oracle/matched/fsel_matched_oracle.png` AND the new
  `scratch/oracle/menu_probe/` capture (400-frame window from the SAME save) — both
  land on the **"Do you want to create a file … on the Memory Card in Slot A? YES/NO"**
  dialog. So `fsel_settled.sav` actually SITS at the create-file dialog (YES highlighted
  green); by frame ~120 of playback the dialog is already up.
- There is currently **no saved-state oracle for the plain "Select data" + New/New/New
  menu** state — the state my live build renders at the file-select landing. The two
  saves bracket it (bare-settle transient ↔ create-file dialog).

Capture recipe (menu_probe): same as the MANIFEST loadstate recipe but
`--load-state-exit-after=400`; extract frames with
`ffmpeg -i <avi> -vf "select=eq(n\,N)" -frames:v 1 out.png`.

## OPEN observation — my build's A-press at file-select did NOT open the create-file dialog

Drove the live build: `SB_PAD_SCRIPT="2600:START 2610:- 3200:A 3210:-"`, dump at present
2400 (scratch/pndump/createfile.png). All four pad events fired (padscript log confirms
frame=3200 entry=3200:A). Result: still the "Select data" menu — no create-file dialog.

Unresolved which of:
1. TIMING — A fired before the menu became interactive, or the present-2400 dump preceded
   the A's effect (present count lags retrace count during load; need the retrace↔present
   map at that point). Retry with a later A (e.g. retrace 3600+) and a later dump.
2. INPUT/CARD-MENU GAP — the file-select card-menu state machine (TCardLoad /
   [[tcardload-title-to-fileselect-state-machine]]) isn't consuming pad A to advance
   New-slot → create-file confirm. Would be a real port gap.

RETRY RESULT (confirms it is NOT timing): re-ran `SB_PAD_SCRIPT="2600:START 2610:-
3600:A 3640:- 4000:A 4040:-"`, dump present 3200 (scratch/pndump/createfile2.png). Both
A presses fired (padscript log: fire frame=4000 entry=4000:A) and frames advanced past
them (Mario's idle pose animated between the two dumps). The menu still did NOT advance —
no create-file dialog. So hypothesis #2 stands: **the file-select "Select data" menu is
not advancing New-slot → create-file confirm on pad A.**

Most likely cause to check FIRST: SMS file-select uses a HAND CURSOR moved by the control
stick over a slot; A only registers once the cursor is ON a slot. My scripts pressed A
with no prior stick input, so the cursor may be on nothing. NEXT: add stick nav before A
(e.g. tap a direction toward slot A, or whatever selects the leftmost slot) and re-press A;
if it still won't advance, the card-menu input path (TCardManager / file-select controller
consuming PAD trig) is the port gap to RE. Only once the dialog opens can a clean same-state
diff (my create-file dialog vs scratch/oracle/matched/fsel_matched_oracle.png, 640x480) run.

This is a NEW file-select arc (menu navigation/input), separate from and lower-priority
than the now-complete render-fidelity work (camera/Mario/shadow all verified). No visible
render defect drives it — it's needed only to unlock same-state parity verification at
interactive file-select states.

Note: `[STUB-CALLED] sb_camera_view_settled -- unported` fires at this scene — a camera
settle predicate stub; unrelated to input but flagged for the worklist.

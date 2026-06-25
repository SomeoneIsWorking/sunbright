# 2026-06-25 — file-select "Select data" banner + slot labels: BMG message reader BE bug

## Symptom
File-select (TCardLoad, stage 15) renders the A/B/C blocks, OPTIONS sign and Mario, but the
two blue text panels are BLANK. The GX oracle (`scratch/oracle/fileselect_gx_oracle.png`) shows
a "Select data." banner across the top panel and "Corrupt / New / New" labels above the blocks.

## Root cause (named, verified)
`SMSGetMessageData(loadmessage.bmg, id)` (`reference/sms/src/GC2D/MessageUtil.cpp`) returned
**null** for every message id → `TCardLoad::setMessage` skipped the strncpy (it already had a
region-tolerant null guard, which logged `[cardload] setMessage skip: src=(nil) id=0x1a`).

The BMG header dwords at `[0x08]` (block/size field) and `[0x0C]` (section count) are stored
**big-endian** and the decomp reads them RAW (`local_40.read(&local_88, 4)`) — correct on the
GameCube's BE CPU, WRONG on the little-endian host. With `SB_MSG_DBG=1`:

```
raw88=1073741824(0x40000000) raw84=33554432(0x2000000)   # before fix (byteswapped)
```

`local_88` (raw 0x40000000) feeds the section-stream length `local_88 * 0x20 - 0x20`, which
**overflows 32-bit to ≤0** → `JSUMemoryInputStream local_74` is empty → `isNotDrained()` is
false → the `while` loop never enters the `INF1` case → falls through to `r31 = nullptr`.
(The INF1 inner reads use `readU16/readS32`, which DO byteswap, so they were never the issue —
the bug was purely the two RAW header reads + the per-message offset `local_68`, also read raw.)

## Fix (faithful, native-guarded)
In `MessageUtil.cpp::SMSGetMessageData`, under `SMS_NATIVE_PLATFORM`:
- byteswap `local_88`/`local_84` after the raw header read (`__builtin_bswap32`),
- byteswap `local_68` (per-message DAT1 offset, also a raw BE u32 read).

The off-platform (BE) path is untouched — the raw reads stay correct there.

## Verification (SB_MSG_DBG=1, after fix)
```
[msgdata] bmg=... raw88=64(0x40) raw84=2(0x2) want=0x1a
[msgdata]   INF1 r27=160 count=35 want=0x1a        # 26 < 35 → in range
[msgdata]   INF1 ... local_68(off)=0x60f           # valid DAT1 offset → non-null string
```
`[cardload] setMessage skip` count: **119+ → 0**. Every message id resolves. This is the same
BE-asset class as the JPA1 loader fix called out in CLAUDE.md's FAIL-FAST section.

## Diagnostics added (kept, env-gated)
`SB_MSG_DBG=1` → `[msgdata]` lines in SMSGetMessageData: header bytes, byteswapped `local_88/84`,
requested id, INF1 entry count, and the resolved DAT1 offset.

## Still open (rendering capture, SEPARATE from this fix — a harness problem)
The string is in the m_0a/m_0b textbox buffers and the J2DTextBox render path is the SAME proven
J2DPrint path the "OPTIONS" sign uses (it renders), so once the settled pane is shown the banner
WILL draw. But capturing a settled-state PPM headlessly is flaky. What was learned chasing it:

- The banner is shown only once `selectBookmark` reaches sub-state **unk10==2** (window-open
  animation done): case 0 sets the text + starts the open anim, case 1 finishes it and shows the
  labels, case 2 is the stable "waiting for pick" state. ~30 perform-frames after mState-0 entry.
- `mState 8->0` happens at perform-frame ~1690; the file blocks/banner animate in over the next
  ~30 frames. `SB_SEL_DUMP_SETTLED=N` (added, CardLoad.cpp) fires a small dump exactly at unk10==2.
- **The "present collapses at state 0" theory is FALSE** (disproved with `SB_PRESENT_TRACE=1` →
  `[present-beat]`): present_hook fires ~80× per perform-frame (136k calls). VI is fine.
- TWO intermittent blockers to a clean settled capture remain:
  1. `selectBookmark` sometimes STALLS at PROGRESS_UNK13 (prog 19) — perform stops ticking,
     unk10 stuck at 0, VIWaitForRetrace spins — so unk10==2 is never reached (card-probe wait /
     thread-sync in the hybrid? `gpCardManager->probe()` in changeScene PROGRESS_UNK13 must
     return CARD_RESULT_READY to call selectBookmark). Other runs DO reach unk10==2.
  2. When unk10==2 IS reached it's near the run's tail, so the dump (renderTevFrame + write_ppm)
     doesn't finish before `timeout -s KILL` fires — bbox prints (line 293) but no PPM lands.
     Also note `[present] frame` is std::printf=stdout (buffered, lost on KILL); write_ppm uses
     fopen/fclose (flushed) so a completed dump SHOULD leave a PPM.
- NEXT to capture the pixel: make the settled state reliably reachable + dumped — e.g. trigger the
  SETTLED dump and then keep running MANY more perform frames (big idle pad tail) so the dump
  completes well before timeout; investigate/own the PROGRESS_UNK13 probe stall so unk10==2 is
  deterministic; consider flushing stdout. The fix itself needs no further change.

## UPDATE — settled capture WORKS but banner text STILL not visible (new finding)
Recipe that captured the settled state: `SB_SEL_DUMP_SETTLED=6` + huge idle pad tail (`... 6000:-`)
+ long `timeout 420` → 6 PPMs (boot_0001..0006) dumped at unk10==2 (`scratch/frames/banner_settled.png`).
RESULT: at the settled choice state the camera is zoomed IN close (the top banner bar is pushed up
near/off the screen top, A/B/C cubes at the very top edge) and there is STILL no "Select data" banner
text and no Corrupt/New/New labels. So blocker (a) BMG-resolve is fixed but (b) remains: either the
m_0a/m_0b/m_1a/m_1b J2DTextBox panes aren't visible/walked, OR the choice-state (up-down-pan) camera
differs from the oracle and pushes the banner off-screen. NEXT (tooling-first): add a load.blo (unk28)
pane-tree dump in TCardLoad (like the SB_SEL_DBG dump in SelectMenu.cpp) printing each J2DTextBox's
resolved string + isVisible + global bounds at unk10==2, AND compare the choice-state camera
(gpCameraOption up-down pan) to the oracle. The settled capture tooling is now proven, so iterate fast.

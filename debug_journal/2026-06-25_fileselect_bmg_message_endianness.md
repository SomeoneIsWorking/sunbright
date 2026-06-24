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

## Still open (rendering, separate from this fix)
The text is now in the textbox buffers, but confirming it RENDERS on-screen needs a frame deep
in the settled choice state (mState==0, unk1C==PROGRESS_UNK13=19, after the window-open animation
in selectBookmark case 1). The first repro only captured ~5 present-frames into state 0 (banner
not yet animated in). Re-running with an idle pad tail to settle. [UPDATE pending verification.]

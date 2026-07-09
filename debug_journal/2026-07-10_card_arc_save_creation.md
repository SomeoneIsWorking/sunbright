# 2026-07-10 — CARD arc: save-file creation works end-to-end (4 defects)

Continuation of the boot-order push (title → save screen). Instrument that made this
possible: the restored `SB_PAD_SCRIPT` (commit `8d0ecb1`). Chain of four distinct defects,
each only reachable after the previous fix:

## 1. Streamed-BGM strcat SEGV — decomp RE gap, seam-guarded (reference/sms `73d898af`)

`JAIBasic::checkWaitStream` does `strcat(buf, unk0->unk1F8[idx].unk10)`; `unk1F8` (per-slot
stream-filename table) is declared, zeroed in `JAIData::init`, and populated NOWHERE in the
decomp — a genuine upstream RE gap (on GC it's almost certainly built from `JaiStInf.bst`).
Any streamed BGM (id top bits 0xC0000000, e.g. post-START title flow) crashed on
nullptr+0x10. Guard drops the request with a one-time `SB_DBG_AUDIO` diagnostic AND releases
the stream slot via `checkPlayingStream`'s completion-path cleanup (a bare return would
wedge the single slot: `checkEntriedStream` only promotes when `unk184->unk14` is null).
**Named follow-up for the audio arc: RE the JaiStInf.bst loader and build unk1F8.**

## 2. aurora CardGciFolder::openFile returned NOCARD on directory miss (aurora `0d63225`)

SDK contract: NOCARD(-3)="no card in slot", NOFILE(-4)="card present, file absent". The game
branches on the difference (`TCardManager::open_` CardManager.cpp:579 → `TCardLoad::changeMode`:
NOFILE → create-save dialog `PROGRESS_UNKE`; NOCARD → "insert card" probe loop `PROGRESS_UNK3`).
The GCI-folder backend (the default) conflated them → per-frame "Failed to open file" spam
(1364/150 s) and the create dialog was DEAD CODE. `CardRawFile` had it right all along.
Fix: two return sites → NOFILE. Same commit: `HostAllocScope` gating on all allocating CARD
entry points (closes CLAUDE.md's "known un-gated remainder", pattern copied from dvd.cpp).

## 3. Card banner region gap — US disc has no `mariobnr_jpn.bti` (Application.cpp)

`Application.cpp:471-474`: `(ResTIMG*)getResource("/card/mariobnr_jpn.bti") + 1` — on GMSE01
the JP name doesn't exist, getResource returns null, and the unconditional `+1` yields
`(ResTIMG*)0x20` (sizeof ResTIMG == 0x20 exactly), which DEFEATS `buildHeader_`'s existing
`if (mBanner)` null-guard → memcpy from 0x20 → SEGV in the create-file flow. GMSE01's
`common.arc:/card/` holds `mariobnr.bti` + `mario_icon.bti` (proved by extracting the arc:
`scratch/extract_common_arc.py` + `scratch/rarc_list.py`, new RARC tooling). Fix (native-
gated): request the US name, null-check BEFORE the `+1`, loud OSReport on miss.

## 4. aurora CardGciFolder::createFile registered files as opened=false

GC `CARDCreate` returns an OPEN handle the game writes through immediately
(`createFile_ → filledInitData_ → CARDWrite`). aurora pushed the new GciFile with
`opened=false`; `getFile()` rejects non-opened entries → every post-create write/close
failed ("Failed to write 8192 bytes") → **every .gci ever created was all-zero** (which the
save screen then read back as "Corrupt"). Fix: register `opened=true`.

## Verified end state (scratch/logs/wf_bannerfix2.log)

True first boot (empty Card A folder) with
`SB_PAD_SCRIPT="600:START 610:- 900:START 910:- 1300:A ... 2200:A"`:
create dialog → CARDCreate → `01-GMSE-super_mario_sunshine.gci` with 4570 nonzero bytes —
Shift-JIS title 「スーパーマリオサンシャイン」, date comment, banner 2905/3584 + icons
1561/2560 nonzero (real US .bti pixels). Save-select pane renders "Select data." with three
"New" blocks + OPTIONS (`scratch/screenshots/wf_bannerfix2_frame.png`). Zero card errors,
zero panics. Next frontier: A-press timing to actually SELECT a file → scenario select →
gameplay load.

## Meta

- The "Corrupt" save blocks seen in earlier sessions were THIS defect chain (all-zero .gci),
  not a parse bug — supersedes any note implying the save-block labels themselves were wrong.
- gdb 17.2 segfaults on this binary's cores; `lldb -c core` works.
- Subagent trap seen twice: an agent that starts its own background run then "waits for the
  completion notification" idles forever — tell agents they must poll/kill their own runs.

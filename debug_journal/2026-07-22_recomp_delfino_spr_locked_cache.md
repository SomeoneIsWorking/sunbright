# Loading a save bounced to the title — per-thread SPRs broke the locked cache, and with it THP

2026-07-22. User report: "I tried loading a save file but it brought me back to the title screen"
(sms-recomp). Root cause found, fixed, verified: **Delfino Plaza now loads and renders in the
recomp** (`scratch/screenshots/recomp_delfino.png` — Mario, FLUDD, HUD, NPCs, palms, the
graffitied statue, dialogue).

## The chain, outermost to innermost

1. `TApplication::proc` (Application.cpp:757): when a director's setup fails it goes to
   `APP_STATE_DONE`, whose body is literally `mNextArea.set(15, 0, 0)` — stage 15 is the
   title/file-select scene. **"Back to the title screen" IS the stage-load failure path.**
2. `TMarDirector::direct` (MarDirectorDirect.cpp:100): `OSJoinThread(&gSetupThread, &local_40);
   if (local_40) return 4;` — the setup thread's exit value is `loadResource()`'s result, and
   nonzero means DONE.
3. `TMarDirector::loadResource` (MarDirectorLoadResource.cpp:122) ends with
   `if (mMap == 1) { int errc = thpInit(); if (errc) return errc; }`. **`mMap == 1` is Delfino
   Plaza and only Delfino Plaza** — which is exactly why the title and file-select were fine and
   loading a save was not.
4. `thpInit` opens `/data/ex128x144_q0.thp`; the plaza runs a persistent THP video
   (`currentStateFinalize` calls `THPPlayerPlay()` on four state transitions, each guarded by
   `mCurrArea.unk0 == 1`). Our `THPPlayerOpen` override declined every movie, so this failed.
5. Letting the real open run got further and then failed inside the codec.
   `THPVideoDecode` (THPDec.c:50) returns **28 = locked cache not enabled**:
   `if (!(PPCMfhid2() & 0x10000000)) goto _err_lc_not_enabled;`

## The actual defect: SPRs were per-thread

`THPPlayerInit` calls `LCEnable()`, whose `__LCEnable` asm sets HID2 bits `0x100F0000`. The codec
then runs on the **video decode thread**. SPRs lived in `CPUState::spr[]`, and every guest thread
has its own `CPUState` — so the decode thread read HID2 as 0 and correctly concluded the locked
cache was off.

HID0/HID1/HID2, L2CR, the BATs and friends describe the **machine**, not a thread. The GameCube
has one core and one set of SPRs. `CPUState::spr` is now a proxy over one shared array
(`CPUState::SprFile::s_spr`); the genuinely per-context registers (SRR0/1, GQR) stay per-CPUState,
which is also what the GC OS saves and restores per thread.

Measured after the fix: `THPVideoDecode -> 0`, `HID2 = 0xf00f0000`, plaza holds
`GAMEPLAY curr={1,5}` indefinitely, Mario at `(0, 300, 7400)` (a plaza coordinate; file-select is
`(950, 100, -1000)`).

**This class of bug is worth remembering: any machine-scope state modelled per-CPUState is
invisible across threads and shows up as an unrelated subsystem "not being ported".**

## Second defect found on the way: OSCancelThread was unmodelled

`OSCancelThread` (0x80348b4c) unlinks the target from scheduler queues, which only works because
retail's own sleep/resume maintained those links. This runtime blocks by token hand-off and never
writes them, so the real body walked a null queue pointer (write to 0x4) when the THP player tore
down its decode threads. Now overridden: `gsched_cancel` marks the thread dead so it is never
scheduled again and publishes MORIBUND to the guest struct, mirroring `gsched_exit`.

## Still open

Opening a SECOND THP session after one is cancelled faults with a null message queue
(`OSMessageQueue+0x1c`, from `PopReadedBuffer`). So THP **decoding** works, but session teardown
and reopen does not. Policy env, default `stage`:

- `SBR_THP=stage` (default) — only the stage-resident player opens. Delfino works; attract movies
  and cutscenes do not play (the game's own movie-setup-failure path, movies marked already-seen).
- `SBR_THP=all` — everything opens and plays, until the second session.
- `SBR_THP=none` — the old behaviour; Delfino Plaza cannot be entered.

The fix for `all` is the THP session lifecycle, NOT the codec — do not re-open "THP is unported".

## Also landed: fastboot is back

`sms-recomp/overrides/fastboot_native.cpp`, ported from the retired Dolphin-era
`runtime/overrides/fastboot_native.cpp` (git `9283f44^`) — same RE and addresses, adapted to this
runtime's override/memory API. `SBR_FASTBOOT=1` boots File 1 straight into Delfino Plaza with the
episode resolved from the save; `SBR_STAGE=<n> [SBR_SCENARIO=<n>]` forces a destination (naming a
stage implies fastboot). This is what let the whole bug be reproduced **deterministically with no
input at all** — the file-select head-butt was never involved.

Diagnostics added: `mario` channel logs Mario's position via the RE'd
`SMS_GetMarioPos` (`lwz r3,-0x60B4(r13)`, r13 = 0x804141C0, so 0x8040E10C holds a POINTER to the
Mario object and the TVec3 is at +0x00 — it is not a position global). The `app` channel now also
reports on AREA changes, not just `mAppState`, which is what made the one-frame bounce visible.

## Retracted

The "Mario's arms are missing/wrong at file-select" investigation is **withdrawn** — the user
reports arms render normally in the real window. That was measured off 320x240 headless dumps;
the "white sliver" was downsample aliasing. Do not re-open it from those dumps.

# 2026-07-10 — BGM double-release SIGSEGV: US retail audit refutes the "dropped check" hypothesis

## The defect (verified previously, this entry documents the retail audit + fix)

`MSound::exitStage → MSBgm::stopTrackBGMs → JAISound::stop(0) →
JAIBasic::stopSoundHandle` (BGM case, `param==0` branch unconditional) →
`stopSeq` on a handle ALREADY released by JAI's internal auto-release paths —
`sound->unk1` already 0, handle sitting on the controller free list; the second
release re-derefed stale links → SIGSEGV at stage-15 attract teardown
(~present 3200). Hypothesis under test: retail GC has an idempotency/liveness
check in `stopSoundHandle`/`stopSeq` (or clears MSBgm's cache) that the decomp
transcription dropped.

## Verdict: hypothesis REFUTED — retail has NO such check anywhere in the chain

US GMSE01 addresses came straight from `reference/sms_gmse01_funcs.txt` (the
repo already carries a full US function map — no JP→US correlation needed):
`stopSoundHandle` 0x80302224, `stopSeq` 0x803068d8, `releaseControllerHandle`
0x803024dc, `MSBgm::stopTrackBGMs` 0x80016740, `checkPlayingSeqTrack`
0x80306da8, `JAISeqEntry::storeBuffer` 0x80304e18, `clearMainSoundPPointer`
0x8030a520, `initSoundParameter` 0x803049f8, `checkEntriedSeq` 0x80306a1c,
`checkReadSeq` 0x80307fac. All decompiled with Ghidra headless
(`pyghidraRun -H scratch/ghidra_proj sms -process sms.dol -noanalysis
-postScript CreateAndDecomp.py`; outputs in `scratch/decomp_audio_stop/`,
gitignored — re-derive with the command above).

Every one of them matches the decomp transcription 1:1. The load-bearing
retail evidence:

**`stopSeq` (US 0x803068d8) — no liveness check, blind unk38 deref:**
```c
param_2[0x34..0x37] = 0;                        // sound->unk34 = nullptr
iVar6 = FUN_8030d0e4(param_2);                  // getSeqParameter() == unk38, NO null check
*(undefined4 *)(iVar6 + 0x1850) = 0;            // seqParam->unk1850 = 0  <- on a released
                                                //    handle unk38==0: writes guest 0x1850
if (2 < param_2[1]) { /* releaseAutoHeapPointer */ }
param_2[1] = 0;
FUN_8030268c(param_1,uVar5);                    // releaseSeqParameterPointer(seqParam)
FUN_803024dc(param_1,*param_1 + 0x210,param_2); // releaseControllerHandle -- RE-SPLICES
*(undefined4 *)(... + (uint)*param_2 * 0x50 + 0x48) = 0;
```

**`releaseControllerHandle` (US 0x803024dc) — unconditional middle-unlink deref:**
```c
if (param_2[1] == param_3) { /* head removal */ }
else {
  *(undefined4 *)(*(int *)(param_3 + 0x2c) + 0x30) = *(undefined4 *)(param_3 + 0x30);
  // sound->unk2C->unk30 = sound->unk30 -- no membership/liveness check
}
```

**`stopTrackBGMs` (US 0x80016740) — the ONLY liveness gate, already transcribed:**
```c
iVar2 = (&DAT_803e9c80)[uVar1];                              // smBgmInTrack[i]
if ((iVar2 != 0) && (iVar2 = *(int *)(iVar2 + 0x14), iVar2 != 0)) {
  FUN_8030a54c(iVar2,param_2);                               // unk14->stop(param2)
  (&DAT_803e9c80)[uVar1] = 0;
}
```

## Retail's real protection: the unk34 back-pointer protocol (fully transcribed)

- `initSoundParameter` (US 0x803049f8): `*(param_2+0x34) = param_3; ...
  *param_3 = param_2;` — the handle remembers the game-side `JAISound**`
  (e.g. `&MSBgm::unk14`).
- `clearMainSoundPPointer` (US 0x8030a520): `if (*(param_1+0x34))
  **(param_1+0x34) = 0;` — nulls the game-side cache.
- Every ordinary release site calls it before `stopSeq`: `stopSoundHandle`
  BGM case, `checkPlayingSeqTrack` deferred fade-stop, `storeBuffer`
  track-replace/priority-replace, `checkStoppedSeq`, `checkFadeoutSeq` —
  retail and decomp agree at every site.

So on GC a stopped BGM normally nulls `MSBgm::unk14`, and `stopTrackBGMs`'s
`unk14 != 0` gate makes the second stop unreachable.

**Retail-latent bug:** two retail release paths SKIP the clear —
`checkEntriedSeq`'s alloc-failure `stopSeq` (US 0x80306a1c end, decomp
JAIGFrameSequence.cpp:158) and `checkReadSeq`'s `setSeqData==-1` failure
(US 0x80307fac, decomp :648). Both leave the game-side cache dangling; GC then
performs the double release **silently** — `*(0+0x1850)` lands in unprotected
low guest RAM, the free-list re-splice self-loops, the stale-`unk2C` splice
writes into a sibling pool handle. All mapped, no MMU trap, ships that way.

## What was actually wrong natively, and the fix

`unk38 == null` in `stopSeq` means **already released** (only
`releaseControllerHandle` nulls `unk38`; `JAISeqEntry::storeBuffer` guarantees
it non-null before a handle is ever registered). The 2026-07-09 stopgap
early-out misread this state as "backend never attached a JASeqParameter" and
**re-released**: `releaseControllerHandle` again (free-list self-loop, stale
`unk2C` splice into sibling handles) plus a stale-`unk0` track-slot clobber.
The 2026-07-10 `unk2C` null-guard then hid the null-deref case of that
corruption but not the stale-pointer case.

Fix (this commit): the early-out is now a **list no-op** (`unk34 = nullptr;
unk1 = 0; return;`) — converging to retail's *observable* behavior (second
stop does nothing user-visible) without reproducing the unprotected-memory
corruption we cannot absorb. Both stopgap comments rewritten to name the true
state. The `releaseControllerHandle` null-guard stays for the stream-handle
double-release (same retail pattern, `unk21C` buffer), which still routes
there.

## Dead ends / negative results

- Audited for a dropped `clearMainSoundPPointer` at every retail release site
  — none missing; transcription is faithful everywhere in this chain.
- `MSound::unkC4` aliasing theory (exitStage stops it after the BGM table):
  ruled out — decomp never assigns it non-null.
- gdb-hosted repro is unusable for this: a ~30min gdb-slowed headless run died
  of Vulkan `VK_ERROR_DEVICE_LOST` (Dawn abort), unrelated to the game defect.

## Verification

`SB_HEADLESS=1 SB_STAGE=15 SB_SCENARIO=0 SB_TURBO=1 SB_MOVIE_DBG=1`, timeout
90 (turbo) and 120 (paced): both runs survive the full window, cycling the
title attract loop far past the former ~present-3200 crash point (49
TMarDirector teardowns in the turbo run), exit only by timeout SIGKILL
(rc=137). No SIGSEGV, no new crash signatures. Logs:
`scratch/logs/wf_stopseq_fix.log`, `scratch/logs/wf_stopseq_fix_paced.log`.

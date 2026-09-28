---
id: 38
title: decomp convergence gate lost both its arms when the retired executor went
status: open
symptom: `rebase_upstream.py converge` and `audit` cannot run at all any more, and the marker scheme that replaces them has already misclassified files once.
state_items: S001
tags: decomp,upstream,convergence,verification,instrument
created: 2026-09-28
updated: 2026-10-01
---

## Root cause

The convergence workflow certified a rebase with two arms: compile the `sms-boot`
target, then run the bounded decomp gameplay smoke. Both arms died with the
retired executor, and neither was replaced:

- `sms-boot` is no longer in the parent build. `CMakeLists.txt` builds
  `native-render`, `title-adapter` and `tools/gcnport_boot` only, so
  `build_dir()` refuses — correctly, since a green that never compiled is not
  evidence.
- `run.sh` refuses the decomp runtime by design, so `run_runtime_smoke()` can
  only ever return "runtime RED, exit 2".
- Nothing in the tree compiles `decomp/sms` at all: zero references in
  `build/compile_commands.json`.

**2026-10-01: the compile arm has a real replacement; the runtime arm is still
refused by design, and this issue stays open.** `tools/decomp/hostcheck.py` now
parses all 580 decomp translation units in both build modes and runs in the
canonical verifier (`tools/verification.py`, step "decomp host compile"), with a
self-test that proves it can report both a clean unit and a broken one. That is
the compile arm back, with a stated scope: it proves the decomp's source parses
as this port and as upstream maintain it, and it deliberately does not link the
game or run a guest instruction, so it cannot stand in for the gameplay smoke.
See issue 39 for the measurement and the per-cause breakdown. The triage step
below is still unpromoted, so `converge` and `audit` still cannot run.

That left `classify()`'s marker screen as the only automated basis for adopting
upstream's copy of a file, and C044 records it misclassifying six files that
built green and then segfaulted. It is a *negative* signal — no marker means
"free candidate" — and a negative signal with a falsified history is the wrong
basis for deleting work.

A second, quieter defect: upstream's relocation into `libs/<lib>/` is
asymmetric (`include/` keeps the library name twice, `src/` drops it), so a
single prefix rule maps every `src/` file to a path that does not exist. The
first version of the loss check silently skipped 90 files and reported a clean
result.

## What the 2026-09-28 sync actually did

Replaced the build-and-run gate with the one property still checkable without a
compiler: **no line this fork added may be silently dropped**. For every file
edited since the fork point, each added line is looked for in the merged result,
relocation-aware, and files upstream merely reworded are separated from files
where a fix is genuinely gone by counting *strong* marker occurrences
(`SMS_NATIVE_PLATFORM`, `SUNBRIGHT-KEEP`, `STOPGAP`, provenance comments,
`sb_host_alloc`, and the `uintptr_t`/`intptr_t` pair as one equivalence class,
because upstream independently made several of the same LP64 fixes with the
sibling type).

That check found 43 files the merge would have stripped, against a `-X theirs`
merge that reported no conflicts at all. See C044.

The gate is now `tools/re/convergence_loss.py`, in the normal verifier. Its
self-test is the merge itself, replayed from its own refs
(`--fork 40c2594b --ours pre-upstream-merge-2026-09-28` against the merged
result the sync produced): that replay reports loss in 56 files, so the check is
known to go red on a real loss rather than only ever having answered "clean".
Against the current tree it reports 355 of 355 delta files intact.

**2026-10-01: the control's answer was depending on the working tree.** The
replay read the merged side out of the live index (`:path`), so its number moved
as the decomp converged — 54 files when issue 38 was written, 76 after the
convergence commit — and, worse, a convergence that removed every last dropped
line would have driven it to zero and made the self-test FAIL. An instrument
that breaks when the thing it measures gets better is not an instrument. The
merged side is now pinned to `d145df88`, the decomp commit the sync actually
landed (`MERGED_CONTROL`), the tool refuses to guess if that commit is not in the
clone, and `--merged` overrides it. The pinned answer is 56 files, reproducibly.
The same run also fixed a bare `FileNotFoundError`: the real check printed its
verdict and then crashed writing `scratch/decomp-sync/loss_review.txt` into a
gitignored directory that gets wiped, which reads like the check itself failed.

**And the check has a scope limit that cost a real removal, now stated in its own
docstring.** "Anything this fork added" means added *since the fork point*
(40c2594b, 2026-08-30). The empty stubs that replaced ~25 recovered
`BathtubKiller` bodies in March 2026 (ab285a7e "BathtubKiller scaffolding") sit
five months *before* that point, so replacing them with upstream's decompiled
bodies reported "intact" instead of flagging a removal. The check answered its own
question correctly and would still be the wrong instrument for "did we lose fork
work here". Pre-fork-point drift is caught by measuring both trees directly, as
the 2026-10-01 debt pass did.

**2026-10-01: the triage claim in this issue was stale, and one of its two gaps closed.**
This issue said the triage step "is not yet promoted; it is still in
`scratch/decomp-sync/`". That was wrong: the triage is inside the promoted
`convergence_loss.py`, which reports each absent line with the distinctive tokens
it carried and separates "upstream reworded it" (`token still present in merged
file: SMS_NATIVE_PLATFORM`, …) from "the fix is gone" (`NONE`) — see its module
docstring's REVIEW clause. What `scratch/decomp-sync/` actually contains is
`loss_review.txt`, the report that tool *writes*. The live tree confirms it: 355
of 355 local-delta files intact, no REVIEW, no UNRESOLVED.

What genuinely remains is the other arm and the marker screen: `converge` and
`audit` still cannot run, because `build_dir()` refuses the retired `sms-boot`
target and `run.sh` refuses the decomp runtime by design; and `classify()`'s
marker text is still the only automated basis for adopting upstream's copy of a
file, which is the falsified history C044 records. The compile arm now has a
real replacement (issue 39, `tools/decomp/hostcheck.py`); the gameplay arm has
none and this issue stays open for it.

## The 43 held files

Recorded here rather than left in `scratch/`, which is gitignored and gets wiped.
Each is at our side because a merge would have stripped native work from it. The
reason each was held is reproducible with
`convergence_loss.py --fork 40c2594bd2706b709f75be0bca01acb283248693 --ours pre-upstream-merge-2026-09-28`.

    include/Camera/Camera.hpp                       our (intptr_t) cast absent from merged
    include/JSystem/JSupport.hpp                    merged casts offset to s32, we used uintptr_t
    include/JSystem/J3D/.../J3DShape.hpp             SMS_NATIVE_PLATFORM 1->0
    include/JSystem/JGadget/linklist.hpp            SMS_NATIVE_PLATFORM 1->0
    include/dolphin/ar.h                            SMS_NATIVE_PLATFORM 1->0
    src/Enemy/BathtubKiller.cpp                     "Native port of" provenance 1->0
    src/GC2D/CardLoad.cpp                           SMS_NATIVE_PLATFORM 18->16
    src/GC2D/Guide.cpp                              "Native port of" provenance 1->0
    src/GC2D/SelectDir.cpp                          SMS_NATIVE_PLATFORM 1->0
    src/JSystem/J2D/J2DPane.cpp                     SMS_NATIVE_PLATFORM 5->4
    src/JSystem/J3D/.../J3DDrawBuffer.cpp            SMS_NATIVE_PLATFORM 19->18
    src/JSystem/J3D/.../J3DShape.cpp                 SMS_NATIVE_PLATFORM 8->6
    src/JSystem/JAudio/JASystem/JASBNKParser.cpp    SMS_NATIVE_PLATFORM 2->1
    src/JSystem/JAudio/JASystem/JASCmdStack.cpp     SMS_NATIVE_PLATFORM 3->1
    src/JSystem/JAudio/JASystem/JASDvdThread.cpp    SMS_NATIVE_PLATFORM 6->2
    src/JSystem/JAudio/JASystem/JASBankMgr.cpp      dropped (u32)(uintptr_t) width assignment
    src/JSystem/JAudio/JASystem/JASTrack.cpp        SMS_NATIVE_PLATFORM 6->4
    src/JSystem/JAudio/JASystem/JASVload.cpp        SMS_NATIVE_PLATFORM 3->0
    src/JSystem/JDrama/JDRCamera.cpp                 SMS_NATIVE_PLATFORM 4->2
    src/JSystem/JKernel/JKRHeap.cpp                  SMS_NATIVE_PLATFORM 5->4, sb_host_alloc 8->0
    src/JSystem/JKernel/JKRDvdArchive.cpp            dropped mDataOffset init + VERSION_SELECT
    src/JSystem/JKernel/JKRMemArchive.cpp            SMS_NATIVE_PLATFORM 7->4
    src/JSystem/JKernel/JKRSolidHeap.cpp             SMS_NATIVE_PLATFORM reduced
    src/JSystem/JKernel/JKRThread.cpp                SMS_NATIVE_PLATFORM reduced
    src/JSystem/JParticle/JPAEmitterLoader.cpp       SMS_NATIVE_PLATFORM reduced
    src/JSystem/JRenderer.cpp                        SMS_NATIVE_PLATFORM reduced
    src/JSystem/JUtility/JUTResFont.cpp              SMS_NATIVE_PLATFORM reduced
    src/MSound/MSound.cpp                            SMS_NATIVE_PLATFORM reduced
    src/MSound/MSoundSE.cpp                          SMS_NATIVE_PLATFORM reduced
    src/MSound/MSoundMainSide.cpp                    SMS_NATIVE_PLATFORM reduced
    src/MarioUtil/ShadowUtil.cpp                     SMS_NATIVE_PLATFORM reduced
    src/Map/MapMakeList.cpp                          SMS_NATIVE_PLATFORM reduced
    src/MoveBG/MapObjBase.cpp                        SMS_NATIVE_PLATFORM reduced
    src/MoveBG/MapObjOption.cpp                      SMS_NATIVE_PLATFORM reduced
    src/MoveBG/MapObjRailBlock.cpp                   SMS_NATIVE_PLATFORM reduced
    src/System/Application.cpp                       SMS_NATIVE_PLATFORM reduced
    src/System/MarDirectorDirect.cpp                 SMS_NATIVE_PLATFORM reduced
    src/System/MarDirectorPreEntry.cpp               SMS_NATIVE_PLATFORM 2->1
    src/System/MarDirectorSetupObjects.cpp            SMS_NATIVE_PLATFORM 6->5
    src/System/MarNameRefGen_BossEnemy.cpp           SUNBRIGHT-KEEP 1->0
    src/System/MarNameRefGen_NPC.cpp                 SMS_NATIVE_PLATFORM 1->0, STOPGAP 1->0
    src/System/MarioGamePad.cpp                      SUNBRIGHT-KEEP 1->0
    src/System/MenuDir.cpp                           SMS_NATIVE_PLATFORM 1->0

Four more keep native code written against field names upstream had just
renamed, and were kept verbatim rather than have the names guessed:
`JAIBasic.cpp`, `JAIData.cpp`, `JAIGFrameSequence.cpp`, `JAISystemInterface.cpp`.
The last two are now reconciled; see the decomp commit "Name the audio fields the
merge left our side calling by offset".

## What is still open

- The 43 held files are knowingly behind upstream and each needs hand
  reconciliation against upstream's current version.
- `TPortArgs::unk1C` / `unk20` are still unnamed; `docs/re_notes/audio_port_args.md`
  records the retail evidence and why it is not yet enough.
- The triage step is not promoted; `convergence_loss.py` reports the candidates
  and a human reads them, which is slower but is not yet wrong.

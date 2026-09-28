---
id: 38
title: decomp convergence gate lost both its arms when the retired executor went
status: open
symptom: `rebase_upstream.py converge` and `audit` cannot run at all any more, and the marker scheme that replaces them has already misclassified files once.
state_items: S001
tags: decomp,upstream,convergence,verification,instrument
created: 2026-09-28
updated: 2026-09-28
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
(`--fork 40c2594b --ours pre-upstream-merge-2026-09-28` against the merged index):
that replay reports loss in 54 files, so the check is known to go red on a real
loss rather than only ever having answered "clean". Against the current tree it
reports 334 of 334 delta files intact.

The triage step that separates "upstream reworded it" from "the fix is gone" is
not yet promoted; it is still in `scratch/decomp-sync/`. It is the part that
needs a promoted form and its own control before it is trusted in the gate,
because it is the step that reads a diff and applies judgement.

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

---
id: 1
title: Stage 9 (Noki Bay) aborts at boot: /scene/mapObj missing from the mounted archive
status: investigating
symptom: SBR_STAGE=9 (Noki Bay, mare0.szs) aborts at boot: NULL-pointer r32 at guest address 0x00000000, guest stack MActorAnmData::init <- TObjManager::createAnmData <- TMActorKeeper <- TMapObjBase::makeMActors <- TJumpBase::initMapObj
tags: recomp,assets,jkr,stage9,boot-crash
created: 2026-08-11
updated: 2026-08-11
---

## What is measured

`SBR_FASTBOOT=1 SBR_STAGE=9` dies before rendering a frame. Every scenario does the same
(`SBR_SCENARIO=0,1,2` all abort identically), so it is not an episode-selection artifact.

The crash is three layers from its cause. `MActorAnmData::init` does, with no null check:

    JKRFileFinder* f = JKRFileLoader::findFirstFile(dir);
    do { addFileNum(f->mFileName); } while (f->findNextFile());

The decomp carries an `#ifdef SMS_NATIVE_PLATFORM` guard at exactly this spot
(`decomp/sms/src/M3DUtil/MActorData.cpp:140`) — the recomp runs the game's real PPC code, where
that guard does not exist and cannot.

## The located fact

`sms-recomp/overrides/diag_anmdata.cpp` (SB_LOG=anmdata) logs the directory, the disc paths and
every `findFirstFile` result. On stage 9:

    DVDConvertPathToEntrynum("mare0.szs") -> 113      (mounts, entry found)
    findFirstFile("/common/map")    -> ok
    findFirstFile("/scene/map/map") -> ok
    findFirstFile("/scene/mapObj")  -> NULL

The same lookup succeeds repeatedly on stage 8 (monte0.szs) and stage 7 (delfino0.szs), so it is
this stage's archive, not the lookup.

## What is NOT known, and must not be assumed

Whether `/scene/mapObj` is genuinely absent from mare0.szs or whether our mount of it is incomplete.
Two facts point away from a truncated read: the DVD log shows the whole file streaming in clean
0x8000 chunks with a final partial 0x6500 through the two JKRDecomp ping-pong buffers, and the RARC
directory table sits at the FRONT of the archive, so late corruption could not drop a directory
entry. Against that, the stage's own object list creates a TJumpBase, which is a TMapObjBase and so
needs the mapObj resources — a retail disc whose Noki Bay had no mapObj directory would crash on
console the same way, which cannot be true.

Next probe, not yet run: walk the mounted RARC node table and print its directory names, which
settles "absent from the archive" against "absent from our mount" without inference.

## Do not

Do not "fix" this by null-guarding the recomp side. The guest's own code is what dereferences it,
the archive contents are the input, and a guard would turn a missing-asset bug into a stage that
loads with no map-object animations — a silent success shape.

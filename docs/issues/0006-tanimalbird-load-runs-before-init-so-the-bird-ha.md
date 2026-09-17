---
id: 6
title: TAnimalBird::load runs BEFORE init(), so the bird has no MActor when retail expects one
status: wontfix
symptom: segfault in TAnimalBird::load under TMarDirector::setupObjects; getModel() dereferences a null mMActor at scene load
tags: port,animal,bird,ordering,delfino
created: 2026-08-12
updated: 2026-09-17
---

FOUND 2026-08-12 while porting TAnimalBird::load (US 0x8000dea8). Confirmed from a core dump, not inferred: eu-stack puts the fault at frame #0 in TAnimalBird::load, called from TStrategy::load -> TViewObjPtrListT::load -> TMarDirector::setupObjects.

MECHANISM. TLiveActor::getModel() is mMActor->unk4. TAnimalBird::init() is what creates mMActor (mMActorKeeper->createMActor("bird_man.bmd", 0)). In this port init() has not run when load() does, so mMActor is null and retail's unconditional getModel() in load() faults.

WHY RETAIL DOES NOT. Retail reaches load() with the actor already initialised, because the manager creates and inits its objects before the scene stream loads them. Our birds are not manager-created yet — the TAnimalBirdManager arc is unported — so the order is inverted.

THIS IS THE ROOT CAUSE FOR MORE THAN THE TINT. Any ported *::load that touches the model will hit the same wall for the same reason. Treat a null model at load() time as this issue, not as a per-actor bug, and do not add per-call-site null checks as the fix.

CURRENT STATE: TAnimalBird::load is ported and lands, with a LOUD documented seam that skips only the body tint when mMActor is null. The species selection, the carried-item spawn and the blue-coin dead-at-birth logic all run.

SECOND, SEPARATE GAP visible in the same run: newAndRegisterObjByEventID returns null for the bird's event id, because our version has 'default: return nullptr' for unimplemented item types. That is what the run currently reports, so the model seam is not even reached yet.

PROPER FIX: manager-driven creation + init ordering (TAnimalBirdManager). Not a null check.

### Note (2026-09-17)
Blocked, not fixed: this issue targets TAnimalBird::load/init ordering in the retired offline-generated-corpus product (sms-boot/boot_stubs/enemy_stubs.cpp + TMarDirector::setupObjects). That product was retired 2026-09-04 by commit b6bd8ff4 (issue #37, S009 verified-absent). CMakeLists.txt now builds no gameplay executable at all -- there is no SB_STAGE/SB_HEADLESS boot to run, and AGENTS.md forbids reconstructing or running the retired executor ('must not be reconstructed or run'; 'Removed executor artifacts...must not return as a migration bridge, oracle, or comparison arm'). Porting TAnimalBirdManager into sms-boot now would be exactly that: reviving gameplay-ordering logic in a non-product tree that issue #37 explicitly says not to revive. sms-boot/boot_stubs is classified evidence-only in docs/codemap.md, not a runnable gameplay path. Leaving status wontfix pending the gcnport/Dolphin-JIT migration (S001/S008); the TAnimalBase/TAnimalBird RE facts (US addrs, init/load ordering, manager-driven creation requirement) remain valid evidence and should be re-applied to whatever plaza-population owner exists once gcnport gameplay boots. No code changed.

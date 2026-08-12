---
id: 6
title: TAnimalBird::load runs BEFORE init(), so the bird has no MActor when retail expects one
status: open
symptom: segfault in TAnimalBird::load under TMarDirector::setupObjects; getModel() dereferences a null mMActor at scene load
tags: port,animal,bird,ordering,delfino
created: 2026-08-12
updated: 2026-08-12
---

FOUND 2026-08-12 while porting TAnimalBird::load (US 0x8000dea8). Confirmed from a core dump, not inferred: eu-stack puts the fault at frame #0 in TAnimalBird::load, called from TStrategy::load -> TViewObjPtrListT::load -> TMarDirector::setupObjects.

MECHANISM. TLiveActor::getModel() is mMActor->unk4. TAnimalBird::init() is what creates mMActor (mMActorKeeper->createMActor("bird_man.bmd", 0)). In this port init() has not run when load() does, so mMActor is null and retail's unconditional getModel() in load() faults.

WHY RETAIL DOES NOT. Retail reaches load() with the actor already initialised, because the manager creates and inits its objects before the scene stream loads them. Our birds are not manager-created yet — the TAnimalBirdManager arc is unported — so the order is inverted.

THIS IS THE ROOT CAUSE FOR MORE THAN THE TINT. Any ported *::load that touches the model will hit the same wall for the same reason. Treat a null model at load() time as this issue, not as a per-actor bug, and do not add per-call-site null checks as the fix.

CURRENT STATE: TAnimalBird::load is ported and lands, with a LOUD documented seam that skips only the body tint when mMActor is null. The species selection, the carried-item spawn and the blue-coin dead-at-birth logic all run.

SECOND, SEPARATE GAP visible in the same run: newAndRegisterObjByEventID returns null for the bird's event id, because our version has 'default: return nullptr' for unimplemented item types. That is what the run currently reports, so the model seam is not even reached yet.

PROPER FIX: manager-driven creation + init ordering (TAnimalBirdManager). Not a null check.

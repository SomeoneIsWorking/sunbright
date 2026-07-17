// RING-2 SCAFFOLD — vtable stubs for boot_buckets2/classes2.txt
// Defines the key function (first non-inline virtual declared in each class body,
// or the first method that overrides a base-class virtual) to emit the vtable
// for each class. Faithful ports come later.
//
// 4 explicit ctors: TBathWaterPreprocessor, THorizontalViking, TSandBase, TSimpleEffect
// 66 vtable-needing classes

// ============================================================
// === Animal/AnimalBase.hpp ===
// ============================================================
#include "stub_trace.h"
#include <Animal/AnimalBase.hpp>

// TAnimalBase: key = load (first explicit virtual)
void TAnimalBase::load(JSUMemoryInputStream&) { SB_STUB_HIT("TAnimalBase::load"); }

// ============================================================
// === Enemy/AreaCylinder.hpp ===
// ============================================================
#include <Enemy/AreaCylinder.hpp>

// TAreaCylinder: key = load (first explicit virtual)
void TAreaCylinder::load(JSUMemoryInputStream&) { SB_STUB_HIT("TAreaCylinder::load"); }

// ============================================================
// === Enemy/EffectObj.hpp ===
// ============================================================
#include <Enemy/EffectObj.hpp>

// TSimpleEffect ctor (explicit ctor in classes2.txt)
TSimpleEffect::TSimpleEffect(const char* name)
    : JDrama::TActor(name)
    , unk44(0)
{
}

// TSimpleEffect: key = perform (first non-inline virtual; emitEffect is pure)

// ============================================================
// === Enemy/Generator.hpp ===
// ============================================================
#include <Enemy/Generator.hpp>

// TGenerator: key = load (first explicit virtual)
void TGenerator::load(JSUMemoryInputStream&) { SB_STUB_HIT("TGenerator::load"); }

// TOneShotGenerator::load lives natively in decomp/sms/src/Enemy/Generator.cpp.

// ============================================================
// === Map/BathWaterManager.hpp ===
// ============================================================
#include <Map/BathWaterManager.hpp>

// TBathWaterPreprocessor ctor (explicit ctor in classes2.txt)
TBathWaterPreprocessor::TBathWaterPreprocessor(TBathWaterManager* mgr)
    : JDrama::TViewObj("<バスタブ前処理>")
    , unk10(mgr)
{
}

// TBathWaterPreprocessor::perform lives natively in decomp/sms/src/Map/BathWaterManager.cpp.

// TBathWaterManager: key = load (first explicit virtual; ctor already in ui_map_stubs.cpp)
void TBathWaterManager::load(JSUMemoryInputStream&) { SB_STUB_HIT("TBathWaterManager::load"); }

// ============================================================
// === MoveBG/MapObjBall.hpp ===
// ============================================================
#include <MoveBG/MapObjBall.hpp>

// TMapObjBall: key = receiveMessage (first explicit virtual)
BOOL TMapObjBall::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TMapObjBall::receiveMessage"); return 0; }

// TResetFruit::perform lives natively in decomp/sms/src/MoveBG/MapObjBall.cpp — no stub.

// TRandomFruit: key = initMapObj (first and only explicit virtual)
void TRandomFruit::initMapObj() { SB_STUB_HIT("TRandomFruit::initMapObj"); }

// TBigWatermelon: key = loadAfter (first explicit virtual)
void TBigWatermelon::loadAfter() { SB_STUB_HIT("TBigWatermelon::loadAfter"); }

// ============================================================
// === MoveBG/MapObjItem2.hpp ===
// ============================================================
#include <MoveBG/MapObjItem2.hpp>

// TMushroom1up: key = load (first explicit virtual)
// [dedup] void TMushroom1up::load(JSUMemoryInputStream&) {} 
// TJumpBase: key = receiveMessage (first explicit virtual)
// [dedup] BOOL TJumpBase::receiveMessage(THitActor*, u32) { return 0; } 
// ============================================================
// === MoveBG/MapObjMamma.hpp ===
// ============================================================
#include <MoveBG/MapObjMamma.hpp>

// TSandBase ctor (explicit ctor in classes2.txt)
// TSandBase has no virtual-overriding methods (isDown/withering not in TMapObjBase),
// so the constructor alone emits the vtable as a weak symbol.
TSandBase::TSandBase(const char* name)
    : TMapObjBase(name)
{
}
void TSandBase::withering() { SB_STUB_HIT("TSandBase::withering"); }
void TSandBase::isDown() const { SB_STUB_HIT("TSandBase::isDown"); }

// TSandLeafBase: key = control (first method shadowing TMapObjBase::control;
//   grow is not a TMapObjBase virtual)
void TSandLeafBase::control() { SB_STUB_HIT("TSandLeafBase::control"); }

// TSandBombBase: key = control (first method shadowing TMapObjBase::control;
//   withered/expanded/exploding/explode/waitBeforeExplode/grow not in base)
void TSandBombBase::control() { SB_STUB_HIT("TSandBombBase::control"); }

// TSandCastle: key = calcRootMatrix (first method shadowing TMapObjBase::calcRootMatrix;
//   withering/expanded/explode/waitBeforeExplode/findTriggerActor not in base)
void TSandCastle::calcRootMatrix() { SB_STUB_HIT("TSandCastle::calcRootMatrix"); }

// TLeanMirror: key = draw (first method shadowing TMapObjBase::draw;
//   enemyIsOn is not a TMapObjBase virtual, but draw IS and comes second)
void TLeanMirror::draw() const { SB_STUB_HIT("TLeanMirror::draw"); }

// TShiningStone: key = perform (first method shadowing THitActor::perform;
//   endDemo/putOnLight not in THitActor virtuals)
// TShiningStone::perform lives natively in decomp/sms/src/MoveBG/MapObjMamma.cpp.

// TMammaBlockRotate: key = touchWater (first method shadowing TMapObjBase::touchWater)
u32 TMammaBlockRotate::touchWater(THitActor*) { SB_STUB_HIT("TMammaBlockRotate::touchWater"); return 0; }

// TMammaYacht: key = control (first method shadowing TMapObjBase::control)
void TMammaYacht::control() { SB_STUB_HIT("TMammaYacht::control"); }

// TSandBird: key = control (first explicit virtual)
void TSandBird::control() { SB_STUB_HIT("TSandBird::control"); }

// TGoalWatermelon: key = touchActor (first method shadowing TMapObjBase::touchActor)
void TGoalWatermelon::touchActor(THitActor*) { SB_STUB_HIT("TGoalWatermelon::touchActor"); }

// TMammaMirrorMapOperator::perform lives natively in decomp/sms/src/MoveBG/MapObjMamma.cpp.

// TSandEgg: key = getSDLModelFlag (first method shadowing TMapObjBase::getSDLModelFlag)
u32 TSandEgg::getSDLModelFlag() const { SB_STUB_HIT("TSandEgg::getSDLModelFlag"); return 0; }

// ============================================================
// === MoveBG/MapObjMare.hpp ===
// ============================================================
#include <MoveBG/MapObjMare.hpp>

// TCogwheelScale: key = touchWater (first method shadowing TMapObjBase::touchWater)
u32 TCogwheelScale::touchWater(THitActor*) { SB_STUB_HIT("TCogwheelScale::touchWater"); return 0; }

// TCogwheel: key = draw (first method shadowing TMapObjBase::draw;
//   initDraw/rebound/calc are not TMapObjBase... wait calc IS. But draw comes first in class)
// Actually TCogwheel class body order: initDraw(not base), draw(yes base), rebound(not), calc(yes), ...
// draw is listed before calc → key = draw
void TCogwheel::draw() const { SB_STUB_HIT("TCogwheel::draw"); }

// TMapObjElasticCode: key = draw (first method shadowing TMapObjBase::draw)
void TMapObjElasticCode::draw() const { SB_STUB_HIT("TMapObjElasticCode::draw"); }

// TMapObjGrowTree: key = touchWater (first method shadowing TMapObjBase::touchWater;
//   getGrowHeightFromRate/updateHeight not in base)
u32 TMapObjGrowTree::touchWater(THitActor*) { SB_STUB_HIT("TMapObjGrowTree::touchWater"); return 0; }

// TWireBell: key = draw (first method shadowing TMapObjBase::draw;
//   initDraw is not a TMapObjBase virtual)
void TWireBell::draw() const { SB_STUB_HIT("TWireBell::draw"); }

// TMapObjPuncher: key = touchPlayer (first method shadowing TMapObjBase::touchPlayer)
void TMapObjPuncher::touchPlayer(THitActor*) { SB_STUB_HIT("TMapObjPuncher::touchPlayer"); }

// TMuddyBoat: key = calcRootMatrix (first method shadowing TMapObjBase::calcRootMatrix;
//   moveByWater not in base)
void TMuddyBoat::calcRootMatrix() { SB_STUB_HIT("TMuddyBoat::calcRootMatrix"); }

// TMareFall: key = calc (first method shadowing TMapObjBase::calc)
void TMareFall::calc() { SB_STUB_HIT("TMareFall::calc"); }

// TMareCork: key = loadAfter (first method shadowing TMapObjBase::loadAfter)
void TMareCork::loadAfter() { SB_STUB_HIT("TMareCork::loadAfter"); }

// TMareEventPoint: key = receiveMessage (first method shadowing THitActor::receiveMessage)
BOOL TMareEventPoint::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TMareEventPoint::receiveMessage"); return 0; }

// ============================================================
// === MoveBG/MapObjMonte.hpp ===
// ============================================================
#include <MoveBG/MapObjMonte.hpp>

// TMapObjMonteRoot: key = initMapObj (only method, shadows TMapObjBase::initMapObj)
void TMapObjMonteRoot::initMapObj() { SB_STUB_HIT("TMapObjMonteRoot::initMapObj"); }

// TJumpMushroom: key = receiveMessage (first method shadowing TMapObjBase::receiveMessage)
BOOL TJumpMushroom::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TJumpMushroom::receiveMessage"); return 0; }

// THangingBridgeBoard: key = control (first method shadowing base virtual;
//   drawOneRope/drawRopes/push/pushNeighbor not in base)
void THangingBridgeBoard::control() { SB_STUB_HIT("THangingBridgeBoard::control"); }

// THangingBridge: key = perform (first method shadowing JDrama::TViewObj::perform;
//   drawLowerMinus/drawLowerPlus/drawUpper/setDrawPos/drawRopeBetweenBoards/initDraw not in base)
void THangingBridge::perform(u32, JDrama::TGraphics*) { SB_STUB_HIT("THangingBridge::perform"); }

// TSwingBoard: key = draw (first method shadowing TMapObjBase::draw;
//   drawOneRope/initDraw not in base, draw IS in base and comes before control)
void TSwingBoard::draw() const { SB_STUB_HIT("TSwingBoard::draw"); }

// TGoalFlag: key = getRadiusAtY (first method shadowing TMapObjBase::getRadiusAtY)
f32 TGoalFlag::getRadiusAtY(f32) const { SB_STUB_HIT("TGoalFlag::getRadiusAtY"); return 0.0f; }

// TFluffManager: key = control (first method shadowing TMapObjBase::control;
//   findNextFluff/registerNextFluff/setUpNextFluff/newFluff/getRandomX/getRandomZ not in base)
void TFluffManager::control() { SB_STUB_HIT("TFluffManager::control"); }

// ============================================================
// === MoveBG/MapObjPinna.hpp ===
// ============================================================
#include <MoveBG/MapObjPinna.hpp>

// TFerrisWheel: key = control (first method shadowing TMapObjBase::control;
//   becomeCalmlyCallback not in base)
void TFerrisWheel::control() { SB_STUB_HIT("TFerrisWheel::control"); }

// THorizontalViking ctor (explicit ctor in classes2.txt)
THorizontalViking::THorizontalViking(const char* name)
    : TMapObjBase(name)
{
}

// THorizontalViking: key = control (first method shadowing TMapObjBase::control;
//   updateTrans/moveNormal not in base)
void THorizontalViking::control() { SB_STUB_HIT("THorizontalViking::control"); }

// TViking: key = control (first method shadowing THorizontalViking/TMapObjBase::control;
//   roll not in base)
void TViking::control() { SB_STUB_HIT("TViking::control"); }

// TShellCup: key = control (first method shadowing TMapObjBase::control;
//   attachCoin/calcAfter not in base)
void TShellCup::control() { SB_STUB_HIT("TShellCup::control"); }

// TMerrygoround: key = control (first method shadowing TMapObjBase::control)
void TMerrygoround::control() { SB_STUB_HIT("TMerrygoround::control"); }

// TBalloonKoopaJr: key = touchActor (first method shadowing TMapObjGeneral/TMapObjBase::touchActor)
void TBalloonKoopaJr::touchActor(THitActor*) { SB_STUB_HIT("TBalloonKoopaJr::touchActor"); }

// TPinnaEntrance::loadAfter lives natively in decomp/sms/src/MoveBG/MapObjPinna.cpp.

// TWaterRecoverObj: key = touchPlayer (first and only method, shadows TMapObjBase::touchPlayer)
void TWaterRecoverObj::touchPlayer(THitActor*) { SB_STUB_HIT("TWaterRecoverObj::touchPlayer"); }

// TAmiKing: key = touchWater (first method shadowing TMapObjBase::touchWater)
u32 TAmiKing::touchWater(THitActor*) { SB_STUB_HIT("TAmiKing::touchWater"); return 0; }

// TPinnaCoaster: key = control (first method shadowing TMapObjBase::control)
void TPinnaCoaster::control() { SB_STUB_HIT("TPinnaCoaster::control"); }

// ============================================================
// === MoveBG/MapObjRicco.hpp ===
// ============================================================
#include <MoveBG/MapObjRicco.hpp>

// TCraneRotY: key = calc (first method shadowing TMapObjBase::calc)
void TCraneRotY::calc() { SB_STUB_HIT("TCraneRotY::calc"); }

// TCraneUpDown: key = control (first method shadowing TMapObjBase::control)
void TCraneUpDown::control() { SB_STUB_HIT("TCraneUpDown::control"); }

// TRiccoWatermill: key = touchWater (first method shadowing TMapObjBase::touchWater)
u32 TRiccoWatermill::touchWater(THitActor*) { SB_STUB_HIT("TRiccoWatermill::touchWater"); return 0; }

// TSurfGesoObj: key = initMapObj (first and only method, shadows TItem/TMapObjBase::initMapObj)
void TSurfGesoObj::initMapObj() { SB_STUB_HIT("TSurfGesoObj::initMapObj"); }

// TFruitSwitch: key = receiveMessage (first method shadowing TMapObjBase::receiveMessage;
//   pullUp/pushDown not in base)
BOOL TFruitSwitch::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TFruitSwitch::receiveMessage"); return 0; }

// TFruitLauncher: key = loadAfter (first method shadowing TMapObjBase::loadAfter;
//   appearFruit/fireObj not in base)
void TFruitLauncher::loadAfter() { SB_STUB_HIT("TFruitLauncher::loadAfter"); }

// ============================================================
// === MoveBG/MapObjTown.hpp ===
// ============================================================
#include <MoveBG/MapObjTown.hpp>

// TDoor: key = touchPlayer (first method shadowing TMapObjBase::touchPlayer)
// [dedup] void TDoor::touchPlayer(THitActor*) {} 
// TManhole: key = touchPlayer (first method shadowing TMapObjGeneral/TMapObjBase::touchPlayer)
// [dedup] void TManhole::touchPlayer(THitActor*) {} 
// TMapObjWaterSpray: key = calc (first method shadowing TMapObjBase::calc)
// [dedup] void TMapObjWaterSpray::calc() {} 
// THideObjInfo provided natively in decomp/sms/src/MoveBG/MapObjTown.cpp.

// TMapObjSwitch::control lives natively in decomp/sms/src/MoveBG/MapObjTown.cpp.

// TRedCoinSwitch: key = receiveMessage (first method shadowing TMapObjBase::receiveMessage)
// [dedup] BOOL TRedCoinSwitch::receiveMessage(THitActor*, u32) { return 0; } 
// TBasketReverse: key = kill (first method shadowing TMapObjBase::kill)
// [dedup] void TBasketReverse::kill() {} 
// ============================================================
// === MoveBG/MapObjTree.hpp ===
// ============================================================
#include <MoveBG/MapObjTree.hpp>

// TMapObjTree: key = perform (first explicit virtual)
// TMapObjTree::perform now PORTED in decomp/sms/src/MoveBG/MapObjTree.cpp (RE'd 2026-07-17).

// TMapObjTreeScale: key = loadAfter (first explicit virtual)
void TMapObjTreeScale::loadAfter() { SB_STUB_HIT("TMapObjTreeScale::loadAfter"); }

// ============================================================
// === MoveBG/MapObjWave.hpp ===
// ============================================================
#include <MoveBG/MapObjWave.hpp>

// TMapObjWave::load is now ported faithfully in decomp/sms/src/MoveBG/MapObjWave.cpp.

// ============================================================
// === MoveBG/ModelGate.hpp ===
// ============================================================
#include <MoveBG/ModelGate.hpp>

// TModelGate: key = getTakingMtx (first explicit virtual)
MtxPtr TModelGate::getTakingMtx() { SB_STUB_HIT("TModelGate::getTakingMtx"); return nullptr; }

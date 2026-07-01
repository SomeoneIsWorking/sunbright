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
#include <Animal/AnimalBase.hpp>

// TAnimalBase: key = load (first explicit virtual)
void TAnimalBase::load(JSUMemoryInputStream&) {}

// ============================================================
// === Enemy/AreaCylinder.hpp ===
// ============================================================
#include <Enemy/AreaCylinder.hpp>

// TAreaCylinder: key = load (first explicit virtual)
void TAreaCylinder::load(JSUMemoryInputStream&) {}

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
void TGenerator::load(JSUMemoryInputStream&) {}

// TOneShotGenerator: key = load (first explicit virtual)
void TOneShotGenerator::load(JSUMemoryInputStream&) {}

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

// TBathWaterPreprocessor::perform lives natively in reference/sms/src/Map/BathWaterManager.cpp.

// TBathWaterManager: key = load (first explicit virtual; ctor already in ui_map_stubs.cpp)
void TBathWaterManager::load(JSUMemoryInputStream&) {}

// ============================================================
// === MoveBG/MapObjBall.hpp ===
// ============================================================
#include <MoveBG/MapObjBall.hpp>

// TMapObjBall: key = receiveMessage (first explicit virtual)
BOOL TMapObjBall::receiveMessage(THitActor*, u32) { return 0; }

// TResetFruit::perform lives natively in reference/sms/src/MoveBG/MapObjBall.cpp — no stub.

// TRandomFruit: key = initMapObj (first and only explicit virtual)
void TRandomFruit::initMapObj() {}

// TCoverFruit: key = loadAfter (first explicit virtual)
void TCoverFruit::loadAfter() {}

// TBigWatermelon: key = loadAfter (first explicit virtual)
void TBigWatermelon::loadAfter() {}

// ============================================================
// === MoveBG/MapObjItem2.hpp ===
// ============================================================
#include <MoveBG/MapObjItem2.hpp>

// TMushroom1up: key = load (first explicit virtual)
void TMushroom1up::load(JSUMemoryInputStream&) {}

// TJumpBase: key = receiveMessage (first explicit virtual)
BOOL TJumpBase::receiveMessage(THitActor*, u32) { return 0; }

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
void TSandBase::withering() {}
void TSandBase::isDown() const {}

// TSandLeafBase: key = control (first method shadowing TMapObjBase::control;
//   grow is not a TMapObjBase virtual)
void TSandLeafBase::control() {}

// TSandBombBase: key = control (first method shadowing TMapObjBase::control;
//   withered/expanded/exploding/explode/waitBeforeExplode/grow not in base)
void TSandBombBase::control() {}

// TSandCastle: key = calcRootMatrix (first method shadowing TMapObjBase::calcRootMatrix;
//   withering/expanded/explode/waitBeforeExplode/findTriggerActor not in base)
void TSandCastle::calcRootMatrix() {}

// TLeanMirror: key = draw (first method shadowing TMapObjBase::draw;
//   enemyIsOn is not a TMapObjBase virtual, but draw IS and comes second)
void TLeanMirror::draw() const {}

// TShiningStone: key = perform (first method shadowing THitActor::perform;
//   endDemo/putOnLight not in THitActor virtuals)
// TShiningStone::perform lives natively in reference/sms/src/MoveBG/MapObjMamma.cpp.

// TMammaBlockRotate: key = touchWater (first method shadowing TMapObjBase::touchWater)
u32 TMammaBlockRotate::touchWater(THitActor*) { return 0; }

// TMammaYacht: key = control (first method shadowing TMapObjBase::control)
void TMammaYacht::control() {}

// TSandBird: key = control (first explicit virtual)
void TSandBird::control() {}

// TGoalWatermelon: key = touchActor (first method shadowing TMapObjBase::touchActor)
void TGoalWatermelon::touchActor(THitActor*) {}

// TMammaMirrorMapOperator::perform lives natively in reference/sms/src/MoveBG/MapObjMamma.cpp.

// TSandEgg: key = getSDLModelFlag (first method shadowing TMapObjBase::getSDLModelFlag)
u32 TSandEgg::getSDLModelFlag() const { return 0; }

// ============================================================
// === MoveBG/MapObjMare.hpp ===
// ============================================================
#include <MoveBG/MapObjMare.hpp>

// TCogwheelScale: key = touchWater (first method shadowing TMapObjBase::touchWater)
u32 TCogwheelScale::touchWater(THitActor*) { return 0; }

// TCogwheel: key = draw (first method shadowing TMapObjBase::draw;
//   initDraw/rebound/calc are not TMapObjBase... wait calc IS. But draw comes first in class)
// Actually TCogwheel class body order: initDraw(not base), draw(yes base), rebound(not), calc(yes), ...
// draw is listed before calc → key = draw
void TCogwheel::draw() const {}

// TMapObjElasticCode: key = draw (first method shadowing TMapObjBase::draw)
void TMapObjElasticCode::draw() const {}

// TMapObjGrowTree: key = touchWater (first method shadowing TMapObjBase::touchWater;
//   getGrowHeightFromRate/updateHeight not in base)
u32 TMapObjGrowTree::touchWater(THitActor*) { return 0; }

// TWireBell: key = draw (first method shadowing TMapObjBase::draw;
//   initDraw is not a TMapObjBase virtual)
void TWireBell::draw() const {}

// TMapObjPuncher: key = touchPlayer (first method shadowing TMapObjBase::touchPlayer)
void TMapObjPuncher::touchPlayer(THitActor*) {}

// TMuddyBoat: key = calcRootMatrix (first method shadowing TMapObjBase::calcRootMatrix;
//   moveByWater not in base)
void TMuddyBoat::calcRootMatrix() {}

// TMareFall: key = calc (first method shadowing TMapObjBase::calc)
void TMareFall::calc() {}

// TMareCork: key = loadAfter (first method shadowing TMapObjBase::loadAfter)
void TMareCork::loadAfter() {}

// TMareEventPoint: key = receiveMessage (first method shadowing THitActor::receiveMessage)
BOOL TMareEventPoint::receiveMessage(THitActor*, u32) { return 0; }

// ============================================================
// === MoveBG/MapObjMonte.hpp ===
// ============================================================
#include <MoveBG/MapObjMonte.hpp>

// TMapObjMonteRoot: key = initMapObj (only method, shadows TMapObjBase::initMapObj)
void TMapObjMonteRoot::initMapObj() {}

// TJumpMushroom: key = receiveMessage (first method shadowing TMapObjBase::receiveMessage)
BOOL TJumpMushroom::receiveMessage(THitActor*, u32) { return 0; }

// THangingBridgeBoard: key = control (first method shadowing base virtual;
//   drawOneRope/drawRopes/push/pushNeighbor not in base)
void THangingBridgeBoard::control() {}

// THangingBridge: key = perform (first method shadowing JDrama::TViewObj::perform;
//   drawLowerMinus/drawLowerPlus/drawUpper/setDrawPos/drawRopeBetweenBoards/initDraw not in base)
void THangingBridge::perform(u32, JDrama::TGraphics*) {}

// TSwingBoard: key = draw (first method shadowing TMapObjBase::draw;
//   drawOneRope/initDraw not in base, draw IS in base and comes before control)
void TSwingBoard::draw() const {}

// TGoalFlag: key = getRadiusAtY (first method shadowing TMapObjBase::getRadiusAtY)
f32 TGoalFlag::getRadiusAtY(f32) const { return 0.0f; }

// TFluffManager: key = control (first method shadowing TMapObjBase::control;
//   findNextFluff/registerNextFluff/setUpNextFluff/newFluff/getRandomX/getRandomZ not in base)
void TFluffManager::control() {}

// ============================================================
// === MoveBG/MapObjPinna.hpp ===
// ============================================================
#include <MoveBG/MapObjPinna.hpp>

// TFerrisWheel: key = control (first method shadowing TMapObjBase::control;
//   becomeCalmlyCallback not in base)
void TFerrisWheel::control() {}

// THorizontalViking ctor (explicit ctor in classes2.txt)
THorizontalViking::THorizontalViking(const char* name)
    : TMapObjBase(name)
{
}

// THorizontalViking: key = control (first method shadowing TMapObjBase::control;
//   updateTrans/moveNormal not in base)
void THorizontalViking::control() {}

// TViking: key = control (first method shadowing THorizontalViking/TMapObjBase::control;
//   roll not in base)
void TViking::control() {}

// TShellCup: key = control (first method shadowing TMapObjBase::control;
//   attachCoin/calcAfter not in base)
void TShellCup::control() {}

// TMerrygoround: key = control (first method shadowing TMapObjBase::control)
void TMerrygoround::control() {}

// TBalloonKoopaJr: key = touchActor (first method shadowing TMapObjGeneral/TMapObjBase::touchActor)
void TBalloonKoopaJr::touchActor(THitActor*) {}

// TPinnaEntrance: key = loadAfter (first and only method, shadows TMapObjBase::loadAfter)
void TPinnaEntrance::loadAfter() {}

// TWaterRecoverObj: key = touchPlayer (first and only method, shadows TMapObjBase::touchPlayer)
void TWaterRecoverObj::touchPlayer(THitActor*) {}

// TAmiKing: key = touchWater (first method shadowing TMapObjBase::touchWater)
u32 TAmiKing::touchWater(THitActor*) { return 0; }

// TPinnaCoaster: key = control (first method shadowing TMapObjBase::control)
void TPinnaCoaster::control() {}

// ============================================================
// === MoveBG/MapObjRicco.hpp ===
// ============================================================
#include <MoveBG/MapObjRicco.hpp>

// TCraneRotY: key = calc (first method shadowing TMapObjBase::calc)
void TCraneRotY::calc() {}

// TCraneUpDown: key = control (first method shadowing TMapObjBase::control)
void TCraneUpDown::control() {}

// TRiccoWatermill: key = touchWater (first method shadowing TMapObjBase::touchWater)
u32 TRiccoWatermill::touchWater(THitActor*) { return 0; }

// TSurfGesoObj: key = initMapObj (first and only method, shadows TItem/TMapObjBase::initMapObj)
void TSurfGesoObj::initMapObj() {}

// TFruitSwitch: key = receiveMessage (first method shadowing TMapObjBase::receiveMessage;
//   pullUp/pushDown not in base)
BOOL TFruitSwitch::receiveMessage(THitActor*, u32) { return 0; }

// TFruitLauncher: key = loadAfter (first method shadowing TMapObjBase::loadAfter;
//   appearFruit/fireObj not in base)
void TFruitLauncher::loadAfter() {}

// ============================================================
// === MoveBG/MapObjTown.hpp ===
// ============================================================
#include <MoveBG/MapObjTown.hpp>

// TDoor: key = touchPlayer (first method shadowing TMapObjBase::touchPlayer)
void TDoor::touchPlayer(THitActor*) {}

// TManhole: key = touchPlayer (first method shadowing TMapObjGeneral/TMapObjBase::touchPlayer)
void TManhole::touchPlayer(THitActor*) {}

// TMapObjWaterSpray: key = calc (first method shadowing TMapObjBase::calc)
void TMapObjWaterSpray::calc() {}

// THideObjInfo: key = perform (first method shadowing JDrama::TViewObj::perform)
void THideObjInfo::perform(u32, JDrama::TGraphics*) {}

// TMapObjSwitch: key = control (first method shadowing TMapObjBase::control)
void TMapObjSwitch::control() {}

// TRedCoinSwitch: key = receiveMessage (first method shadowing TMapObjBase::receiveMessage)
BOOL TRedCoinSwitch::receiveMessage(THitActor*, u32) { return 0; }

// TBasketReverse: key = kill (first method shadowing TMapObjBase::kill)
void TBasketReverse::kill() {}

// ============================================================
// === MoveBG/MapObjTree.hpp ===
// ============================================================
#include <MoveBG/MapObjTree.hpp>

// TMapObjTree: key = perform (first explicit virtual)
void TMapObjTree::perform(u32, JDrama::TGraphics*) {}

// TMapObjTreeScale: key = loadAfter (first explicit virtual)
void TMapObjTreeScale::loadAfter() {}

// ============================================================
// === MoveBG/MapObjWave.hpp ===
// ============================================================
#include <MoveBG/MapObjWave.hpp>

// TMapObjWave::load is now ported faithfully in reference/sms/src/MoveBG/MapObjWave.cpp.

// ============================================================
// === MoveBG/ModelGate.hpp ===
// ============================================================
#include <MoveBG/ModelGate.hpp>

// TModelGate: key = getTakingMtx (first explicit virtual)
MtxPtr TModelGate::getTakingMtx() { return nullptr; }

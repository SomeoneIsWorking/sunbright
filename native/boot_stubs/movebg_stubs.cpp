// native/boot_stubs/movebg_stubs.cpp
//
// Scaffold-to-link stubs for the MoveBG subsystem of the native SMS port.
//
// These are MINIMAL stub definitions whose sole purpose is to satisfy the
// linker when building libsms-native.a before the real implementations are
// ported.  Every body returns a neutral value (0 / nullptr / empty) and does
// nothing.  Functions already defined in reference/sms/src/MoveBG/ are NOT
// redefined here.
//
// Symbols covered: the 93 entries in scratch/boot_buckets/movebg.txt

#include <MoveBG/MapObjBase.hpp>
#include <MoveBG/MapObjGeneral.hpp>
#include <MoveBG/MapObjHide.hpp>
#include <MoveBG/MapObjEx.hpp>
#include <MoveBG/MapObjBlock.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <MoveBG/MapObjWave.hpp>
#include <MoveBG/ModelGate.hpp>
#include <MoveBG/MapObjMamma.hpp>
#include <MoveBG/MapObjMare.hpp>
#include <MoveBG/MapObjPinna.hpp>
#include <MoveBG/MapObjRicco.hpp>
#include <MoveBG/MapObjTree.hpp>
#include <MoveBG/MapObjPlane.hpp>
#include <MoveBG/MapObjFence.hpp>
#include <MoveBG/MapObjMonte.hpp>
#include <MoveBG/MapObjItem2.hpp>
#include <MoveBG/MapObjTown.hpp>
#include <MoveBG/MapObjSample.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/MapObjRailBlock.hpp>

// ============================================================
// Data symbols
// ============================================================
f32 TMapObjTree::mBananaTreeJumpPower = 0.0f;

// ============================================================
// TAmiKing
// ============================================================
TAmiKing::TAmiKing(const char* name) : TMapObjBase(name) {}

// ============================================================
// TBalloonKoopaJr
// ============================================================
TBalloonKoopaJr::TBalloonKoopaJr(const char* name) : TMapObjGeneral(name) {}

// ============================================================
// TBasketReverse
// ============================================================
TBasketReverse::TBasketReverse(const char* name) : TMapObjBase(name) {}

// ============================================================
// TBigWatermelon
// ============================================================
TBigWatermelon::TBigWatermelon(const char* name) : TMapObjBall(name) {}
void TBigWatermelon::startEvent() {}

// ============================================================
// TBrickBlock
// ============================================================
TBrickBlock::TBrickBlock(const char* name) : THideObjBase(name) {}

// ============================================================
// TCogwheel
// ============================================================
TCogwheel::TCogwheel(const char* name) : TMapObjBase(name) {}

// ============================================================
// TCogwheelScale
// ============================================================
TCogwheelScale::TCogwheelScale(const char* name) : TMapObjBase(name) {}

// ============================================================
// TCoverFruit
// ============================================================
TCoverFruit::TCoverFruit(const char* name) : TMapObjBase(name) {}

// ============================================================
// TCraneRotY
// ============================================================
TCraneRotY::TCraneRotY(const char* name) : TMapObjBase(name) {}

// ============================================================
// TCraneUpDown
// ============================================================
TCraneUpDown::TCraneUpDown(const char* name) : TMapObjBase(name) {}

// ============================================================
// TDoor
// ============================================================
TDoor::TDoor(const char* name) : TMapObjBase(name) {}

// ============================================================
// TFerrisWheel
// ============================================================
TFerrisWheel::TFerrisWheel(const char* name) : TMapObjBase(name) {}

// ============================================================
// TFlowerCoin
// ============================================================
TFlowerCoin::TFlowerCoin(const char* name) : TCoin(name) {}

// ============================================================
// TFluffManager
// ============================================================
TFluffManager::TFluffManager(const char* name) : TMapObjBase(name) {}

// ============================================================
// TFruitLauncher
// ============================================================
TFruitLauncher::TFruitLauncher(const char* name) : TMapObjBase(name) {}

// ============================================================
// TFruitSwitch
// ============================================================
TFruitSwitch::TFruitSwitch(const char* name) : TMapObjBase(name) {}

// ============================================================
// TGateShadow (vtable for TGateShadow)
// ============================================================
TGateShadow::TGateShadow(const char* name) : JDrama::TViewObj(name) {}
void TGateShadow::perform(u32, JDrama::TGraphics*) {}

// ============================================================
// TGoalFlag
// ============================================================
TGoalFlag::TGoalFlag(const char* name) : TMapObjBase(name) {}

// ============================================================
// TGoalWatermelon
// ============================================================
TGoalWatermelon::TGoalWatermelon(const char* name) : TMapObjBase(name) {}

// ============================================================
// THangingBridge
// ============================================================
THangingBridge::THangingBridge(const char* name) : JDrama::TViewObj(name) {}

// ============================================================
// THangingBridgeBoard
// ============================================================
THangingBridgeBoard::THangingBridgeBoard(const char* name) : TLeanBlock(name) {}

// ============================================================
// THideObjInfo
// ============================================================
THideObjInfo::THideObjInfo(const char* name) : JDrama::TViewObj(name) {}

// ============================================================
// TIceBlock
// ============================================================
TIceBlock::TIceBlock(const char* name) : TMapObjBase(name) {}

// ============================================================
// TJumpBase
// ============================================================
TJumpBase::TJumpBase(const char* name) : TMapObjBase(name) {}

// ============================================================
// TJumpMushroom
// ============================================================
TJumpMushroom::TJumpMushroom(const char* name) : TMapObjBase(name) {}

// ============================================================
// TLeanMirror
// ============================================================
TLeanMirror::TLeanMirror(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMammaBlockRotate
// ============================================================
TMammaBlockRotate::TMammaBlockRotate(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMammaMirrorMapOperator
// ============================================================
TMammaMirrorMapOperator::TMammaMirrorMapOperator(const char* name)
    : JDrama::TViewObj(name) {}

// ============================================================
// TMammaYacht
// ============================================================
TMammaYacht::TMammaYacht(const char* name) : TMapObjBase(name) {}

// ============================================================
// TManhole
// ============================================================
TManhole::TManhole(const char* name) : TMapObjGeneral(name) {}
void TManhole::makeManholeUnuseful(const TMapObjBase*) {}

// ============================================================
// TMapObjBall
// ============================================================
TMapObjBall::TMapObjBall(const char* name) : TMapObjGeneral(name) {}

// ============================================================
// TMapObjElasticCode
// ============================================================
TMapObjElasticCode::TMapObjElasticCode(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMapObjGrowTree
// ============================================================
TMapObjGrowTree::TMapObjGrowTree(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMapObjMonteRoot
// ============================================================
TMapObjMonteRoot::TMapObjMonteRoot(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMapObjPuncher
// ============================================================
TMapObjPuncher::TMapObjPuncher(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMapObjSteam
// ============================================================
TMapObjSteam::TMapObjSteam(const char* name) : THideObjBase(name) {}

// ============================================================
// TMapObjSwitch
// ============================================================
TMapObjSwitch::TMapObjSwitch(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMapObjTree / TMapObjTreeScale
// ============================================================
TMapObjTree::TMapObjTree(const char* name) : TMapObjGeneral(name) {}
TMapObjTreeScale::TMapObjTreeScale(const char* name) : TMapObjGeneral(name) {}

// ============================================================
// TMapObjWaterSpray
// ============================================================
TMapObjWaterSpray::TMapObjWaterSpray(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMapObjWave
// ============================================================
TMapObjWave::TMapObjWave(const char* name) : JDrama::TViewObj(name) {}
f32 TMapObjWave::getHeight(float, float, float) const { return 0.0f; }
f32 TMapObjWave::getWaveHeight(float, float) const { return 0.0f; }

// ============================================================
// TMareCork
// ============================================================
TMareCork::TMareCork(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMareEventPoint
// ============================================================
TMareEventPoint::TMareEventPoint(const char* name) : THitActor(name) {}

// ============================================================
// TMareFall
// ============================================================
TMareFall::TMareFall(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMerrygoround
// ============================================================
TMerrygoround::TMerrygoround(const char* name) : TMapObjBase(name) {}

// ============================================================
// TModelGate
// ============================================================
TModelGate::TModelGate(const char* name) : TTakeActor(name) {}
void TModelGate::startOpen() {}

// ============================================================
// TMuddyBoat
// ============================================================
TMuddyBoat::TMuddyBoat(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMushroom1up
// ============================================================
TMushroom1up::TMushroom1up(int, const char* name) : TMapObjBase(name) {}

// ============================================================
// TPinnaCoaster
// ============================================================
TPinnaCoaster::TPinnaCoaster(const char* name) : TMapObjBase(name) {}

// ============================================================
// TPinnaEntrance
// ============================================================
TPinnaEntrance::TPinnaEntrance(const char* name) : TMapObjBase(name) {}

// ============================================================
// TRandomFruit
// ============================================================
TRandomFruit::TRandomFruit(const char* name) : TResetFruit(name) {}

// ============================================================
// TRedCoinSwitch
// ============================================================
TRedCoinSwitch::TRedCoinSwitch(const char* name) : TMapObjBase(name) {}

// ============================================================
// TResetFruit
// ============================================================
TResetFruit::TResetFruit(const char* name) : TMapObjBall(name) {}
void TResetFruit::makeObjLiving() {}
void TResetFruit::makeObjWaitingToAppear() {}

// ============================================================
// TRiccoWatermill
// ============================================================
TRiccoWatermill::TRiccoWatermill(const char* name) : TMapObjBase(name) {}

// ============================================================
// TRockPlane
// ============================================================
TRockPlane::TRockPlane(const char* name) : TMapObjPlane(name) {}

// ============================================================
// TSandBird
// ============================================================
TSandBird::TSandBird(const char* name) : TJointCoin(name) {}

// ============================================================
// TSandBlock
// ============================================================
TSandBlock::TSandBlock(const char* name) : TMapObjBase(name) {}

// ============================================================
// TSandBombBase
// ============================================================
TSandBombBase::TSandBombBase(const char* name) : TSandBase(name) {}

// ============================================================
// TSandCastle
// ============================================================
TSandCastle::TSandCastle(const char* name) : TSandBombBase(name) {}

// ============================================================
// TSandEgg
// ============================================================
TSandEgg::TSandEgg(const char* name) : TMapObjBase(name) {}

// ============================================================
// TSandLeafBase
// ============================================================
TSandLeafBase::TSandLeafBase(const char* name) : TSandBase(name) {}

// ============================================================
// TSandPlane
// ============================================================
TSandPlane::TSandPlane(const char* name) : TMapObjPlane(name) {}

// ============================================================
// TShellCup
// ============================================================
TShellCup::TShellCup(const char* name) : TMapObjBase(name) {}

// ============================================================
// TShiningStone
// ============================================================
TShiningStone::TShiningStone(const char* name) : THitActor(name) {}

// ============================================================
// TSuperHipDropBlock
// ============================================================
TSuperHipDropBlock::TSuperHipDropBlock(const char* name) : TBreakHideObj(name) {}

// ============================================================
// TSurfGesoObj
// ============================================================
TSurfGesoObj::TSurfGesoObj(const char* name) : TItem(name) {}

// ============================================================
// TSwingBoard
// ============================================================
TSwingBoard::TSwingBoard(const char* name) : TMapObjBase(name) {}

// ============================================================
// TViking
// ============================================================
TViking::TViking(const char* name) : THorizontalViking(name) {}

// ============================================================
// TWaterRecoverObj
// ============================================================
TWaterRecoverObj::TWaterRecoverObj(const char* name) : TMapObjBase(name) {}

// ============================================================
// TWireBell
// ============================================================
TWireBell::TWireBell(const char* name) : TMapObjBase(name) {}

// ============================================================
// vtable-bearing classes: all declared virtual methods need bodies
// ============================================================

// --- TChangeStageMerrygoround (vtable for TChangeStageMerrygoround) ---
// Ctor is inline in header.
void TChangeStageMerrygoround::touchPlayer(THitActor*) {}
void TChangeStageMerrygoround::calc() {}

// --- TCraneCargo (vtable for TCraneCargo) ---
// Ctor is inline in header.
void TCraneCargo::control() {}
void TCraneCargo::calc() {}

// --- TDamageObj (vtable for TDamageObj) ---
// Ctor is inline in header.
void TDamageObj::perform(u32, JDrama::TGraphics*) {}
void TDamageObj::init(u32) {}
void TDamageObj::load(JSUMemoryInputStream& s) { (void)s; }

// --- TFence (vtable for TFence) ---
// Ctor is inline in header.
BOOL TFence::receiveMessage(THitActor*, u32) { return 0; }
void TFence::initMapCollisionData() {}
void TFence::initMapObj() {}

// --- TFenceWater (vtable for TFenceWater) ---
// Ctor is inline in header.
void TFenceWater::draw() const {}
BOOL TFenceWater::receiveMessage(THitActor*, u32) { return 0; }
void TFenceWater::changeStatusToGo() {}
void TFenceWater::changeStatusToWait() {}
void TFenceWater::controlRotation() {}
void TFenceWater::control() {}
void TFenceWater::initMapCollisionData() {}
void TFenceWater::initMapObj() {}

// --- TFenceWaterH (vtable for TFenceWaterH) ---
// Ctor is inline in header.
void TFenceWaterH::control() {}
void TFenceWaterH::changeStatusToGo() {}
void TFenceWaterH::changeStatusToWait() {}

// --- TMapObjBillboard (vtable for TMapObjBillboard) ---
// Ctor is inline in header.
void TMapObjBillboard::swing(THitActor*) {}
void TMapObjBillboard::touchActor(THitActor*) {}
u32 TMapObjBillboard::touchWater(THitActor*) { return 0; }

// --- TMapObjChangeStage (vtable for TMapObjChangeStage) ---
// Ctor is inline in header.
void TMapObjChangeStage::touchPlayer(THitActor*) {}
void TMapObjChangeStage::load(JSUMemoryInputStream& s) { (void)s; }

// --- TMapObjChangeStageHipDrop (vtable for TMapObjChangeStageHipDrop) ---
// Ctor is inline in header.
void TMapObjChangeStageHipDrop::touchPlayer(THitActor*) {}
void TMapObjChangeStageHipDrop::initMapObj() {}

// --- TMapObjStartDemo (vtable for TMapObjStartDemo) ---
// Ctor is inline in header.
void TMapObjStartDemo::touchPlayer(THitActor*) {}
void TMapObjStartDemo::load(JSUMemoryInputStream& s) { (void)s; }

// --- TRailFence (vtable for TRailFence) ---
// Ctor is inline in header.
BOOL TRailFence::receiveMessage(THitActor*, u32) { return 0; }
void TRailFence::falling() {}
void TRailFence::goOnRail() {}
void TRailFence::control() {}
void TRailFence::initMapCollisionData() {}
void TRailFence::load(JSUMemoryInputStream& s) { (void)s; }

// --- TRevolvingFenceInner (vtable for TRevolvingFenceInner) ---
// Ctor is inline in header.
BOOL TRevolvingFenceInner::receiveMessage(THitActor*, u32) { return 0; }
void TRevolvingFenceInner::calcCurrentMtx() {}
void TRevolvingFenceInner::controlWall() {}
void TRevolvingFenceInner::controlGroundRoof() {}
void TRevolvingFenceInner::setGroundCollision() {}
void TRevolvingFenceInner::control() {}
void TRevolvingFenceInner::initMapCollisionData() {}
void TRevolvingFenceInner::initMapObj() {}

// --- TRevolvingFenceOuter (vtable for TRevolvingFenceOuter) ---
// Ctor is inline in header.
BOOL TRevolvingFenceOuter::receiveMessage(THitActor*, u32) { return 0; }
void TRevolvingFenceOuter::initMapCollisionData() {}

// --- TSandBomb (vtable for TSandBomb) ---
// Ctor is inline in header.
void TSandBomb::makeObjAppeared() {}
u32 TSandBomb::touchWater(THitActor*) { return 0; }
u32 TSandBomb::getSDLModelFlag() const { return 0; }
void TSandBomb::initMapObj() {}

// --- TSandLeaf (vtable for TSandLeaf) ---
// Ctor is inline in header.
u32 TSandLeaf::touchWater(THitActor*) { return 0; }
void TSandLeaf::control() {}

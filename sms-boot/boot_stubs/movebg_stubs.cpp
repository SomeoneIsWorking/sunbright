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

#include "stub_trace.h"
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
void TBigWatermelon::startEvent() { SB_STUB_HIT("TBigWatermelon::startEvent"); }

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

// TDoor now provided by reference/sms/src/MoveBG/MapObjTown.cpp.

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
void TGateShadow::perform(u32, JDrama::TGraphics*) { SB_STUB_HIT("TGateShadow::perform"); }

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

// THideObjInfo now provided by reference/sms/src/MoveBG/MapObjTown.cpp.

// ============================================================
// TIceBlock
// ============================================================
TIceBlock::TIceBlock(const char* name) : TMapObjBase(name) {}

// ============================================================
// TJumpBase
// ============================================================
// [dedup] TJumpBase::TJumpBase(const char* name) : TMapObjBase(name) {} 
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
    : JDrama::TViewObj(name)
{
	// The 8 per-node arrays are populated by loadAfter (native port in MapObjMamma.cpp);
	// perform runs first-and-only-time in gpMSMirror.mCurrentIdx==-1 mode which forces all
	// visibility flags to 1 anyway, but zeroing them here means a perform-before-loadAfter
	// call (or a scene without a mirror-terrain resource) can't read uninitialized garbage.
	for (int i = 0; i < 8; ++i) {
		mNodes[i] = nullptr;
		mNodeCenter[i].setAll(0.0f);
		mNodeRadius[i] = 0.0f;
		mNodeVisible[i] = 0;
	}
	mMirrorSPos.setAll(0.0f);
	mMirrorMPos.setAll(0.0f);
	mMirrorLPos.setAll(0.0f);
}

// ============================================================
// TMammaYacht
// ============================================================
TMammaYacht::TMammaYacht(const char* name) : TMapObjBase(name) {}

// ============================================================
// TManhole
// ============================================================
// [dedup] TManhole::TManhole(const char* name) : TMapObjGeneral(name) {}
// [dedup] void TManhole::makeManholeUnuseful(const TMapObjBase*) {} 
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
// [dedup] TMapObjSwitch::TMapObjSwitch(const char* name) : TMapObjBase(name) {} 
// ============================================================
// TMapObjTree / TMapObjTreeScale
// ============================================================
TMapObjTree::TMapObjTree(const char* name) : TMapObjGeneral(name) {}
TMapObjTreeScale::TMapObjTreeScale(const char* name) : TMapObjGeneral(name) {}

// ============================================================
// TMapObjWaterSpray
// ============================================================
// [dedup] TMapObjWaterSpray::TMapObjWaterSpray(const char* name) : TMapObjBase(name) {} 
// ============================================================
// TMapObjWave
// ============================================================
// TMapObjWave ctor / load / perform / draw / initDraw are now ported faithfully in
// reference/sms/src/MoveBG/MapObjWave.cpp (no longer stubbed here).

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
void TModelGate::startOpen() { SB_STUB_HIT("TModelGate::startOpen"); }

// ============================================================
// TMuddyBoat
// ============================================================
TMuddyBoat::TMuddyBoat(const char* name) : TMapObjBase(name) {}

// ============================================================
// TMushroom1up
// ============================================================
// [dedup] TMushroom1up::TMushroom1up(int, const char* name) : TMapObjBase(name) {} 
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
// [dedup] TRedCoinSwitch::TRedCoinSwitch(const char* name) : TMapObjBase(name) {} 
// ============================================================
// TResetFruit
// ============================================================
// TResetFruit::TResetFruit now PORTED in MapObjBall.cpp (RE'd 2026-07-17).
void TResetFruit::makeObjLiving() { SB_STUB_HIT("TResetFruit::makeObjLiving"); }
void TResetFruit::makeObjWaitingToAppear() { SB_STUB_HIT("TResetFruit::makeObjWaitingToAppear"); }

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
TWireBell::TWireBell(const char* name) : TMapObjBase(name), mWireIndex(0) {}

// ============================================================
// vtable-bearing classes: all declared virtual methods need bodies
// ============================================================

// --- TChangeStageMerrygoround (vtable for TChangeStageMerrygoround) ---
// Ctor is inline in header.
void TChangeStageMerrygoround::touchPlayer(THitActor*) { SB_STUB_HIT("TChangeStageMerrygoround::touchPlayer"); }
void TChangeStageMerrygoround::calc() { SB_STUB_HIT("TChangeStageMerrygoround::calc"); }

// --- TCraneCargo (vtable for TCraneCargo) ---
// Ctor is inline in header.
void TCraneCargo::calc() { SB_STUB_HIT("TCraneCargo::calc"); }

// --- TDamageObj (vtable for TDamageObj) ---
// Ctor is inline in header.
// perform lives natively in reference/sms/src/MoveBG/MapObjTown.cpp.
// [dedup] void TDamageObj::init(u32) {}
// [dedup] void TDamageObj::load(JSUMemoryInputStream& s) { (void)s; } 
// --- TFence (vtable for TFence) ---
// Ctor is inline in header.
BOOL TFence::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TFence::receiveMessage"); return 0; }
void TFence::initMapCollisionData() { SB_STUB_HIT("TFence::initMapCollisionData"); }
void TFence::initMapObj() { SB_STUB_HIT("TFence::initMapObj"); }

// --- TFenceWater (vtable for TFenceWater) ---
// Ctor is inline in header.
void TFenceWater::draw() const { SB_STUB_HIT("TFenceWater::draw"); }
BOOL TFenceWater::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TFenceWater::receiveMessage"); return 0; }
void TFenceWater::changeStatusToGo() { SB_STUB_HIT("TFenceWater::changeStatusToGo"); }
void TFenceWater::changeStatusToWait() { SB_STUB_HIT("TFenceWater::changeStatusToWait"); }
void TFenceWater::controlRotation() { SB_STUB_HIT("TFenceWater::controlRotation"); }
void TFenceWater::control() { SB_STUB_HIT("TFenceWater::control"); }
void TFenceWater::initMapCollisionData() { SB_STUB_HIT("TFenceWater::initMapCollisionData"); }
void TFenceWater::initMapObj() { SB_STUB_HIT("TFenceWater::initMapObj"); }

// --- TFenceWaterH (vtable for TFenceWaterH) ---
// Ctor is inline in header.
void TFenceWaterH::control() { SB_STUB_HIT("TFenceWaterH::control"); }
void TFenceWaterH::changeStatusToGo() { SB_STUB_HIT("TFenceWaterH::changeStatusToGo"); }
void TFenceWaterH::changeStatusToWait() { SB_STUB_HIT("TFenceWaterH::changeStatusToWait"); }

// --- TMapObjBillboard (vtable for TMapObjBillboard) ---
// Ctor is inline in header.
// [dedup] void TMapObjBillboard::swing(THitActor*) {}
// [dedup] void TMapObjBillboard::touchActor(THitActor*) {}
// [dedup] u32 TMapObjBillboard::touchWater(THitActor*) { return 0; } 
// --- TMapObjChangeStage (vtable for TMapObjChangeStage) ---
// Ctor is inline in header.
// [dedup] void TMapObjChangeStage::touchPlayer(THitActor*) {}
// [dedup] void TMapObjChangeStage::load(JSUMemoryInputStream& s) { (void)s; } 
// --- TMapObjChangeStageHipDrop (vtable for TMapObjChangeStageHipDrop) ---
// Ctor is inline in header.
// [dedup] void TMapObjChangeStageHipDrop::touchPlayer(THitActor*) {}
// [dedup] void TMapObjChangeStageHipDrop::initMapObj() {} 
// --- TMapObjStartDemo (vtable for TMapObjStartDemo) ---
// Ctor is inline in header.
// [dedup] void TMapObjStartDemo::touchPlayer(THitActor*) {}
// [dedup] void TMapObjStartDemo::load(JSUMemoryInputStream& s) { (void)s; } 
// --- TRailFence (vtable for TRailFence) ---
// Ctor is inline in header.
BOOL TRailFence::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TRailFence::receiveMessage"); return 0; }
void TRailFence::falling() { SB_STUB_HIT("TRailFence::falling"); }
void TRailFence::goOnRail() { SB_STUB_HIT("TRailFence::goOnRail"); }
void TRailFence::control() { SB_STUB_HIT("TRailFence::control"); }
void TRailFence::initMapCollisionData() { SB_STUB_HIT("TRailFence::initMapCollisionData"); }
void TRailFence::load(JSUMemoryInputStream& s) { (void)s; }

// --- TRevolvingFenceInner (vtable for TRevolvingFenceInner) ---
// Ctor is inline in header.
BOOL TRevolvingFenceInner::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TRevolvingFenceInner::receiveMessage"); return 0; }
void TRevolvingFenceInner::calcCurrentMtx() { SB_STUB_HIT("TRevolvingFenceInner::calcCurrentMtx"); }
void TRevolvingFenceInner::controlWall() { SB_STUB_HIT("TRevolvingFenceInner::controlWall"); }
void TRevolvingFenceInner::controlGroundRoof() { SB_STUB_HIT("TRevolvingFenceInner::controlGroundRoof"); }
void TRevolvingFenceInner::setGroundCollision() { SB_STUB_HIT("TRevolvingFenceInner::setGroundCollision"); }
void TRevolvingFenceInner::control() { SB_STUB_HIT("TRevolvingFenceInner::control"); }
void TRevolvingFenceInner::initMapCollisionData() { SB_STUB_HIT("TRevolvingFenceInner::initMapCollisionData"); }
void TRevolvingFenceInner::initMapObj() { SB_STUB_HIT("TRevolvingFenceInner::initMapObj"); }

// --- TRevolvingFenceOuter (vtable for TRevolvingFenceOuter) ---
// Ctor is inline in header.
BOOL TRevolvingFenceOuter::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TRevolvingFenceOuter::receiveMessage"); return 0; }
void TRevolvingFenceOuter::initMapCollisionData() { SB_STUB_HIT("TRevolvingFenceOuter::initMapCollisionData"); }

// --- TSandBomb (vtable for TSandBomb) ---
// Ctor is inline in header.
void TSandBomb::makeObjAppeared() { SB_STUB_HIT("TSandBomb::makeObjAppeared"); }
u32 TSandBomb::touchWater(THitActor*) { SB_STUB_HIT("TSandBomb::touchWater"); return 0; }
u32 TSandBomb::getSDLModelFlag() const { SB_STUB_HIT("TSandBomb::getSDLModelFlag"); return 0; }
void TSandBomb::initMapObj() { SB_STUB_HIT("TSandBomb::initMapObj"); }

// --- TSandLeaf (vtable for TSandLeaf) ---
// Ctor is inline in header.
u32 TSandLeaf::touchWater(THitActor*) { SB_STUB_HIT("TSandLeaf::touchWater"); return 0; }

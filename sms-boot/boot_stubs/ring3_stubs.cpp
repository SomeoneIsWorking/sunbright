// ring3_stubs.cpp — RING-3 SCAFFOLD: remaining vtable-slot virtuals (args verbatim
// from the linker undef => exact match; return types from headers; minimal bodies).

#include "stub_trace.h"
#include <dolphin/os.h>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>
#include <Animal/AnimalBase.hpp>
#include <Enemy/AreaCylinder.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Generator.hpp>
#include <GC2D/Guide.hpp>
#include <GC2D/PauseMenu2.hpp>
#include <Map/BathWaterManager.hpp>
#include <MarioUtil/ShadowUtil.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <MoveBG/MapObjItem2.hpp>
#include <MoveBG/MapObjMamma.hpp>
#include <MoveBG/MapObjMare.hpp>
#include <MoveBG/MapObjMonte.hpp>
#include <MoveBG/MapObjPinna.hpp>
#include <MoveBG/MapObjRicco.hpp>
#include <MoveBG/MapObjTown.hpp>
#include <MoveBG/MapObjTree.hpp>
#include <MoveBG/MapObjWave.hpp>
#include <MoveBG/ModelGate.hpp>

void TAmiKing::bind() { SB_STUB_HIT("TAmiKing::bind"); }
void TAmiKing::calcRootMatrix() { SB_STUB_HIT("TAmiKing::calcRootMatrix"); }
void TAmiKing::initMapObj() { SB_STUB_HIT("TAmiKing::initMapObj"); }
void TAmiKing::moveObject() { SB_STUB_HIT("TAmiKing::moveObject"); }
void TAmiKing::touchPlayer(THitActor*) { SB_STUB_HIT("TAmiKing::touchPlayer"); }
void TAnimalBase::calcRootMatrix() { SB_STUB_HIT("TAnimalBase::calcRootMatrix"); }
void TAnimalBase::init(TLiveManager*) { SB_STUB_HIT("TAnimalBase::init"); }
void TAnimalBase::loadAfter() { SB_STUB_HIT("TAnimalBase::loadAfter"); }
void TAnimalBase::perform(unsigned int, JDrama::TGraphics*) { SB_STUB_HIT("TAnimalBase::perform"); }
BOOL TAnimalBase::receiveMessage(THitActor*, unsigned int) { SB_STUB_HIT("TAnimalBase::receiveMessage"); return 0; }
void TAreaCylinder::perform(unsigned int, JDrama::TGraphics*) { SB_STUB_HIT("TAreaCylinder::perform"); }
void TBalloonKoopaJr::kill() { SB_STUB_HIT("TBalloonKoopaJr::kill"); }
void TBalloonKoopaJr::load(JSUMemoryInputStream&) { SB_STUB_HIT("TBalloonKoopaJr::load"); }
// [dedup] void TBasketReverse::initMapObj() {}
void TBathWaterManager::loadAfter() { SB_STUB_HIT("TBathWaterManager::loadAfter"); }
void TBathWaterManager::perform(unsigned int, JDrama::TGraphics*) { SB_STUB_HIT("TBathWaterManager::perform"); }
void TBigWatermelon::appearing() { SB_STUB_HIT("TBigWatermelon::appearing"); }
void TBigWatermelon::checkWallCollision(JGeometry::TVec3<float>*) { SB_STUB_HIT("TBigWatermelon::checkWallCollision"); }
void TBigWatermelon::initMapObj() { SB_STUB_HIT("TBigWatermelon::initMapObj"); }
void TBigWatermelon::kill() { SB_STUB_HIT("TBigWatermelon::kill"); }
void TBigWatermelon::rebound(JGeometry::TVec3<float>*) { SB_STUB_HIT("TBigWatermelon::rebound"); }
BOOL TBigWatermelon::receiveMessage(THitActor*, unsigned int) { SB_STUB_HIT("TBigWatermelon::receiveMessage"); return 0; }
void TBigWatermelon::touchActor(THitActor*) { SB_STUB_HIT("TBigWatermelon::touchActor"); }
void TBigWatermelon::touchGround(JGeometry::TVec3<float>*) { SB_STUB_HIT("TBigWatermelon::touchGround"); }
void TBigWatermelon::touchWall(JGeometry::TVec3<float>*, TBGWallCheckRecord*) { SB_STUB_HIT("TBigWatermelon::touchWall"); }
void TBigWatermelon::touchWaterSurface() { SB_STUB_HIT("TBigWatermelon::touchWaterSurface"); }
void TCogwheel::calc() { SB_STUB_HIT("TCogwheel::calc"); }
void TCogwheel::control() { SB_STUB_HIT("TCogwheel::control"); }
void TCogwheel::initMapObj() { SB_STUB_HIT("TCogwheel::initMapObj"); }
void TCogwheelScale::control() { SB_STUB_HIT("TCogwheelScale::control"); }
BOOL TCogwheelScale::receiveMessage(THitActor*, unsigned int) { SB_STUB_HIT("TCogwheelScale::receiveMessage"); return 0; }
void TCogwheelScale::touchPlayer(THitActor*) { SB_STUB_HIT("TCogwheelScale::touchPlayer"); }
void TCoverFruit::calcRootMatrix() { SB_STUB_HIT("TCoverFruit::calcRootMatrix"); }
BOOL TCoverFruit::receiveMessage(THitActor*, unsigned int) { SB_STUB_HIT("TCoverFruit::receiveMessage"); return 0; }
void TCraneRotY::control() { SB_STUB_HIT("TCraneRotY::control"); }
void TCraneRotY::load(JSUMemoryInputStream&) { SB_STUB_HIT("TCraneRotY::load"); }
void TCraneUpDown::initMapObj() { SB_STUB_HIT("TCraneUpDown::initMapObj"); }
void TFerrisWheel::initMapObj() { SB_STUB_HIT("TFerrisWheel::initMapObj"); }
void TFluffManager::loadAfter() { SB_STUB_HIT("TFluffManager::loadAfter"); }
void TFluffManager::load(JSUMemoryInputStream&) { SB_STUB_HIT("TFluffManager::load"); }
void TGenerator::perform(unsigned int, JDrama::TGraphics*) { SB_STUB_HIT("TGenerator::perform"); }
void TGoalFlag::initMapObj() { SB_STUB_HIT("TGoalFlag::initMapObj"); }
void TGoalFlag::touchActor(THitActor*) { SB_STUB_HIT("TGoalFlag::touchActor"); }
void TGoalWatermelon::control() { SB_STUB_HIT("TGoalWatermelon::control"); }
void TGoalWatermelon::loadAfter() { SB_STUB_HIT("TGoalWatermelon::loadAfter"); }
void TGoalWatermelon::load(JSUMemoryInputStream&) { SB_STUB_HIT("TGoalWatermelon::load"); }
void THangingBridgeBoard::calcDefaultMtx() { SB_STUB_HIT("THangingBridgeBoard::calcDefaultMtx"); }
void THangingBridgeBoard::initMapObj() { SB_STUB_HIT("THangingBridgeBoard::initMapObj"); }
void THangingBridgeBoard::setGroundCollision() { SB_STUB_HIT("THangingBridgeBoard::setGroundCollision"); }
void THangingBridge::loadAfter() { SB_STUB_HIT("THangingBridge::loadAfter"); }
// [dedup] void THideObjInfo::load(JSUMemoryInputStream&) {}
void THorizontalViking::initMapObj() { SB_STUB_HIT("THorizontalViking::initMapObj"); }
// [dedup] void TJumpBase::calcRootMatrix() {}
// [dedup] void TJumpBase::control() {}
// [dedup] void TJumpBase::ensureTakeSituation() {}
// [dedup] Mtx* TJumpBase::getRootJointMtx() const { return nullptr; }
// [dedup] void TJumpBase::initMapObj() {}
void TJumpMushroom::load(JSUMemoryInputStream&) { SB_STUB_HIT("TJumpMushroom::load"); }
void TLeanMirror::control() { SB_STUB_HIT("TLeanMirror::control"); }
u32 TLeanMirror::getSDLModelFlag() const { SB_STUB_HIT("TLeanMirror::getSDLModelFlag"); return 0; }
void TLeanMirror::initMapObj() { SB_STUB_HIT("TLeanMirror::initMapObj"); }
void TLeanMirror::loadAfter() { SB_STUB_HIT("TLeanMirror::loadAfter"); }
void TLeanMirror::load(JSUMemoryInputStream&) { SB_STUB_HIT("TLeanMirror::load"); }
BOOL TLeanMirror::receiveMessage(THitActor*, unsigned int) { SB_STUB_HIT("TLeanMirror::receiveMessage"); return 0; }
void TLeanMirror::touchEnemy(THitActor*) { SB_STUB_HIT("TLeanMirror::touchEnemy"); }
void TLeanMirror::touchPlayer(THitActor*) { SB_STUB_HIT("TLeanMirror::touchPlayer"); }
void TMammaBlockRotate::control() { SB_STUB_HIT("TMammaBlockRotate::control"); }
void TMammaBlockRotate::initMapObj() { SB_STUB_HIT("TMammaBlockRotate::initMapObj"); }
void TMammaBlockRotate::load(JSUMemoryInputStream&) { SB_STUB_HIT("TMammaBlockRotate::load"); }
void TMammaYacht::initMapObj() { SB_STUB_HIT("TMammaYacht::initMapObj"); }
// [dedup] void TManhole::appeared() {}
// [dedup] void TManhole::calc() {}
// [dedup] void TManhole::setGroundCollision() {}
void TMapObjBall::calcCurrentMtx() { SB_STUB_HIT("TMapObjBall::calcCurrentMtx"); }
void TMapObjBall::checkWallCollision(JGeometry::TVec3<float>*) { SB_STUB_HIT("TMapObjBall::checkWallCollision"); }
void TMapObjBall::control() { SB_STUB_HIT("TMapObjBall::control"); }
void TMapObjBall::hold(TTakeActor*) { SB_STUB_HIT("TMapObjBall::hold"); }
void TMapObjBall::initMapObj() { SB_STUB_HIT("TMapObjBall::initMapObj"); }
void TMapObjBall::kicked() { SB_STUB_HIT("TMapObjBall::kicked"); }
void TMapObjBall::makeObjAppeared() { SB_STUB_HIT("TMapObjBall::makeObjAppeared"); }
void TMapObjBall::makeObjDefault() { SB_STUB_HIT("TMapObjBall::makeObjDefault"); }
void TMapObjBall::put() { SB_STUB_HIT("TMapObjBall::put"); }
void TMapObjBall::rebound(JGeometry::TVec3<float>*) { SB_STUB_HIT("TMapObjBall::rebound"); }
void TMapObjBall::touchActor(THitActor*) { SB_STUB_HIT("TMapObjBall::touchActor"); }
void TMapObjBall::touchGround(JGeometry::TVec3<float>*) { SB_STUB_HIT("TMapObjBall::touchGround"); }
void TMapObjBall::touchPollution() { SB_STUB_HIT("TMapObjBall::touchPollution"); }
void TMapObjBall::touchRoof(JGeometry::TVec3<float>*) { SB_STUB_HIT("TMapObjBall::touchRoof"); }
void TMapObjBall::touchWall(JGeometry::TVec3<float>*, TBGWallCheckRecord*) { SB_STUB_HIT("TMapObjBall::touchWall"); }
void TMapObjBall::touchWaterSurface() { SB_STUB_HIT("TMapObjBall::touchWaterSurface"); }
u32 TMapObjBall::touchWater(THitActor*) { SB_STUB_HIT("TMapObjBall::touchWater"); return 0; }
void TMapObjElasticCode::control() { SB_STUB_HIT("TMapObjElasticCode::control"); }
void TMapObjElasticCode::initMapObj() { SB_STUB_HIT("TMapObjElasticCode::initMapObj"); }
void TMapObjGrowTree::control() { SB_STUB_HIT("TMapObjGrowTree::control"); }
void TMapObjGrowTree::initMapObj() { SB_STUB_HIT("TMapObjGrowTree::initMapObj"); }
void TMapObjPuncher::control() { SB_STUB_HIT("TMapObjPuncher::control"); }
void TMapObjPuncher::load(JSUMemoryInputStream&) { SB_STUB_HIT("TMapObjPuncher::load"); }
// [dedup] void TMapObjSwitch::load(JSUMemoryInputStream&) {}
// [dedup] BOOL TMapObjSwitch::receiveMessage(THitActor*, unsigned int) { return 0; }
f32 TMapObjTree::getRadiusAtY(float) const { SB_STUB_HIT("TMapObjTree::getRadiusAtY"); return 0; }
// [ported] TMapObjTree::initMapObj + initEach + TMapObjLeaf ctor now live in
// reference/sms/src/MoveBG/MapObjTree.cpp (cold RE of US 0x801f68b4/0x801f6a64/
// 0x801f6ef4, 2026-07-15).
void TMapObjTreeScale::control() { SB_STUB_HIT("TMapObjTreeScale::control"); }
u32 TMapObjTreeScale::touchWater(THitActor*) { SB_STUB_HIT("TMapObjTreeScale::touchWater"); return 0; }
void TMapObjTree::touchPlayer(THitActor*) { SB_STUB_HIT("TMapObjTree::touchPlayer"); }
// [dedup] void TMapObjWaterSpray::load(JSUMemoryInputStream&) {}
void TMareCork::calcRootMatrix() { SB_STUB_HIT("TMareCork::calcRootMatrix"); }
void TMareCork::drawObject(JDrama::TGraphics*) { SB_STUB_HIT("TMareCork::drawObject"); }
MtxPtr TMareCork::getTakingMtx() { SB_STUB_HIT("TMareCork::getTakingMtx"); return {}; }
void TMareCork::moveObject() { SB_STUB_HIT("TMareCork::moveObject"); }
void TMareFall::load(JSUMemoryInputStream&) { SB_STUB_HIT("TMareFall::load"); }
void TMerrygoround::draw() const { SB_STUB_HIT("TMerrygoround::draw"); }
void TMerrygoround::initMapObj() { SB_STUB_HIT("TMerrygoround::initMapObj"); }
void TModelGate::loadAfter() { SB_STUB_HIT("TModelGate::loadAfter"); }
void TModelGate::perform(unsigned int, JDrama::TGraphics*) { SB_STUB_HIT("TModelGate::perform"); }
BOOL TModelGate::receiveMessage(THitActor*, unsigned int) { SB_STUB_HIT("TModelGate::receiveMessage"); return 0; }
void TMuddyBoat::bind() { SB_STUB_HIT("TMuddyBoat::bind"); }
void TMuddyBoat::calc() { SB_STUB_HIT("TMuddyBoat::calc"); }
void TMuddyBoat::control() { SB_STUB_HIT("TMuddyBoat::control"); }
u32 TMuddyBoat::getSDLModelFlag() const { SB_STUB_HIT("TMuddyBoat::getSDLModelFlag"); return 0; }
void TMuddyBoat::initMapObj() { SB_STUB_HIT("TMuddyBoat::initMapObj"); }
void TMuddyBoat::kill() { SB_STUB_HIT("TMuddyBoat::kill"); }
// [dedup] void TMushroom1up::control() {}
// [dedup] void TMushroom1up::initMapObj() {}
// [dedup] void TMushroom1up::makeObjAppeared() {}
// [dedup] void TMushroom1up::perform(unsigned int, JDrama::TGraphics*) {}
// [dedup] void TMushroom1up::touchPlayer(THitActor*) {}
void TOneShotGenerator::loadAfter() { SB_STUB_HIT("TOneShotGenerator::loadAfter"); }
BOOL TOneShotGenerator::receiveMessage(THitActor*, unsigned int) { SB_STUB_HIT("TOneShotGenerator::receiveMessage"); return 0; }
void TPinnaCoaster::initMapObj() { SB_STUB_HIT("TPinnaCoaster::initMapObj"); }
// [dedup] void TRedCoinSwitch::control() {}
// [dedup] void TRedCoinSwitch::loadAfter() {}
void TResetFruit::appearing() { SB_STUB_HIT("TResetFruit::appearing"); }
void TResetFruit::breaking() { SB_STUB_HIT("TResetFruit::breaking"); }
void TResetFruit::checkGroundCollision(JGeometry::TVec3<float>*) { SB_STUB_HIT("TResetFruit::checkGroundCollision"); }
void TResetFruit::control() { SB_STUB_HIT("TResetFruit::control"); }
u32 TResetFruit::getLivingTime() const { SB_STUB_HIT("TResetFruit::getLivingTime"); return 0; }
void TResetFruit::hold(TTakeActor*) { SB_STUB_HIT("TResetFruit::hold"); }
void TResetFruit::initMapObj() { SB_STUB_HIT("TResetFruit::initMapObj"); }
void TResetFruit::kicked() { SB_STUB_HIT("TResetFruit::kicked"); }
void TResetFruit::makeObjAppeared() { SB_STUB_HIT("TResetFruit::makeObjAppeared"); }
BOOL TResetFruit::receiveMessage(THitActor*, unsigned int) { SB_STUB_HIT("TResetFruit::receiveMessage"); return 0; }
void TResetFruit::thrown() { SB_STUB_HIT("TResetFruit::thrown"); }
void TResetFruit::touchActor(THitActor*) { SB_STUB_HIT("TResetFruit::touchActor"); }
void TResetFruit::touchGround(JGeometry::TVec3<float>*) { SB_STUB_HIT("TResetFruit::touchGround"); }
void TResetFruit::touchPollution() { SB_STUB_HIT("TResetFruit::touchPollution"); }
void TResetFruit::touchWaterSurface() { SB_STUB_HIT("TResetFruit::touchWaterSurface"); }
u32 TResetFruit::touchWater(THitActor*) { SB_STUB_HIT("TResetFruit::touchWater"); return 0; }
void TResetFruit::waitingToAppear() { SB_STUB_HIT("TResetFruit::waitingToAppear"); }
void TRiccoWatermill::calc() { SB_STUB_HIT("TRiccoWatermill::calc"); }
void TRiccoWatermill::control() { SB_STUB_HIT("TRiccoWatermill::control"); }
void TRiccoWatermill::loadAfter() { SB_STUB_HIT("TRiccoWatermill::loadAfter"); }
void TSandBird::initMapObj() { SB_STUB_HIT("TSandBird::initMapObj"); }
TMapObjBase* TSandBird::makeObjFromJointName(char const*, unsigned short) { SB_STUB_HIT("TSandBird::makeObjFromJointName"); return nullptr; }
bool TSandBird::nameIsObj(char const*) { SB_STUB_HIT("TSandBird::nameIsObj"); return 0; }
void TSandBombBase::initMapObj() { SB_STUB_HIT("TSandBombBase::initMapObj"); }
void TSandBombBase::loadAfter() { SB_STUB_HIT("TSandBombBase::loadAfter"); }
void TSandCastle::initMapObj() { SB_STUB_HIT("TSandCastle::initMapObj"); }
void TSandCastle::loadAfter() { SB_STUB_HIT("TSandCastle::loadAfter"); }
void TSandLeafBase::initMapObj() { SB_STUB_HIT("TSandLeafBase::initMapObj"); }
void TShellCup::initMapObj() { SB_STUB_HIT("TShellCup::initMapObj"); }
void TShellCup::loadAfter() { SB_STUB_HIT("TShellCup::loadAfter"); }
void TShellCup::perform(unsigned int, JDrama::TGraphics*) { SB_STUB_HIT("TShellCup::perform"); }
void TSwingBoard::control() { SB_STUB_HIT("TSwingBoard::control"); }
void TSwingBoard::load(JSUMemoryInputStream&) { SB_STUB_HIT("TSwingBoard::load"); }
void TViking::initMapObj() { SB_STUB_HIT("TViking::initMapObj"); }
void TWireBell::control() { SB_STUB_HIT("TWireBell::control"); }
// (emit vtable for TGuide)
//
// The real TGuide::load (Guide.cpp, undecompiled) FIRST mounts the "guide" 2D archive
// — guide.arc, streamed into ARAM as gArBkGuide by TApplication::setupThreadFuncLogo —
// into the director's shared 2D-archive object (unkD8) as the JKRFileLoader volume
// "guide", THEN builds its UI panes from it. TGuide loads in the scene-common nameref
// tree immediately BEFORE TConsoleStr, whose load() does SMSSwitch2DArchive("guide",
// gArBkConsole): getVolume("guide") -> unmount -> remount with the console (game_6)
// data. So the "guide" volume MUST exist before that switch. We port the mount (the
// 2D/ARAM archive subsystem step the whole stage-15/file-select load was blocked on);
// TGuide's own pane build + perform() remain unported stubs (it draws nothing yet),
// which is fine — the console graphics come from game_6 via TConsoleStr.
void TGuide::load(JSUMemoryInputStream& stream)
{
	JDrama::TViewObj::load(stream); // read NameRef header so search("ガイド画面") finds it
	SMSMountAramArchive(gpMarDirector->unkD8, gArBkGuide);
}
// TMBindShadowManager::load lives natively in reference/sms/src/MarioUtil/ShadowUtil.cpp
// TPauseMenu2::load now provided natively in reference/sms/src/GC2D/PauseMenu2.cpp.

// ring-4: last vtable-slot virtuals for the 3 misc classes (perform/loadAfter).
void TGuide::perform(unsigned int, JDrama::TGraphics*) { SB_STUB_HIT("TGuide::perform"); }
// TMBindShadowManager::perform lives natively in reference/sms/src/MarioUtil/ShadowUtil.cpp
// [dedup] void TPauseMenu2::loadAfter() {}
// [dedup] void TPauseMenu2::perform(unsigned int, JDrama::TGraphics*) {} 
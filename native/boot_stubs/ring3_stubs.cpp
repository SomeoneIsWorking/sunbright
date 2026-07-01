// ring3_stubs.cpp — RING-3 SCAFFOLD: remaining vtable-slot virtuals (args verbatim
// from the linker undef => exact match; return types from headers; minimal bodies).

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

void TAmiKing::bind() {}
void TAmiKing::calcRootMatrix() {}
void TAmiKing::initMapObj() {}
void TAmiKing::loadAfter() {}
void TAmiKing::moveObject() {}
void TAmiKing::touchPlayer(THitActor*) {}
void TAnimalBase::calcRootMatrix() {}
void TAnimalBase::init(TLiveManager*) {}
void TAnimalBase::loadAfter() {}
void TAnimalBase::perform(unsigned int, JDrama::TGraphics*) {}
BOOL TAnimalBase::receiveMessage(THitActor*, unsigned int) { return 0; }
void TAreaCylinder::perform(unsigned int, JDrama::TGraphics*) {}
void TBalloonKoopaJr::kill() {}
void TBalloonKoopaJr::load(JSUMemoryInputStream&) {}
void TBasketReverse::initMapObj() {}
void TBathWaterManager::loadAfter() {}
void TBathWaterManager::perform(unsigned int, JDrama::TGraphics*) {}
void TBigWatermelon::appearing() {}
void TBigWatermelon::checkWallCollision(JGeometry::TVec3<float>*) {}
void TBigWatermelon::initMapObj() {}
void TBigWatermelon::kill() {}
void TBigWatermelon::rebound(JGeometry::TVec3<float>*) {}
BOOL TBigWatermelon::receiveMessage(THitActor*, unsigned int) { return 0; }
void TBigWatermelon::touchActor(THitActor*) {}
void TBigWatermelon::touchGround(JGeometry::TVec3<float>*) {}
void TBigWatermelon::touchWall(JGeometry::TVec3<float>*, TBGWallCheckRecord*) {}
void TBigWatermelon::touchWaterSurface() {}
void TCogwheel::calc() {}
void TCogwheel::control() {}
void TCogwheel::initMapObj() {}
void TCogwheelScale::control() {}
BOOL TCogwheelScale::receiveMessage(THitActor*, unsigned int) { return 0; }
void TCogwheelScale::touchPlayer(THitActor*) {}
void TCoverFruit::calcRootMatrix() {}
BOOL TCoverFruit::receiveMessage(THitActor*, unsigned int) { return 0; }
void TCraneRotY::control() {}
void TCraneRotY::load(JSUMemoryInputStream&) {}
void TCraneUpDown::initMapObj() {}
void TDoor::load(JSUMemoryInputStream&) {}
void TFerrisWheel::initMapObj() {}
void TFluffManager::loadAfter() {}
void TFluffManager::load(JSUMemoryInputStream&) {}
void TGenerator::perform(unsigned int, JDrama::TGraphics*) {}
void TGoalFlag::initMapObj() {}
void TGoalFlag::touchActor(THitActor*) {}
void TGoalWatermelon::control() {}
void TGoalWatermelon::loadAfter() {}
void TGoalWatermelon::load(JSUMemoryInputStream&) {}
void THangingBridgeBoard::calcDefaultMtx() {}
void THangingBridgeBoard::initMapObj() {}
void THangingBridgeBoard::setGroundCollision() {}
void THangingBridge::loadAfter() {}
void THideObjInfo::load(JSUMemoryInputStream&) {}
void THorizontalViking::initMapObj() {}
void TJumpBase::calcRootMatrix() {}
void TJumpBase::control() {}
void TJumpBase::ensureTakeSituation() {}
Mtx* TJumpBase::getRootJointMtx() const { return nullptr; }
void TJumpBase::initMapObj() {}
void TJumpMushroom::load(JSUMemoryInputStream&) {}
void TLeanMirror::control() {}
u32 TLeanMirror::getSDLModelFlag() const { return 0; }
void TLeanMirror::initMapObj() {}
void TLeanMirror::loadAfter() {}
void TLeanMirror::load(JSUMemoryInputStream&) {}
BOOL TLeanMirror::receiveMessage(THitActor*, unsigned int) { return 0; }
void TLeanMirror::touchEnemy(THitActor*) {}
void TLeanMirror::touchPlayer(THitActor*) {}
void TMammaBlockRotate::control() {}
void TMammaBlockRotate::initMapObj() {}
void TMammaBlockRotate::load(JSUMemoryInputStream&) {}
void TMammaMirrorMapOperator::loadAfter() {}
void TMammaYacht::initMapObj() {}
void TManhole::appeared() {}
void TManhole::calc() {}
void TManhole::initMapObj() {}
void TManhole::loadAfter() {}
void TManhole::setGroundCollision() {}
void TMapObjBall::calcCurrentMtx() {}
void TMapObjBall::checkWallCollision(JGeometry::TVec3<float>*) {}
void TMapObjBall::control() {}
void TMapObjBall::hold(TTakeActor*) {}
void TMapObjBall::initMapObj() {}
void TMapObjBall::kicked() {}
void TMapObjBall::makeObjAppeared() {}
void TMapObjBall::makeObjDefault() {}
void TMapObjBall::put() {}
void TMapObjBall::rebound(JGeometry::TVec3<float>*) {}
void TMapObjBall::touchActor(THitActor*) {}
void TMapObjBall::touchGround(JGeometry::TVec3<float>*) {}
void TMapObjBall::touchPollution() {}
void TMapObjBall::touchRoof(JGeometry::TVec3<float>*) {}
void TMapObjBall::touchWall(JGeometry::TVec3<float>*, TBGWallCheckRecord*) {}
void TMapObjBall::touchWaterSurface() {}
u32 TMapObjBall::touchWater(THitActor*) { return 0; }
void TMapObjElasticCode::control() {}
void TMapObjElasticCode::initMapObj() {}
void TMapObjGrowTree::control() {}
void TMapObjGrowTree::initMapObj() {}
void TMapObjGrowTree::loadAfter() {}
void TMapObjPuncher::control() {}
void TMapObjPuncher::load(JSUMemoryInputStream&) {}
void TMapObjSwitch::load(JSUMemoryInputStream&) {}
BOOL TMapObjSwitch::receiveMessage(THitActor*, unsigned int) { return 0; }
f32 TMapObjTree::getRadiusAtY(float) const { return 0; }
void TMapObjTree::initMapObj() {}
void TMapObjTreeScale::control() {}
u32 TMapObjTreeScale::touchWater(THitActor*) { return 0; }
void TMapObjTree::touchPlayer(THitActor*) {}
void TMapObjWaterSpray::load(JSUMemoryInputStream&) {}
void TMareCork::calcRootMatrix() {}
void TMareCork::drawObject(JDrama::TGraphics*) {}
MtxPtr TMareCork::getTakingMtx() { return {}; }
void TMareCork::moveObject() {}
void TMareEventPoint::load(JSUMemoryInputStream&) {}
void TMareFall::load(JSUMemoryInputStream&) {}
void TMerrygoround::draw() const {}
void TMerrygoround::initMapObj() {}
void TModelGate::loadAfter() {}
void TModelGate::perform(unsigned int, JDrama::TGraphics*) {}
BOOL TModelGate::receiveMessage(THitActor*, unsigned int) { return 0; }
void TMuddyBoat::bind() {}
void TMuddyBoat::calc() {}
void TMuddyBoat::control() {}
u32 TMuddyBoat::getSDLModelFlag() const { return 0; }
void TMuddyBoat::initMapObj() {}
void TMuddyBoat::kill() {}
void TMushroom1up::control() {}
void TMushroom1up::initMapObj() {}
void TMushroom1up::makeObjAppeared() {}
void TMushroom1up::perform(unsigned int, JDrama::TGraphics*) {}
void TMushroom1up::touchPlayer(THitActor*) {}
void TOneShotGenerator::loadAfter() {}
BOOL TOneShotGenerator::receiveMessage(THitActor*, unsigned int) { return 0; }
void TPinnaCoaster::initMapObj() {}
void TRedCoinSwitch::control() {}
void TRedCoinSwitch::loadAfter() {}
void TRedCoinSwitch::load(JSUMemoryInputStream&) {}
void TResetFruit::appearing() {}
void TResetFruit::breaking() {}
void TResetFruit::checkGroundCollision(JGeometry::TVec3<float>*) {}
void TResetFruit::control() {}
u32 TResetFruit::getLivingTime() const { return 0; }
void TResetFruit::hold(TTakeActor*) {}
void TResetFruit::initMapObj() {}
void TResetFruit::kicked() {}
void TResetFruit::makeObjAppeared() {}
BOOL TResetFruit::receiveMessage(THitActor*, unsigned int) { return 0; }
void TResetFruit::thrown() {}
void TResetFruit::touchActor(THitActor*) {}
void TResetFruit::touchGround(JGeometry::TVec3<float>*) {}
void TResetFruit::touchPollution() {}
void TResetFruit::touchWaterSurface() {}
u32 TResetFruit::touchWater(THitActor*) { return 0; }
void TResetFruit::waitingToAppear() {}
void TRiccoWatermill::calc() {}
void TRiccoWatermill::control() {}
void TRiccoWatermill::loadAfter() {}
void TSandBird::initMapObj() {}
TMapObjBase* TSandBird::makeObjFromJointName(char const*, unsigned short) { return nullptr; }
bool TSandBird::nameIsObj(char const*) { return 0; }
void TSandBombBase::initMapObj() {}
void TSandBombBase::loadAfter() {}
void TSandCastle::initMapObj() {}
void TSandCastle::loadAfter() {}
void TSandLeafBase::initMapObj() {}
void TShellCup::initMapObj() {}
void TShellCup::loadAfter() {}
void TShellCup::perform(unsigned int, JDrama::TGraphics*) {}
void TSwingBoard::control() {}
void TSwingBoard::load(JSUMemoryInputStream&) {}
void TViking::initMapObj() {}
void TViking::loadAfter() {}
void TWireBell::control() {}
void TWireBell::loadAfter() {}
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
// (emit vtable for TPauseMenu2)
void TPauseMenu2::load(JSUMemoryInputStream& stream)
{
	JDrama::TViewObj::load(stream); // read NameRef header so search("ポーズメニュー") finds it
}

// ring-4: last vtable-slot virtuals for the 3 misc classes (perform/loadAfter).
void TGuide::perform(unsigned int, JDrama::TGraphics*) {}
// TMBindShadowManager::perform lives natively in reference/sms/src/MarioUtil/ShadowUtil.cpp
void TPauseMenu2::loadAfter() {}
void TPauseMenu2::perform(unsigned int, JDrama::TGraphics*) {}

// enemy_stubs.cpp — SCAFFOLD-to-link layer for Enemy + Animal subsystems.
//
// These are minimal stub definitions that exist ONLY to satisfy the linker
// while the native engine boots.  None of these bodies implement real behavior;
// the real implementations either come from the decomp src (reference/sms/src/)
// or will be ported as PC-native overrides later.
//
// Symbols covered: the 37 undefined symbols listed in scratch/boot_buckets/enemy.txt
// that are NOT already defined in the decomp src tree (reference/sms/src/).
//
// Symbols already in decomp src (NOT stubbed here, would be ODR violations):
//   TEffectBiancoFunsui::loadAfter / emitEffect  (effectObj.cpp)
//   TEffectPinnaFunsui::loadAfter / emitEffect   (effectObj.cpp)
//   TGesso and TGessoManager methods             (gesso.cpp)
//   TNameKuriLauncher::stateLaunch               (namekuri.cpp)
//   TRiccoHook / THookTake / THookParams ctors   (riccohook.cpp)
//   TSmallEnemy (most methods)                   (smallEnemy.cpp)
//   TStageEnemyInfo::load                        (enemytable.cpp)
//   TEnemyManager (most methods)                 (enemymanager.cpp)
//   TAnimalManagerBase / TMewManager methods     (AnimalManager.cpp)

// ---------------------------------------------------------------------------
// Animal subsystem
// ---------------------------------------------------------------------------
#include <Animal/AnimalBase.hpp>

// TAnimalBase::TAnimalBase(u32, const char*)
TAnimalBase::TAnimalBase(u32 param0, const char* name)
    : TSpineEnemy(name)
{
    (void)param0;
    mFrameTimer = nullptr;
}

// TAnimalBase::execWalk(bool)
void TAnimalBase::execWalk(bool) { }

// TAnimalBase::resetRandomCurPathNode()
void TAnimalBase::resetRandomCurPathNode() { }

// ---------------------------------------------------------------------------
// Animal manager — TMewManager ctor (methods are in AnimalManager.cpp)
// ---------------------------------------------------------------------------
#include <Animal/AnimalManager.hpp>

TMewManager::TMewManager(const char* name)
    : TAnimalManagerBase(name)
{
}

// ---------------------------------------------------------------------------
// AreaCylinder
// ---------------------------------------------------------------------------
#include <Enemy/AreaCylinder.hpp>

TAreaCylinder::TAreaCylinder(const char* name)
    : JDrama::TViewObj(name)
    , unk10(0.f), unk14(0.f), unk18(0.f), unk1C(0.f), unk20(0.f), unk24(0.f)
{
}

TAreaCylinder* TAreaCylinderManager::getCylinderContains(
        const JGeometry::TVec3<f32>&)
{
    return nullptr;
}

// ---------------------------------------------------------------------------
// EffectObj — ctors only (loadAfter/emitEffect are in effectObj.cpp)
// ---------------------------------------------------------------------------
#include <Enemy/EffectObj.hpp>

TEffectBiancoFunsui::TEffectBiancoFunsui(const char* name)
    : TSimpleEffect(name)
{
}

TEffectPinnaFunsui::TEffectPinnaFunsui(const char* name)
    : TSimpleEffect(name)
{
}

// ---------------------------------------------------------------------------
// EnemyManager — static data member definition
// ---------------------------------------------------------------------------
#include <Enemy/EnemyManager.hpp>

bool TEnemyManager::mIsCopyAnmMtx = false;

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------
#include <Enemy/Generator.hpp>

TGenerator::TGenerator(const char* name)
    : JDrama::TViewObj(name)
{
}

TOneShotGenerator::TOneShotGenerator(const char* name)
    : THitActor(name)
    , mSpawnKey2(nullptr)
    , unk6C(0)
    , mSpawnKey1(nullptr)
{
}

// ---------------------------------------------------------------------------
// Gesso — TLandGesso and TSurfGesso ctors
// (TGesso methods are in reference/sms/src/Enemy/gesso.cpp)
// ---------------------------------------------------------------------------
#include <Enemy/Gesso.hpp>

TLandGesso::TLandGesso(const char* name)
    : TGesso(name)
{
}

TSurfGesso::TSurfGesso(const char* name)
    : TGesso(name)
{
}

// ---------------------------------------------------------------------------
// MapObjBase — virtual method bodies
// (TMapObjBase::TMapObjBase ctor + receiveMessage + getRootJointMtx are in
//  reference/sms/src/MoveBG/MapObjBase.cpp; everything else is stubbed here)
// ---------------------------------------------------------------------------
#include <MoveBG/MapObjBase.hpp>

void TMapObjBase::appear() { }
// calc/draw/dead now defined in reference/sms/src/MoveBG/MapObjBase.cpp
// alongside the perform() port (all four are byte-exact `blr` in the
// binary — empty stubs for vtable slot fill).
void TMapObjBase::getDepthAtFloating() { }
u16  TMapObjBase::getHitObjNumMax() { return 0; }
f32  TMapObjBase::getRadiusAtY(f32) const { return 0.f; }
MtxPtr TMapObjBase::getTakingMtx() { return nullptr; }
void TMapObjBase::kill() { }
void TMapObjBase::loadBeforeInit(JSUMemoryInputStream&) { }
void TMapObjBase::setModelMtx(MtxPtr) { }
u32  TMapObjBase::touchWater(THitActor*) { return 0; }

// ---------------------------------------------------------------------------
// MtxCalcFootInv
// (typeinfo is emitted automatically by defining the class methods here)
// ---------------------------------------------------------------------------
#include <Enemy/FeetInv.hpp>

TMtxCalcFootInv::TMtxCalcFootInv(u16 a, u16 b, u16 c, u16 d, u16 e, u16 f, f32 g)
    : J3DMtxCalcSoftimageAnm(nullptr)
    , unk68(a), unk6A(b), unk6C(c), unk6E(d), unk70(e), unk72(f), unk74(g)
{
}

void TMtxCalcFootInv::calc(u16) { }

// ---------------------------------------------------------------------------
// NameKuri — TNameKuriLauncher ctor + TDiffusionNameKuriManager ctor
// (TNameKuriLauncher::stateLaunch + TNameKuri* methods are in namekuri.cpp)
// ---------------------------------------------------------------------------
#include <Enemy/NameKuri.hpp>

TNameKuriLauncher::TNameKuriLauncher(const char* name)
    : TLauncher(name)
{
}

TDiffusionNameKuriManager::TDiffusionNameKuriManager(const char* name)
    : TNameKuriManager(name)
{
}

// ---------------------------------------------------------------------------
// RiccoHook — TRiccoHookManager::perform
// (TRiccoHookManager ctor and most methods are in riccohook.cpp)
// ---------------------------------------------------------------------------
#include <Enemy/RiccoHook.hpp>

void TRiccoHookManager::perform(u32, JDrama::TGraphics*) { }

// ---------------------------------------------------------------------------
// Rocket
// ---------------------------------------------------------------------------
#include <Enemy/Rocket.hpp>

TRocket::TRocket(const char* name)
    : TSmallEnemy(name)
{
}

bool TRocket::isAttack() { return false; }

// ---------------------------------------------------------------------------
// SmallEnemy — TSmallEnemy::initAttacker
// (rest of TSmallEnemy is in smallEnemy.cpp)
// ---------------------------------------------------------------------------
#include <Enemy/SmallEnemy.hpp>

void TSmallEnemy::initAttacker(THitActor*) { }

// ---------------------------------------------------------------------------
// EnemyTable — TStageEnemyInfo ctor
// (TStageEnemyInfo::load + TStageEnemyInfoTable are in enemytable.cpp)
// ---------------------------------------------------------------------------
#include <Enemy/EnemyTable.hpp>

TStageEnemyInfo::TStageEnemyInfo()
    : JDrama::TNameRef("<TStageEnemyInfo>")
    , mManagerName(nullptr)
    , mFlags(0)
    , mWeight(0)
{
}

// TMBindShadowBody ctor + entryDrawShadow live natively in
// reference/sms/src/MarioUtil/ShadowUtil.cpp — no stub needed here.

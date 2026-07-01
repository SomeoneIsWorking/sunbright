// SCAFFOLD-TO-LINK: ui_map_stubs.cpp
// Stub definitions for the 39 symbols in scratch/boot_buckets/ui_map.txt.
// These are ACKNOWLEDGED STUBS — minimal bodies that satisfy the linker.
// Faithful implementations land later as the boot-path actually exercises each class.
// Generated: 2026-06-20. Replace each stub as the class is ported.

// ─── GC2D ─────────────────────────────────────────────────────────────────────

#include <GC2D/BlendPane.hpp>
#include <GC2D/GCConsole2.hpp>
#include <GC2D/Guide.hpp>
#include <GC2D/PauseMenu2.hpp>
#include <GC2D/SunGlass.hpp>
#include <GC2D/Talk2D2.hpp>

// TBlendPane — inherits TBoundPane; has virtual update().
// vtable emitted when we define the non-inline virtual.
TBlendPane::TBlendPane(J2DScreen* screen, u32 tag)
    : TBoundPane(screen, tag)
{
}
void TBlendPane::update() {}

// TGCConsole2 static data members (JUTPoint — default-init to 0).
JUTPoint TGCConsole2::cDownMidPoint;
JUTPoint TGCConsole2::cDownTopPoint;
JUTPoint TGCConsole2::cUpMidPoint;
JUTPoint TGCConsole2::cUpTopPoint;

// TGuide — inherits JDrama::TViewObj; non-virtual methods only in bucket.
TGuide::TGuide(const char* name)
    : JDrama::TViewObj(name)
{
}
void TGuide::setup(JKRMemArchive*) {}
void TGuide::startMoveCursor() {}

// TPauseMenu2 — inherits JDrama::TViewObj; non-virtual methods in bucket.
TPauseMenu2::TPauseMenu2(const char* name)
    : JDrama::TViewObj(name)
    , unk10(0)
    , unk14(nullptr)
    , unk18(nullptr)
    , unk1C(nullptr)
    , unk84(0.0f)
    , unk88(0.0f)
    , unk8C(0.0f)
    , unk90(0.0f)
    , unk94(0)
    , unkD4(nullptr)
    , unkD8(nullptr)
    , unkDC(nullptr)
    , unkE0(0)
    , unkE4(0)
    , unkE8(0)
    , unkEC(0)
    , unkF4(0)
    , unkF8(0)
    , unkFC(0)
    , unk100(0)
    , unk102(0)
    , unk104(0)
    , unk108(0)
    , unk109(0)
    , unk110(nullptr)
    , unk10C(nullptr)
    , unk114(0)
    , unk118(nullptr)
{
    for (int i = 0; i < 5; i++) unk20[i] = nullptr;
    for (int i = 0; i < 3; i++) unk98[i] = nullptr;
}
u8 TPauseMenu2::getNextState() { return 0; }
void TPauseMenu2::setDrawEnd() {}
void TPauseMenu2::setDrawStart() {}

// TSunGlass — inherits JDrama::TViewObj; has 3 non-inline virtuals.
// Must read the NameRef header (keyCode + name) so the object is findable by
// JDrama::TNameRefGen::search; setup2 looks it up by name ("サングラスフェーダ")
// and derefs the result. An empty stub left mName at the ctor default -> search
// returned null -> null deref in setup2.
void TSunGlass::load(JSUMemoryInputStream& stream)
{
	JDrama::TViewObj::load(stream);
}
void TSunGlass::loadAfter() {}
void TSunGlass::perform(u32, JDrama::TGraphics*) {}

// TSunShine — inherits TSunGlass; overrides 2 virtuals.
void TSunShine::loadAfter() {}
void TSunShine::perform(u32, JDrama::TGraphics*) {}

// TTalk2D2 — inherits JDrama::TViewObj; 3 pure non-virtuals + 3 overrides.
TTalk2D2::TTalk2D2(const char* name)
    : JDrama::TViewObj(name)
{
}
void TTalk2D2::load(JSUMemoryInputStream& stream)
{
	JDrama::TViewObj::load(stream); // read NameRef header so search() finds it
}
void TTalk2D2::loadAfter() {}
void TTalk2D2::perform(u32, JDrama::TGraphics*) {}
void TTalk2D2::forceCloseTalk() {}
void TTalk2D2::openTalkWindow(TBaseNPC*) {}
void TTalk2D2::setMessageID(u32, u32) {}

void* TTalk2D2::cColorTable = nullptr;

// ─── Map ──────────────────────────────────────────────────────────────────────

#include <Map/BathWaterManager.hpp>
#include <Map/MapCollisionEntry.hpp>
#include <Map/MapWireManager.hpp>
#include <Map/PollutionLayer.hpp>

// TBathWaterManager — inherits JDrama::TViewObj; has load/loadAfter/perform.
// unk10 is JMath::TRandom_fast_ which has no default ctor; seed with 0.
TBathWaterManager::TBathWaterManager()
    : JDrama::TViewObj("<バスタブの水>")
    , unk10(0u)
    , unk14(nullptr)
    , unk18(nullptr)
    , unk1C(0)
    , unk20(nullptr)
    , unk24(0)
    , unk28(nullptr)
    , unk2C(nullptr)
    , unk30(nullptr)
    , unk34(this)
{
}

// TMapCollisionBase::setMtx — plain non-virtual member.
void TMapCollisionBase::setMtx(MtxPtr mtx)
{
    MTXCopy(mtx, unk20);
}

// TMapWireActor statics.
f32 TMapWireActor::mCommonAttackHeight = 0.0f;
f32 TMapWireActor::mCommonAttackRadius = 0.0f;

// TPollutionLayer — has pure-abstract-in-base virtuals that THIS class is the
// first concrete owner of.  Define them so vtable for TPollutionLayer is emitted.
int TPollutionLayer::getPlaneType() const { return 0; }
int TPollutionLayer::getTexPosS(f32) const { return 0; }
int TPollutionLayer::getTexPosT(f32) const { return 0; }
bool TPollutionLayer::isInArea(f32, f32, f32) const { return false; }

// ─── Strategic ────────────────────────────────────────────────────────────────

#include <Strategic/TakeActor.hpp>

// TTakeActor::getRadiusAtY — non-inline virtual declared in TakeActor.hpp.
// typeinfo for TTakeActor is emitted when its first non-inline virtual is defined.
f32 TTakeActor::getRadiusAtY(f32) const { return 0.0f; }

// ─── System ───────────────────────────────────────────────────────────────────

#include <System/MarioGamePad.hpp>
#include <System/SelectDir.hpp>

// TMarioGamePad — inherits JUTGamePad; dtor is the sole non-inline virtual.
// vtable emitted by defining dtor.
TMarioGamePad::~TMarioGamePad() {}

// (TMarioGamePad::mResetFlag is defined in MarioGamePad.cpp — not stubbed, was over-reach.)

// TSelectDir is now faithfully reconstructed in reference/sms/src/GC2D/SelectDir.cpp
// (file-select port). The stub here is removed so the real implementation links.

// ─── MarioUtil / Shadow ───────────────────────────────────────────────────────

// TMBindShadowManager ctor + forceRequest/request live natively in
// reference/sms/src/MarioUtil/ShadowUtil.cpp — no stub needed here.

// ─── Player ───────────────────────────────────────────────────────────────────

#include <Player/ModelWaterManager.hpp>

// TWaterHitActor — inherits THitActor; has one non-inline virtual.
// vtable + typeinfo emitted by defining receiveMessage.
BOOL TWaterHitActor::receiveMessage(THitActor*, u32) { return 0; }

// ─── JSystem / JKernel ────────────────────────────────────────────────────────

#include <JSystem/JKernel/JKRFileFinder.hpp>

// JKRDvdFinder — inherits JKRFileFinder; needs findNextFile (pure virtual) defined
// to make the vtable non-abstract.
JKRDvdFinder::JKRDvdFinder(const char*)
    : mIsDvdOpen(false)
{
}
bool JKRDvdFinder::findNextFile() { return false; }

// ─── JSystem / JParticle ──────────────────────────────────────────────────────

#include <JSystem/JParticle/JPATexture.hpp>

// JPADefaultTexture::~JPADefaultTexture — non-inline dtor; emits vtable/typeinfo.
JPADefaultTexture::~JPADefaultTexture() {}

// ─── M3DUtil ──────────────────────────────────────────────────────────────────

#include <M3DUtil/M3UJoint.hpp>

// M3UMtxCalcSIAnmBlendQuat — default ctor (the bool-param ctor is in M3UJoint.cpp).
M3UMtxCalcSIAnmBlendQuat::M3UMtxCalcSIAnmBlendQuat()
    : J3DMtxCalcSoftimage()
    , unk50(0.0f)
    , unk54(nullptr)
    , unk58(nullptr)
    , unk5C(false)
    , unk60(0.0f)
{
}

// ─── JSystem / JAudio ─────────────────────────────────────────────────────────

#include <JSystem/JAudio/JAInterface/JAIBasic.hpp>

// JAIBasic::~JAIBasic — non-inline dtor; emits vtable.
// (The virtual table has many pure/virtual entries defined in JASTrack etc.)
JAIBasic::~JAIBasic() {}

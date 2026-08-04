// SCAFFOLD-TO-LINK: ui_map_stubs.cpp
// Stub definitions for the 39 symbols in scratch/boot_buckets/ui_map.txt.
// These are ACKNOWLEDGED STUBS — minimal bodies that satisfy the linker.
// Faithful implementations land later as the boot-path actually exercises each class.
// Generated: 2026-06-20. Replace each stub as the class is ported.

// ─── GC2D ─────────────────────────────────────────────────────────────────────

#include "stub_trace.h"
#include <GC2D/BlendPane.hpp>
#include <GC2D/GCConsole2.hpp>
#include <GC2D/Guide.hpp>
#include <GC2D/PauseMenu2.hpp>
#include <GC2D/SunGlass.hpp>
#include <GC2D/Talk2D2.hpp>

// TBlendPane's ctor and update() were stubs here until upstream implemented them for real
// (decomp/sms src/GC2D/BlendPane.cpp — update() returns bool and drives the blend progress).
// Deleted rather than kept alongside: a stub that shadows a real body is a silent downgrade, and
// two definitions of the same symbol is a link error waiting for whichever order the build picks.

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
void TGuide::setup(JKRMemArchive*) { SB_STUB_HIT("TGuide::setup"); }
void TGuide::startMoveCursor() { SB_STUB_HIT("TGuide::startMoveCursor"); }

// TPauseMenu2 fully implemented natively in decomp/sms/src/GC2D/PauseMenu2.cpp.

// TSunGlass + TSunShine load/loadAfter/perform live natively in
// decomp/sms/src/GC2D/SunGlass.cpp — no stubs needed here.

// TTalk2D2 — inherits JDrama::TViewObj; 3 pure non-virtuals + 3 overrides.
TTalk2D2::TTalk2D2(const char* name)
    : JDrama::TViewObj(name)
{
}
void TTalk2D2::load(JSUMemoryInputStream& stream)
{
	JDrama::TViewObj::load(stream); // read NameRef header so search() finds it
}
void TTalk2D2::loadAfter() { SB_STUB_HIT("TTalk2D2::loadAfter"); }
void TTalk2D2::perform(u32, JDrama::TGraphics*) { SB_STUB_HIT("TTalk2D2::perform"); }
void TTalk2D2::forceCloseTalk() { SB_STUB_HIT("TTalk2D2::forceCloseTalk"); }
void TTalk2D2::openTalkWindow(TBaseNPC*) { SB_STUB_HIT("TTalk2D2::openTalkWindow"); }
void TTalk2D2::setMessageID(u32, u32) { SB_STUB_HIT("TTalk2D2::setMessageID"); }

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

// setMtx / TMapWireActor statics / TPollutionLayer methods now live natively
// in decomp/sms (upstream 2026-07 sync).

// ─── Strategic ────────────────────────────────────────────────────────────────

#include <Strategic/TakeActor.hpp>

// TTakeActor::getRadiusAtY — non-inline virtual declared in TakeActor.hpp.
// typeinfo for TTakeActor is emitted when its first non-inline virtual is defined.
f32 TTakeActor::getRadiusAtY(f32) const { SB_STUB_HIT("TTakeActor::getRadiusAtY"); return 0.0f; }

// ─── System ───────────────────────────────────────────────────────────────────

#include <System/MarioGamePad.hpp>
#include <System/SelectDir.hpp>

// TMarioGamePad — inherits JUTGamePad; dtor is the sole non-inline virtual.
// vtable emitted by defining dtor.
TMarioGamePad::~TMarioGamePad() {}

// (TMarioGamePad::mResetFlag is defined in MarioGamePad.cpp — not stubbed, was over-reach.)

// TSelectDir is now faithfully reconstructed in decomp/sms/src/GC2D/SelectDir.cpp
// (file-select port). The stub here is removed so the real implementation links.

// ─── MarioUtil / Shadow ───────────────────────────────────────────────────────

// TMBindShadowManager ctor + forceRequest/request live natively in
// decomp/sms/src/MarioUtil/ShadowUtil.cpp — no stub needed here.

// ─── Player ───────────────────────────────────────────────────────────────────

#include <Player/ModelWaterManager.hpp>

// TWaterHitActor — inherits THitActor; has one non-inline virtual.
// vtable + typeinfo emitted by defining receiveMessage.
BOOL TWaterHitActor::receiveMessage(THitActor*, u32) { SB_STUB_HIT("TWaterHitActor::receiveMessage"); return 0; }

// ─── JSystem / JKernel ────────────────────────────────────────────────────────

#include <JSystem/JKernel/JKRFileFinder.hpp>

// JKRDvdFinder — inherits JKRFileFinder; needs findNextFile (pure virtual) defined
// to make the vtable non-abstract.
JKRDvdFinder::JKRDvdFinder(const char*)
    : mIsDvdOpen(false)
{
}
bool JKRDvdFinder::findNextFile() { SB_STUB_HIT("JKRDvdFinder::findNextFile"); return false; }

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
    , mAnmTransformNew(nullptr)
    , mAnmTransformOld(nullptr)
    , unk5C(false)
    , unk60(0.0f)
{
}

// ─── JSystem / JAudio ─────────────────────────────────────────────────────────

#include <JSystem/JAudio/JAInterface/JAIBasic.hpp>

// JAIBasic::~JAIBasic — non-inline dtor; emits vtable.
// (The virtual table has many pure/virtual entries defined in JASTrack etc.)
JAIBasic::~JAIBasic() {}

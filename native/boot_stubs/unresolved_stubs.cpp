// boot_stubs/unresolved_stubs.cpp — SCAFFOLD-to-link stubs for the boot closure's
// free functions / globals / nerve singletons the decomp left undefined. These are
// off the boot->scene hot path (boss AI nerves, movie-wipe, low-level DSP, audio
// list dtors) or PPC intrinsics with no native effect. Minimal/no-op bodies; faithful
// versions replace them as the runtime exercises each. NOT a bandaid — an explicit,
// acknowledged link scaffold (see CLAUDE.md: named stopgap, not a silent patch).

#include <Enemy/BossEel.hpp>
#include <Enemy/BossHanachan.hpp>
#include <Enemy/BossPakkun.hpp>
#include <Enemy/BossWanwan.hpp>
#include <MarioUtil/MtxUtil.hpp>
#include <Enemy/FeetInv.hpp>
#include <GC2D/hx_wiper.h>
#include <JSystem/dspproc.h>
#include <JSystem/dsptask.h>
#include <Strategic/spcinterp.hpp>
#include <Enemy/Conductor.hpp>
#include <MoveBG/MapObjWave.hpp>
#include <GC2D/Talk2D2.hpp>
#include <MarioUtil/ShadowUtil.hpp>
#include <dolphin/base/PPCArch.h>
#include <JSystem/JAudio/JASystem/JASProbe.hpp>

// ---- boss-AI nerve singletons (DEFINE_NERVE provides theNerve(); execute() is
//      compiled elsewhere, so only the accessor is missing) ----
const TNerveBEelTearsGenerate& TNerveBEelTearsGenerate::theNerve() { static TNerveBEelTearsGenerate n; return n; }
const TNerveBEelTearsMarioRecover& TNerveBEelTearsMarioRecover::theNerve() { static TNerveBEelTearsMarioRecover n; return n; }
const TNerveBEelTearsMoveUp& TNerveBEelTearsMoveUp::theNerve() { static TNerveBEelTearsMoveUp n; return n; }
const TNerveBEelTearsSplit& TNerveBEelTearsSplit::theNerve() { static TNerveBEelTearsSplit n; return n; }
const TNerveBEelTearsWaterHit& TNerveBEelTearsWaterHit::theNerve() { static TNerveBEelTearsWaterHit n; return n; }
const TNerveBPBreakSleep& TNerveBPBreakSleep::theNerve() { static TNerveBPBreakSleep n; return n; }
const TNerveBPCannon& TNerveBPCannon::theNerve() { static TNerveBPCannon n; return n; }
const TNerveBPCannonL& TNerveBPCannonL::theNerve() { static TNerveBPCannonL n; return n; }
const TNerveBPDie& TNerveBPDie::theNerve() { static TNerveBPDie n; return n; }
const TNerveBPFall& TNerveBPFall::theNerve() { static TNerveBPFall n; return n; }
const TNerveBPFly& TNerveBPFly::theNerve() { static TNerveBPFly n; return n; }
const TNerveBPFlyCannon& TNerveBPFlyCannon::theNerve() { static TNerveBPFlyCannon n; return n; }
const TNerveBPFlyPivot& TNerveBPFlyPivot::theNerve() { static TNerveBPFlyPivot n; return n; }
const TNerveBPGetUp& TNerveBPGetUp::theNerve() { static TNerveBPGetUp n; return n; }
const TNerveBPHover& TNerveBPHover::theNerve() { static TNerveBPHover n; return n; }
const TNerveBPJumpReact& TNerveBPJumpReact::theNerve() { static TNerveBPJumpReact n; return n; }
const TNerveBPPivot& TNerveBPPivot::theNerve() { static TNerveBPPivot n; return n; }
const TNerveBPPreDie& TNerveBPPreDie::theNerve() { static TNerveBPPreDie n; return n; }
const TNerveBPSleep& TNerveBPSleep::theNerve() { static TNerveBPSleep n; return n; }
const TNerveBPStompReact& TNerveBPStompReact::theNerve() { static TNerveBPStompReact n; return n; }
const TNerveBPSwallow& TNerveBPSwallow::theNerve() { static TNerveBPSwallow n; return n; }
const TNerveBPSwing& TNerveBPSwing::theNerve() { static TNerveBPSwing n; return n; }
const TNerveBPTakeOff& TNerveBPTakeOff::theNerve() { static TNerveBPTakeOff n; return n; }
const TNerveBPTornado& TNerveBPTornado::theNerve() { static TNerveBPTornado n; return n; }
const TNerveBPTouchDown& TNerveBPTouchDown::theNerve() { static TNerveBPTouchDown n; return n; }
const TNerveBPTumble& TNerveBPTumble::theNerve() { static TNerveBPTumble n; return n; }
const TNerveBPTumbleIn& TNerveBPTumbleIn::theNerve() { static TNerveBPTumbleIn n; return n; }
const TNerveBPTumbleOut& TNerveBPTumbleOut::theNerve() { static TNerveBPTumbleOut n; return n; }
const TNerveBPVomit& TNerveBPVomit::theNerve() { static TNerveBPVomit n; return n; }
const TNerveBPWait& TNerveBPWait::theNerve() { static TNerveBPWait n; return n; }
const TNerveBPWaitL& TNerveBPWaitL::theNerve() { static TNerveBPWaitL n; return n; }
const TNerveBWBark& TNerveBWBark::theNerve() { static TNerveBWBark n; return n; }
const TNerveBWDie& TNerveBWDie::theNerve() { static TNerveBWDie n; return n; }
const TNerveBWFall& TNerveBWFall::theNerve() { static TNerveBWFall n; return n; }
const TNerveBWGraphWander& TNerveBWGraphWander::theNerve() { static TNerveBWGraphWander n; return n; }
const TNerveBWJump& TNerveBWJump::theNerve() { static TNerveBWJump n; return n; }
const TNerveBWJumpAway& TNerveBWJumpAway::theNerve() { static TNerveBWJumpAway n; return n; }
const TNerveBWJumpToBath& TNerveBWJumpToBath::theNerve() { static TNerveBWJumpToBath n; return n; }
const TNerveBWRoll& TNerveBWRoll::theNerve() { static TNerveBWRoll n; return n; }
const TNerveBWShake& TNerveBWShake::theNerve() { static TNerveBWShake n; return n; }
const TNerveBWStun& TNerveBWStun::theNerve() { static TNerveBWStun n; return n; }
const TNerveBWWakeup& TNerveBWWakeup::theNerve() { static TNerveBWWakeup n; return n; }
const TNerveBossEelAppear& TNerveBossEelAppear::theNerve() { static TNerveBossEelAppear n; return n; }
const TNerveBossEelDie& TNerveBossEelDie::theNerve() { static TNerveBossEelDie n; return n; }
const TNerveBossEelEat& TNerveBossEelEat::theNerve() { static TNerveBossEelEat n; return n; }
const TNerveBossEelFirstSpin& TNerveBossEelFirstSpin::theNerve() { static TNerveBossEelFirstSpin n; return n; }
const TNerveBossEelMouthOpenWait& TNerveBossEelMouthOpenWait::theNerve() { static TNerveBossEelMouthOpenWait n; return n; }
const TNerveBossEelOutWait& TNerveBossEelOutWait::theNerve() { static TNerveBossEelOutWait n; return n; }
const TNerveBossEelQuickBack& TNerveBossEelQuickBack::theNerve() { static TNerveBossEelQuickBack n; return n; }
const TNerveBossEelSecondSpin& TNerveBossEelSecondSpin::theNerve() { static TNerveBossEelSecondSpin n; return n; }
const TNerveBossEelSleepOnBottom& TNerveBossEelSleepOnBottom::theNerve() { static TNerveBossEelSleepOnBottom n; return n; }
const TNerveBossEelSlowBack& TNerveBossEelSlowBack::theNerve() { static TNerveBossEelSlowBack n; return n; }
const TNerveBossEelWaitAppear& TNerveBossEelWaitAppear::theNerve() { static TNerveBossEelWaitAppear n; return n; }
const TNerveBossHanachanDamage& TNerveBossHanachanDamage::theNerve() { static TNerveBossHanachanDamage n; return n; }
const TNerveBossHanachanDead& TNerveBossHanachanDead::theNerve() { static TNerveBossHanachanDead n; return n; }
const TNerveBossHanachanDown& TNerveBossHanachanDown::theNerve() { static TNerveBossHanachanDown n; return n; }
const TNerveBossHanachanGetUp& TNerveBossHanachanGetUp::theNerve() { static TNerveBossHanachanGetUp n; return n; }
const TNerveBossHanachanGraphWander& TNerveBossHanachanGraphWander::theNerve() { static TNerveBossHanachanGraphWander n; return n; }
const TNerveBossHanachanSnort& TNerveBossHanachanSnort::theNerve() { static TNerveBossHanachanSnort n; return n; }
const TNerveBossHanachanTumble& TNerveBossHanachanTumble::theNerve() { static TNerveBossHanachanTumble n; return n; }
const TNerveOilBallStay& TNerveOilBallStay::theNerve() { static TNerveOilBallStay n; return n; }
const TNerveSBH_Fall& TNerveSBH_Fall::theNerve() { static TNerveSBH_Fall n; return n; }
const TNerveSBH_SleepContinue& TNerveSBH_SleepContinue::theNerve() { static TNerveSBH_SleepContinue n; return n; }

// ---- movie-wipe (GC2D/hx_wiper.h) ----
void Hx_StartWipe(int, int) {}
void Hx_ResetWipe(u32, u32) {}
u32  Hx_UpdateWipe(f32) { return 0; }
int  Hx_GetWipeType(int) { return 0; }
int  Hx_MovieStartSyncEx() { return 0; }
void Hx_ProvideResource(void*, int) {}
void Hx_ProvideResourceEx(void*) {}
void Hx_RemoveResource() {}

// ---- low-level DSP/audio (JSystem/dspproc.h, dsptask.h) ----
void DSPReleaseHalt() {}
void DsetMixerLevel(float) {}
void DsetupTable(u32, u32, u32, u32, u32) {}
void DspBoot(void (*)(void*)) {}
void DspFinishWork(u16) {}
void DsyncFrame2(u32, u32, u32) {}

// ---- matrix / effect helpers (MarioUtil/MtxUtil.hpp, Enemy/FeetInv.hpp) ----
void SMS_GetActorMtx(const THitActor&, MtxPtr) {}
void SMS_GetLightPerspectiveForEffectMtx(MtxPtr) {}
void SMS_MakeJointsToArc(J3DModel*, const JGeometry::TVec3<f32>&, const JGeometry::TVec3<f32>&, const JGeometry::TVec3<f32>&) {}
void FeetInvCalc(J3DModel*, u16, u16, u16, f32) {}
int  TMtxSwingRZCallBack(J3DNode*, int) { return 0; }
int  TMtxSwingRZReverseXZCallBack(J3DNode*, int) { return 0; }
int  TMtxTimeLagCallBack(J3DNode*, int) { return 0; }

// ---- spc trace (Strategic/spcinterp.hpp) ----
void SpcTrace(const char*, ...) {}

// ---- JAS kernel probe (no native profiler) ----
void JASystem::Kernel::probeStart(s32, char*) {}
void JASystem::Kernel::probeFinish(s32) {}

// ---- PPC intrinsics — no native effect (PC has no MSR / sync) ----
u32  PPCMfmsr() { return 0; }
void PPCSync() {}

// ---- globals the catalogue references (null until the owning manager constructs) ----
TConductor*          gpConductor          = nullptr;
TMapObjWave*         gpMapObjWave         = nullptr;
TTalk2D2*            gpTalk2D             = nullptr;
TMBindShadowManager* gpBindShadowManager  = nullptr;

// NOTE: 'non-virtual thunk to TMtxCalcFootInv::calc(u16)' is emitted automatically
// alongside TMtxCalcFootInv::calc() in the enemy bucket — not stubbed here.

// ---- audio list template dtors -> provided by mangled name in unresolved_stubs_asm.S
//      (template bodies are out-of-line/undefined; CRTP + missing defs block C++ here) ----
// MSSetSoundTL<MSSetSoundGrp>::~MSSetSoundTL() — CRTP self-instantiation blocks an
// explicit specialization here; provided by mangled name in unresolved_stubs_asm.S.

// ---- nerve execute() bodies (DECLARE_NERVE declares the virtual; defining it emits
//      the vtable that theNerve()'s static instance needs). Off-path boss AI -> FALSE. ----
BOOL TNerveBEelTearsGenerate::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBEelTearsMarioRecover::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBEelTearsMoveUp::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBEelTearsSplit::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBEelTearsWaterHit::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPBreakSleep::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPCannon::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPCannonL::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPDie::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPFall::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPFly::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPFlyCannon::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPFlyPivot::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPGetUp::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPHover::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPJumpReact::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPPivot::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPPreDie::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPSleep::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPStompReact::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPSwallow::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPSwing::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPTakeOff::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPTornado::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPTouchDown::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPTumble::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPTumbleIn::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPTumbleOut::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPVomit::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPWait::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBPWaitL::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWBark::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWDie::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWFall::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWGraphWander::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWJump::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWJumpAway::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWJumpToBath::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWRoll::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWShake::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWStun::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBWWakeup::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelAppear::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelDie::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelEat::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelFirstSpin::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelMouthOpenWait::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelOutWait::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelQuickBack::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelSecondSpin::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelSleepOnBottom::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelSlowBack::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossEelWaitAppear::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossHanachanDamage::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossHanachanDead::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossHanachanDown::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossHanachanGetUp::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossHanachanGraphWander::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossHanachanSnort::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveBossHanachanTumble::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveOilBallStay::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveSBH_Fall::execute(TSpineBase<TLiveActor>*) const { return FALSE; }
BOOL TNerveSBH_SleepContinue::execute(TSpineBase<TLiveActor>*) const { return FALSE; }

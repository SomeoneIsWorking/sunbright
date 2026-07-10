// boot_stubs/unresolved_stubs.cpp — SCAFFOLD-to-link stubs for the boot closure's
// free functions / globals / nerve singletons the decomp left undefined. These are
// off the boot->scene hot path (boss AI nerves, movie-wipe, low-level DSP, audio
// list dtors) or PPC intrinsics with no native effect. Minimal/no-op bodies; faithful
// versions replace them as the runtime exercises each. NOT a bandaid — an explicit,
// acknowledged link scaffold (see CLAUDE.md: named stopgap, not a silent patch).

#include "stub_trace.h"
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

// ---- movie-wipe (GC2D/hx_wiper.h): the 8 referenced Hx_ symbols are now a faithful
//      native port in sms-boot/runtime/hx_wipe.cpp (was stubbed here). ----

// ---- low-level DSP/audio (JSystem/dspproc.h, dsptask.h) ----
// INTENTIONAL SEAM: part of the documented audio-silence gap (CLAUDE.md "Named audio
// arc" — the JAS DSP-frame mixer is not ported yet; output = aurora::audio via
// sb_audio_frame). These raw DSP-hardware mailbox/mixer primitives have no meaning
// until that port lands; no loud stub here duplicates the already-tracked audio gap.
void DSPReleaseHalt() {}
void DsetMixerLevel(float) {}
void DsetupTable(u32, u32, u32, u32, u32) {}
void DspBoot(void (*)(void*)) {}
void DspFinishWork(u16) {}
void DsyncFrame2(u32, u32, u32) {}

// ---- matrix / effect helpers (MarioUtil/MtxUtil.hpp, Enemy/FeetInv.hpp) ----
// SILENT LANDMINE class: these leave their MtxPtr/J3DModel out-params untouched (not
// even zeroed) rather than computing the real IK/effect matrix — a caller that reads
// the result gets whatever was already on the stack/heap. Loud-once so a boss/effect
// path that starts depending on real output doesn't fail silently like JRNISetTevOrder did.
void SMS_GetActorMtx(const THitActor&, MtxPtr) { SB_STUB_HIT("SMS_GetActorMtx"); }
void SMS_GetLightPerspectiveForEffectMtx(MtxPtr) { SB_STUB_HIT("SMS_GetLightPerspectiveForEffectMtx"); }
void SMS_MakeJointsToArc(J3DModel*, const JGeometry::TVec3<f32>&, const JGeometry::TVec3<f32>&, const JGeometry::TVec3<f32>&) { SB_STUB_HIT("SMS_MakeJointsToArc"); }
void FeetInvCalc(J3DModel*, u16, u16, u16, f32) { SB_STUB_HIT("FeetInvCalc"); }
int  TMtxSwingRZCallBack(J3DNode*, int) { SB_STUB_HIT("TMtxSwingRZCallBack"); return 0; }
int  TMtxSwingRZReverseXZCallBack(J3DNode*, int) { SB_STUB_HIT("TMtxSwingRZReverseXZCallBack"); return 0; }
int  TMtxTimeLagCallBack(J3DNode*, int) { SB_STUB_HIT("TMtxTimeLagCallBack"); return 0; }

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
BOOL TNerveBEelTearsGenerate::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBEelTearsGenerate::execute"); return FALSE; }
BOOL TNerveBEelTearsMarioRecover::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBEelTearsMarioRecover::execute"); return FALSE; }
BOOL TNerveBEelTearsMoveUp::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBEelTearsMoveUp::execute"); return FALSE; }
BOOL TNerveBEelTearsSplit::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBEelTearsSplit::execute"); return FALSE; }
BOOL TNerveBEelTearsWaterHit::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBEelTearsWaterHit::execute"); return FALSE; }
BOOL TNerveBPBreakSleep::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPBreakSleep::execute"); return FALSE; }
BOOL TNerveBPCannon::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPCannon::execute"); return FALSE; }
BOOL TNerveBPCannonL::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPCannonL::execute"); return FALSE; }
BOOL TNerveBPDie::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPDie::execute"); return FALSE; }
BOOL TNerveBPFall::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPFall::execute"); return FALSE; }
BOOL TNerveBPFly::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPFly::execute"); return FALSE; }
BOOL TNerveBPFlyCannon::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPFlyCannon::execute"); return FALSE; }
BOOL TNerveBPFlyPivot::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPFlyPivot::execute"); return FALSE; }
BOOL TNerveBPGetUp::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPGetUp::execute"); return FALSE; }
BOOL TNerveBPHover::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPHover::execute"); return FALSE; }
BOOL TNerveBPJumpReact::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPJumpReact::execute"); return FALSE; }
BOOL TNerveBPPivot::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPPivot::execute"); return FALSE; }
BOOL TNerveBPPreDie::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPPreDie::execute"); return FALSE; }
BOOL TNerveBPSleep::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPSleep::execute"); return FALSE; }
BOOL TNerveBPStompReact::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPStompReact::execute"); return FALSE; }
BOOL TNerveBPSwallow::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPSwallow::execute"); return FALSE; }
BOOL TNerveBPSwing::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPSwing::execute"); return FALSE; }
BOOL TNerveBPTakeOff::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPTakeOff::execute"); return FALSE; }
BOOL TNerveBPTornado::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPTornado::execute"); return FALSE; }
BOOL TNerveBPTouchDown::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPTouchDown::execute"); return FALSE; }
BOOL TNerveBPTumble::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPTumble::execute"); return FALSE; }
BOOL TNerveBPTumbleIn::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPTumbleIn::execute"); return FALSE; }
BOOL TNerveBPTumbleOut::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPTumbleOut::execute"); return FALSE; }
BOOL TNerveBPVomit::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPVomit::execute"); return FALSE; }
BOOL TNerveBPWait::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPWait::execute"); return FALSE; }
BOOL TNerveBPWaitL::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBPWaitL::execute"); return FALSE; }
BOOL TNerveBWBark::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWBark::execute"); return FALSE; }
BOOL TNerveBWDie::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWDie::execute"); return FALSE; }
BOOL TNerveBWFall::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWFall::execute"); return FALSE; }
BOOL TNerveBWGraphWander::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWGraphWander::execute"); return FALSE; }
BOOL TNerveBWJump::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWJump::execute"); return FALSE; }
BOOL TNerveBWJumpAway::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWJumpAway::execute"); return FALSE; }
BOOL TNerveBWJumpToBath::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWJumpToBath::execute"); return FALSE; }
BOOL TNerveBWRoll::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWRoll::execute"); return FALSE; }
BOOL TNerveBWShake::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWShake::execute"); return FALSE; }
BOOL TNerveBWStun::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWStun::execute"); return FALSE; }
BOOL TNerveBWWakeup::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBWWakeup::execute"); return FALSE; }
BOOL TNerveBossEelAppear::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelAppear::execute"); return FALSE; }
BOOL TNerveBossEelDie::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelDie::execute"); return FALSE; }
BOOL TNerveBossEelEat::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelEat::execute"); return FALSE; }
BOOL TNerveBossEelFirstSpin::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelFirstSpin::execute"); return FALSE; }
BOOL TNerveBossEelMouthOpenWait::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelMouthOpenWait::execute"); return FALSE; }
BOOL TNerveBossEelOutWait::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelOutWait::execute"); return FALSE; }
BOOL TNerveBossEelQuickBack::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelQuickBack::execute"); return FALSE; }
BOOL TNerveBossEelSecondSpin::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelSecondSpin::execute"); return FALSE; }
BOOL TNerveBossEelSleepOnBottom::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelSleepOnBottom::execute"); return FALSE; }
BOOL TNerveBossEelSlowBack::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelSlowBack::execute"); return FALSE; }
BOOL TNerveBossEelWaitAppear::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossEelWaitAppear::execute"); return FALSE; }
BOOL TNerveBossHanachanDamage::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossHanachanDamage::execute"); return FALSE; }
BOOL TNerveBossHanachanDead::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossHanachanDead::execute"); return FALSE; }
BOOL TNerveBossHanachanDown::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossHanachanDown::execute"); return FALSE; }
BOOL TNerveBossHanachanGetUp::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossHanachanGetUp::execute"); return FALSE; }
BOOL TNerveBossHanachanGraphWander::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossHanachanGraphWander::execute"); return FALSE; }
BOOL TNerveBossHanachanSnort::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossHanachanSnort::execute"); return FALSE; }
BOOL TNerveBossHanachanTumble::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveBossHanachanTumble::execute"); return FALSE; }
BOOL TNerveOilBallStay::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveOilBallStay::execute"); return FALSE; }
BOOL TNerveSBH_Fall::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveSBH_Fall::execute"); return FALSE; }
BOOL TNerveSBH_SleepContinue::execute(TSpineBase<TLiveActor>*) const { SB_STUB_HIT("TNerveSBH_SleepContinue::execute"); return FALSE; }

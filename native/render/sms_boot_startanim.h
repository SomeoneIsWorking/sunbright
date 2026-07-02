// sms_boot_startanim.h — pure decision predicates extracted from
// TMapObjBase::startAnim (@0x801b09d4, 125 insns).
//
// startAnim is mostly effectful (calls into TMActorKeeper::getMActor,
// TLiveActor::setAnmSound / stopAnmSound, MActor::getFrameCtrl / setAnimation
// on guest state), so what's tested here is the small set of hex constants
// and predicates the disasm hangs off. If any of these drift the port
// mis-dispatches silently (wrong slot, wrong flag, off-by-one on count).
//
// Everything here is Dolphin-free / no ROM / no GPU.

#pragma once

#include <cstddef>
#include <cstdint>

namespace sb::startanim {

// TMapObjAnimData size (bytes). Verified from `mulli r0,r0,0x14`
// at 801b0a68 and 801b0a9c — both compute `&anim->unk4[idx]` as
// `anim->unk4 + idx * 0x14`.
constexpr std::size_t kMapObjAnimDataSize = 0x14;

// The sentinel value stored in unkFE meaning "no active animation slot".
// Verified from `cmpli 0xffff` at 801b0a84 (bail if unkFE == 0xffff → skip
// the outgoing-slot reset) and from makeObjDead which writes
// `unkFE = 0xffff` after reset.
constexpr std::uint16_t kSentinelSlot = 0xFFFF;

// The MapObj flag bit that startBck / startAnim clear at their start
// (bit 8 of unkF8). Same value cleared here as in startBck.
// Verified from `rlwinm r0,r0,0,24,22` at 801b0b44 (MB=24,ME=22 = single
// bit at position 23 = 0x100).
constexpr std::uint32_t kMapObjFlagBit_ClearOnStart = 0x100;

// The MapObj flag bit tested for "should force-clear kMapObjFlagBit_ClearOnStart
// once more". Verified from `rlwinm. r0,r0,0,22,22` at 801b0b60 (MB=22,ME=22 =
// single bit at position 22 = 0x200).
constexpr std::uint32_t kMapObjFlagBit_ForceClearGate = 0x200;

// Value written to the outgoing MActorAnmBase's unk0 when we clear it (matches
// makeObjDead's `mMActor->getUnk28(uVar6)->unk0 = 0xffffffff`).
constexpr std::int32_t kMActorAnmReset = -1;

// -- Predicates --

// Fencepost: startAnim proceeds ONLY when `count > idx` (u16 compare).
// Disasm uses `cmpl` (unsigned) at 801b0a5c with `bgt` — so count MUST be
// strictly greater than idx. A relaxed `count >= idx` would OOB-read entry
// at index == count.
constexpr bool has_slot(std::uint16_t count, std::uint16_t idx)
{
	return count > idx;
}

// Sentinel test — reused for both "no previous slot" (skip outgoing reset)
// and "no incoming state to cancel".
constexpr bool is_sentinel(std::uint16_t slot) { return slot == kSentinelSlot; }

// Should we lazily create mMActor from anim entry [0]? Yes iff mMActor is
// currently null AND there is at least one entry. Disasm at 801b0a10-40:
// `if (mMActor == null && anim->count != 0) mMActor = keeper->getMActor(anim[0].unk0);`
constexpr bool should_lazy_load_mactor(bool have_mactor, std::uint16_t count)
{
	return !have_mactor && count > 0;
}

// Should we skip the whole slot-changed reload path? Yes iff we're already
// on this slot. Disasm at 801b0b0c-14: `if (idx == unkFE) skip reload`.
constexpr bool skip_reload(std::uint16_t prev, std::uint16_t idx)
{
	return prev == idx;
}

// Native compat predicate (2026-07-02, [[fileselect-startanim-mtxcalc-null]]):
// The "no anim name" tail branch (@0x801b0b8c..0ba4) writes
//   *(mMActor->unk8 + 0x58) = mMActor->model->modelData->mJointNodePointer[0]
// The RE reads mMActor->unk8 (J3DMtxCalc*) and dereferences it with no null
// guard — hardware never trips it because MActor::setModel (populates unk8)
// runs before any startAnim path with a null anim entry. Native's
// TFileLoadBlock::makeBlockNormal → startAnim(0) fires before setModel does,
// so unk8 is null and the raw RE write SEGVs. `can_reset_mtxcalc` is the
// safety-net predicate the port uses in the else branch — it returns TRUE
// only when all three pointers along the write path are non-null. Returning
// FALSE means "skip the reset entirely" (this call path is "clear the
// currently-playing anim"; with no bound MtxCalc there's nothing to reset).
//
// The RE-faithful side of the branch (all three pointers non-null) is
// unchanged; this predicate ONLY gates the null-safety escape hatch.
constexpr bool can_reset_mtxcalc(bool has_mtx_calc, bool has_model_data,
                                 bool has_root_joint)
{
	return has_mtx_calc && has_model_data && has_root_joint;
}

} // namespace sb::startanim

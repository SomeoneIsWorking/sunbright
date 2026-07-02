// sms_boot_itemnozzle.h — pure specs for three small ports:
//
//   * TItemNozzle::put       (@0x801bbcf4, 24 bytes / 6 insns)
//   * TItemNozzle::appearing (@0x801bbc0c, 36 bytes / 9 insns)
//   * TNozzleBox::control    (@0x801bb674, 88 bytes / 22 insns, chains base)
//
// Regression targets:
//   * PPC bit-mask direction: LIVE_FLAG_UNK10 == 0x10 (rlwinm 0,27,27 must
//     produce mask 0x00000010, NOT 0x08000000).
//   * `unk64 &= ~1u` — clear ONLY the LSB (rlwinm 0,0,30 = mask 0..30 kept).
//   * 3-guard order in NozzleBox::control (state == 4 → return before
//     inspecting unk166 / mColCount).

#pragma once
#include <cstdint>

namespace sb {

// Bit constants — must match Strategic/LiveActor.hpp exactly.
inline constexpr std::uint32_t kLiveFlagUnk10 = 0x10u;
inline constexpr std::uint32_t kHitActorUnk64Bit0 = 0x1u;

struct ItemNozzleFields {
	std::uint32_t unk64;        // THitActor::unk64
	std::uint32_t live_flag;    // TLiveActor::mLiveFlag  (only appearing reads)
	std::uint16_t state;        // TMapObjBase::mState
};

// put(): unconditional. Clears unk64 LSB, sets state = 1.
inline void item_nozzle_put(ItemNozzleFields& s)
{
	s.unk64 &= ~kHitActorUnk64Bit0;
	s.state = 1;
}

// appearing(): guarded by (mLiveFlag & 0x10). Same mutations as put().
inline void item_nozzle_appearing(ItemNozzleFields& s)
{
	if ((s.live_flag & kLiveFlagUnk10) == 0)
		return;
	s.state = 1;
	s.unk64 &= ~kHitActorUnk64Bit0;
}

struct NozzleBoxFields {
	std::int32_t  unk148;       // sentinel value 4 short-circuits
	std::uint16_t col_count;    // THitActor::mColCount
	std::uint8_t  unk166;       // pending-latch byte we may clear
};

// control() body AFTER the delegation to TMapObjGeneral::control().
// Guards in exact ROM order.
inline void nozzle_box_control_tail(NozzleBoxFields& s)
{
	if (s.unk148 == 4)
		return;
	if (s.unk166 == 0)
		return;
	if (s.col_count != 0)
		return;
	s.unk166 = 0;
}

}  // namespace sb

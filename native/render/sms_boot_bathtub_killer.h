// sms_boot_bathtub_killer.h — pure spec for the
// TBathtubKillerManager::countActiveKillers port (@0x8012f204).
//
// The manager walks its obj-array up to a params-clamped limit and counts
// entries whose TLiveActor::mLiveFlag has bit 0 (LIVE_FLAG_DEAD) unset.
// Extracted so the loop invariants can be pinned by unit tests without
// dragging TSpineEnemy / TLiveManager into the harness.

#pragma once
#include <cstdint>

namespace sb::bathtub_killer {

// LIVE_FLAG_DEAD — bit 0 of TLiveActor::mLiveFlag.
inline constexpr std::uint32_t kLiveFlagDead = 0x1u;

// Compute the per-iteration limit used inside the RE loop:
//   if (params_ptr != nullptr)
//     limit = min(obj_num, params_active_enemy_num)
//   else
//     limit = obj_num
// `has_params` mirrors "unk38 != nullptr". `params_active` is the value read
// from `unk38->mSLActiveEnemyNum.get()` (u8 -> int) — 0..255.
inline int clamp_limit(int obj_num, bool has_params, int params_active)
{
    if (!has_params) return obj_num;
    return (params_active <= obj_num) ? params_active : obj_num;
}

// Pure counter: given the loop's per-iteration limit and a callable
// `is_dead(int i)` predicate, count the entries where the predicate is FALSE.
// The callable is a stand-in for the runtime `getObj(i)->mLiveFlag & 1`.
template <typename IsDead>
int count_active(int obj_num, bool has_params, int params_active, IsDead is_dead)
{
    int count = 0;
    for (int i = 0;; ++i) {
        int limit = clamp_limit(obj_num, has_params, params_active);
        if (i >= limit) break;
        if (!is_dead(i)) ++count;
    }
    return count;
}

}  // namespace sb::bathtub_killer

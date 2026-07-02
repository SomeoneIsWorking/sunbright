// sms_boot_bathtub.h — spec constants for TBathtub::loadAfter's four particle
// registrations (@0x801fb894). Corona Mountain's Bowser-arena bathtub: steam,
// fountain, and two break-piece variants. Each pair is byte-verified against the
// DOL rodata strings and the JPAResourceManager IDs from the disasm.

#pragma once
#include <cstdint>

namespace sb::bathtub_load_after {

inline constexpr const char* kSteamPath      = "/scene/map/map/ms_lkp_yuge1.jpa";
inline constexpr const char* kFountainPath   = "/scene/map/map/ms_kp_funsui.jpa";
inline constexpr const char* kBreakAPath     = "/scene/map/map/ms_kp_break_a.jpa";
inline constexpr const char* kBreakBPath     = "/scene/map/map/ms_kp_break_b.jpa";

inline constexpr std::uint16_t kSteamId      = 0x1beu;
inline constexpr std::uint16_t kFountainId   = 0x1bfu;
inline constexpr std::uint16_t kBreakAId     = 0xf6u;
inline constexpr std::uint16_t kBreakBId     = 0xf7u;

// Invariant asserted by the RE's byte layout: the two "kp" (Koopa) IDs at 0xf6/0xf7
// are consecutive, and the two Steam/Fountain IDs at 0x1be/0x1bf are consecutive.
static_assert(kFountainId == kSteamId + 1, "fountain id follows steam id");
static_assert(kBreakBId   == kBreakAId + 1, "break_b id follows break_a id");

}  // namespace sb::bathtub_load_after

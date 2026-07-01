// sms_boot_amiking.h — spec-derived constants for TAmiKing::loadAfter (@0x801d48e0).
// One particle .jpa is registered per scene, keyed to a specific ID that the boss's own
// spawn code references at runtime. Path + ID pair are both spec — a typo in either would
// silently miss the resource load and the boss would spawn without its effect.

#pragma once
#include <cstdint>

namespace sb {

// Ami King's boss-effect particle path and id (matches DOL @0x803920b4 / arg 0x184).
constexpr const char*   kAmiKingParticlePath = "/scene/mapObj/amiking.jpa";
constexpr std::uint16_t kAmiKingParticleId   = 0x184;

}  // namespace sb

// sms_boot_coverfruit.h — spec-derived constants for TCoverFruit::loadAfter (@0x801e1748).
// フタのフルーツ ("lid fruit") — a fruit hidden under a lid; once collected in a given save
// slot it must not respawn. loadAfter queries TFlagManager for the "already collected"
// boolean under a specific flag id; if set, the object is killed at load time via
// makeObjDead(). This header pins the flag id (from `lis r4, 1; addi r4, r4, 0x38B`).

#pragma once
#include <cstdint>

namespace sb {

// TFlagManager boolean flag id checked at TCoverFruit::loadAfter — set by the fruit's own
// collection handler and persisted in the save block. A typo (0x1038A / 0x1038C — both are
// adjacent, actively used flag ids) would silently make the fruit either always-dead
// (wrong flag always set) or always-alive (wrong flag never set).
constexpr std::uint32_t kCoverFruitCollectedFlag = 0x1038Bu;

}  // namespace sb

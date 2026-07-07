// sms_boot_jumpmushroom.h — pure spec for the TJumpMushroom::load port (@0x801f57a0).
// ジャンプきのこ (Delfino / Pianta Village "jump mushroom" trampolines). The scene stream
// stores the collision-data id in a 4-byte aligned record but only the LOW 16 bits (the
// last 2 stream bytes, in big-endian) are the actual signed s16 id passed to
// TMapCollisionBase::setAllData(s16). The top 2 bytes are unused padding.
//
// Extracting this into a pure helper catches two common regressions:
//   1. Wrong-width read (`stream.readS16()` alone would consume 2 bytes, desyncing every
//      later field in the record).
//   2. Wrong endianness (on our LE host a naive `*(s16*)buf` would sample the padding).

#pragma once
#include <cstdint>

namespace sb {

// Given the 4-byte record read from the stream as a numeric BE-interpreted u32 (i.e. the
// value `JSUInputStream::readU32()` returns), return the collision-data id in the low
// 16 bits, sign-extended per PPC `extsh`.
inline std::int16_t jump_mushroom_collision_id_from_serialized(std::uint32_t raw)
{
	return static_cast<std::int16_t>(raw & 0xFFFFu);
}

}  // namespace sb

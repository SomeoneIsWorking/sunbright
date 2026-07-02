// sms_boot_shine.h — pure spec-constants for TShine::initMapObj (@0x801bcd70).
// The port is a fixed-init function chaining TMapObjGeneral::initMapObj and
// seeding 10 per-instance fields. Pinning the constants catches any transcription
// error (0x1e0 typo'd as 0x1d0, off-by-one field offset, u32→u16 write, etc.).

#pragma once
#include <cstdint>

namespace sb::shine_init {

// Field seeds. Byte-verified against the RE.
inline constexpr std::uint32_t kUnk14C      = 0x1e0u;  // 480
inline constexpr std::uint32_t kUnk150      = 0x78u;   // 120
inline constexpr std::uint8_t  kUnk1A4      = 0u;
inline constexpr float         kUnk1A8      = 0.0f;
inline constexpr float         kUnk1AC      = 0.0f;
inline constexpr float         kUnk1B0      = 0.0f;
inline constexpr std::uint32_t kUnk170      = 0xf0u;   // 240
inline constexpr std::uint32_t kUnk174      = 0u;
inline constexpr std::uint32_t kUnk178      = 0xf0u;   // 240

// Also: unk170 must equal unk178, and both must equal 0xf0.
static_assert(kUnk170 == kUnk178, "unk170 and unk178 seed to the same value in RE");
static_assert(kUnk170 == 0xf0u,   "the shared unk170/178 seed is exactly 0xf0");
static_assert(kUnk14C == 480u,    "unk14C decimal check");
static_assert(kUnk150 == 120u,    "unk150 decimal check");

}  // namespace sb::shine_init

namespace sb::shine_load_after {

// Post-base-chain effect of TShine::loadAfter (@0x801bcd08). Dispatch on
// unk154: 2 → wait-timer branch, 1 → self-kill branch (virtual makeObjDead),
// otherwise → no-op. Pure decision, so the effect enum is trivially testable
// with no field snapshots.
enum class Effect { NoOp, WaitTimerAndSet, MakeObjDead };

inline constexpr std::uint32_t kUnk154Wait     = 2;
inline constexpr std::uint32_t kUnk154Kill     = 1;
inline constexpr std::uint32_t kWaitFrames     = 0xf0u;  // 240
inline constexpr std::uint16_t kWaitState      = 0x12u;  // 18

inline Effect classify(std::uint32_t unk154)
{
	if (unk154 == kUnk154Wait) return Effect::WaitTimerAndSet;
	if (unk154 == kUnk154Kill) return Effect::MakeObjDead;
	return Effect::NoOp;
}

}  // namespace sb::shine_load_after

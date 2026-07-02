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

namespace sb::shine_load_before_init {

// Pure predicates from TShine::loadBeforeInit (@0x801bcc18).

// Name-tag → unk154 mapping. Byte-verified string literals; do NOT lower-case
// or trim on entry — the RE calls strcmp verbatim.
inline std::uint32_t name_to_unk154(const char* name)
{
	if (name == nullptr) return 1;
	// Manual strcmp to keep the header header-only + freestanding.
	auto streq = [](const char* a, const char* b) {
		while (*a && *b && *a == *b) { ++a; ++b; }
		return *a == *b;
	};
	if (streq(name, "normal"))  return 0;
	if (streq(name, "quickly")) return 2;
	return 1;
}

// Default wait-time when stream reads -1.
inline constexpr int kDefaultWait = 0x78;  // 120

// s32 wait-time → clamped to 0x78 if -1, otherwise passthrough.
inline int clamp_wait(int wait) { return (wait == -1) ? kDefaultWait : wait; }

// Signed toggle → u8 unk190 via the (val+1)<2 clamp.
// Effect: toggle == 0 → 1; toggle == -1 → 0; anything else → 0.
inline std::uint8_t clamp_toggle(int toggle)
{
	if (toggle + 1 >= 2)
		toggle = -1;
	return static_cast<std::uint8_t>(toggle + 1);
}

}  // namespace sb::shine_load_before_init

namespace sb::shine_make_mactors {

// Pure decision spec for TShine::makeMActors (@0x801bcdd4). After allocating
// the TMActorKeeper (which the port does by calling the engine primitive),
// the function chooses ONE of two bmd names to hand to initMActor based on:
//   (1) whether TFlagManager marks this shine's ID as already collected, AND
//   (2) whether this instance's name is the Mani-shop variant.
// Any other case (flag unset, or set but name mismatch) → "shine.bmd".
// The 「シャイン（マニ屋用）」 name is the Mani-shop's placeholder shine that
// shows an empty pedestal after the mission has been cleared once.
//
// Regressions this catches:
//   * Missing the AND (falling back to shine.bmd on flag-only) — the Mani-shop
//     display would keep showing a full Shine after collection.
//   * Reversing the flag polarity → shine_empty.bmd shown on every OTHER shine
//     that hasn't been collected yet.
//   * Trimming/lowercasing the name check — strcmp against the SJIS-encoded
//     literal is exact-match, per the RE.

inline constexpr const char* kShineNormalBmd = "shine.bmd";
inline constexpr const char* kShineEmptyBmd  = "shine_empty.bmd";
inline constexpr const char* kManiShopName   = "シャイン（マニ屋用）";

inline constexpr std::uint32_t kKeeperModelLoaderFlags = 0x10220000u;

// After-decision unk1B4 latch: set to 1 only in the empty-pedestal branch.
inline constexpr std::uint8_t kUnk1B4EmptyBranch = 1;

// Choose which bmd to load — returns the pointer to one of the constants above.
// Manual strcmp keeps the header freestanding (matches the rest of the file).
inline const char* choose_bmd(bool shine_flag_set, const char* actor_name)
{
    if (!shine_flag_set || actor_name == nullptr)
        return kShineNormalBmd;
    // Byte-exact compare against the SJIS bytes of the Mani-shop variant.
    const char* a = actor_name;
    const char* b = kManiShopName;
    while (*a && *b && *a == *b) { ++a; ++b; }
    if (*a == 0 && *b == 0) return kShineEmptyBmd;
    return kShineNormalBmd;
}

// True when the port should latch unk1B4 = 1 (empty-pedestal branch).
inline bool should_latch_empty(bool shine_flag_set, const char* actor_name)
{
    return choose_bmd(shine_flag_set, actor_name) == kShineEmptyBmd;
}

}  // namespace sb::shine_make_mactors

// platform_types.h — shared scalar/result types for the native platform seams.
//
// The game's C++ (reference/sms) uses the GC SDK's <dolphin/types.h> aliases
// (u8/u16/u32/s8/.../f32/BOOL). The native build keeps the SAME aliases so the
// game source compiles unchanged; this header is the native definition of them
// plus the small set of cross-seam helpers. It is deliberately HW-agnostic and
// architecture-independent (no x86/arm specifics).
//
// Phase-2 note: include <dolphin/types.h> from the read-only reference tree if it
// already defines these for the native target; otherwise this is the source of
// truth. Do NOT duplicate-define — pick one. (Lead decides at integration.)
#pragma once
#include <cstdint>
#include <cstddef>

namespace sb::platform {

using u8  = std::uint8_t;   using s8  = std::int8_t;
using u16 = std::uint16_t;  using s16 = std::int16_t;
using u32 = std::uint32_t;  using s32 = std::int32_t;
using u64 = std::uint64_t;  using s64 = std::int64_t;
using f32 = float;          using f64 = double;

// Generic seam result. SDK functions return assorted ints/BOOLs; for new native
// entry points prefer this. Map existing SDK signatures' return values per-seam.
enum class Result : s32 {
    Ok          = 0,
    NotReady    = -1,
    NotFound    = -2,
    Busy        = -3,
    IoError     = -4,
    Unsupported = -5,
};

} // namespace sb::platform

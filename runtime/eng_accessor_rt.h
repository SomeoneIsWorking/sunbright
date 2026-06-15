#pragma once
// =============================================================================
// Tailored-recomp engine ACCESSOR runtime shim (docs/re_notes/j3d_subsystem_ownership_plan.md
// STEP 0, Option A). The generated `generated/eng_accessors.cpp` is the ONE translation unit
// that bakes host-struct field offsets / ctor sizes — it #includes the pristine decomp engine
// headers (host C++ types). Those headers define their own u8/u16/u32/u64/f32 typedefs
// (decomp `u64` == `unsigned long long`), which CONFLICT with runtime/cpu_state.h
// (`u64` == `uint64_t` == `unsigned long` on LP64) — so the accessor TU must NOT include
// cpu_state.h / intrinsics.h. This header gives it just the boundary runtime entry points it
// needs, declared with explicit <cstdint> widths so they link against eng_handle.cpp /
// memory_bridge.cpp regardless of the decomp typedef world.
//
// Portable C++17, no host-arch / endianness assumptions (arm64-clean).
// =============================================================================
#include <cstdint>
#include <cstddef>
#include <new>

// Engine-object handle table (runtime/eng_handle.cpp). A recompiled-game register holds a
// 32-bit HANDLE for a host-native engine object; the accessor maps it to the host pointer.
extern std::uint32_t sb_eng_handle(void* host);
extern void*         sb_eng_host(std::uint32_t handle);
extern void          sb_eng_release(void* host);

// Guest-data pointer translation (runtime/memory_bridge.cpp). A host engine object may hold a
// POINTER FIELD into guest RAM (e.g. JUTTexture::mTexInfo = const ResTIMG*); the game reads/
// writes a 32-bit guest ADDRESS, so the boundary translates host<->guest rather than truncating
// an 8-byte host pointer.
extern void*         sb_guest_to_host(std::uint32_t ea);
extern std::uint32_t sb_host_to_guest(void* host);

// Store a 32-bit guest address into a guest-data pointer field, translating to a host pointer.
// Deduces the member's pointer type so the assignment stays type-correct (incl. const pointees)
// without the accessor carrying the type name. Header-local copy of the intrinsics.h template
// (which can't be pulled in here — it includes cpu_state.h).
template <class P> inline void sb_set_guest_ptr(P*& field, std::uint32_t ea) {
    field = static_cast<P*>(sb_guest_to_host(ea));
}

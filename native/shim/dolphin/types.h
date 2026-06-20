// Native (host) replacement for the GC SDK <dolphin/types.h>. The decomp's original typedefs s32/u32
// to `long`, which is 64-bit under LP64 — wrong size for every struct field. We target a 64-bit native
// build with FIXED-WIDTH integer types (clean host layout; 64-bit pointers are fine — the engine owns
// allocation and asset loaders convert on-disc 32-bit BE offsets to native pointers at load time).
#ifndef _DOLPHIN_TYPES_H_
#define _DOLPHIN_TYPES_H_
#include <cstdint>
typedef int8_t   s8;   typedef uint8_t  u8;
typedef int16_t  s16;  typedef uint16_t u16;
typedef int32_t  s32;  typedef uint32_t u32;
typedef int64_t  s64;  typedef uint64_t u64;
typedef float f32;     typedef double f64;
typedef char* Ptr;
typedef int BOOL;
#define FALSE 0
#define TRUE  1
// GC fixed-address globals (`u32 __OSBusClock AT_ADDRESS(...)`) had ONE physical
// location at a hardware address via the linker script. On host there's no such
// placement, and the empty expansion made every including TU emit its own .bss
// definition -> "multiple definition" at link. Mark them WEAK so the linker merges
// all definitions to one (zero-initialized; the OS seam sets real values at init).
#define AT_ADDRESS(addr) __attribute__((weak))
// Marker: this is the native PC build (the host shim is on the include path). Game
// code that genuinely depends on GC hardware (e.g. reads of physical low-memory at
// OSPhysicalToCached(0)) guards on this to take the host-native path instead.
#define SMS_NATIVE_PLATFORM 1
#ifdef __cplusplus
typedef bool   __bool;
#endif
#endif

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
#define AT_ADDRESS(addr)
#ifdef __cplusplus
typedef bool   __bool;
#endif
#endif

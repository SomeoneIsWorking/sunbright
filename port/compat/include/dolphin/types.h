#ifndef _DOLPHIN_TYPES_H_
#define _DOLPHIN_TYPES_H_

// ---------------------------------------------------------------------------
// SHADOW of reference/sms/include/dolphin/types.h for the NATIVE x86-64 port.
//
// This file is placed EARLIER on the -I path than reference/sms/include so it
// wins the `#include <dolphin/types.h>` resolution. The vendored decomp stays
// pristine; we override by shadowing, never by editing it.
//
// THE ONE REQUIRED CHANGE vs the original: the GameCube was a 32-bit ILP32
// big-endian target where `long` is 4 bytes, so the decomp typedef'd
//     s32/u32 as signed/unsigned long.
// On x86-64 (LP64) `long` is 8 bytes, which silently doubles the width of
// every u32/s32 field — corrupting struct layouts, sizeof, masks, and
// printf. We typedef them as `int` (always 4 bytes on x86-64) to restore the
// intended 32-bit semantics. Everything else mirrors the original.
// ---------------------------------------------------------------------------

typedef signed char s8;
typedef unsigned char u8;
typedef signed short int s16;
typedef unsigned short int u16;
typedef signed int s32;           // was `signed long`  — 4 bytes on x86-64
typedef unsigned int u32;         // was `unsigned long`— 4 bytes on x86-64
typedef signed long long int s64;
typedef unsigned long long int u64;

typedef float f32;
typedef double f64;

typedef char* Ptr;

typedef int BOOL;

#define FALSE 0
#define TRUE  1

// AT_ADDRESS: the decomp used CodeWarrior's `: (addr)` placement to pin
// hardware register globals at fixed guest addresses. There is no native
// equivalent (and the core lib does not touch hardware), so it is a no-op.
#if defined(__MWERKS__)
#define AT_ADDRESS(addr) : (addr)
#else
#define AT_ADDRESS(addr)
#endif

#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))

#ifndef NULL
#define NULL ((void*)0)
#endif

#endif

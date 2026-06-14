#pragma once
#include "cpu_state.h"

// =============================================================================
// runtime/eng_handle.h — the engine-object HANDLE table (tailored-recomp
// boundary, docs/ARCHITECTURE_TARGET.md, data/field-access half).
//
// In the target architecture the PC-native engine objects (port/ JSystem: J3D,
// J2D, JKR, JUTTexture, …) live in host memory with host pointers (8 bytes on a
// 64-bit host). Recompiled game code keeps a 32-bit register file and stores
// engine-object pointers in guest RAM as 32-bit values, so it cannot hold a raw
// host pointer. Instead it holds a 32-bit HANDLE.
//
//   sb_eng_handle(host)  -> stable 32-bit token for a host object (0 == null)
//   sb_eng_host(token)   -> the host pointer (nullptr for 0 or a stale token)
//   sb_eng_release(host) -> invalidate the token when the object is freed
//
// The recompiler emits sb_eng_host() at every TYPED field-access site and
// sb_eng_handle()/sb_eng_host() when an engine pointer crosses the boundary
// (return value of a bridged ctor/getter, a stored engine-pointer field). The
// token encoding is opaque to the generated code.
//
// Token encoding (defensive): a token is kEngHandleBase | index, with
// kEngHandleBase chosen in an UNMAPPED guest address range (0x9xxxxxxx — between
// main RAM 0x8xxxxxxx and MMIO 0xCCxxxxxx). So if a recovery MISS ever lets a
// handle reach a guest MEM_* access (a correctness bug, per ARCHITECTURE_TARGET),
// it faults LOUDLY in the MMU path instead of silently corrupting RAM.
// =============================================================================

constexpr u32 kEngHandleBase = 0x90000000u;
constexpr u32 kEngHandleMask = 0x0FFFFFFFu;  // index bits

// host pointer -> stable 32-bit handle. nullptr -> 0. Idempotent: the same host
// pointer always maps to the same handle until it is released.
u32 sb_eng_handle(void* host);

// 32-bit handle -> host pointer. 0 -> nullptr. A stale / non-handle token -> nullptr.
void* sb_eng_host(u32 handle);

// Invalidate the handle for a freed host object and recycle its slot. Safe to
// call with a pointer that was never handled (no-op). Call this from the bridged
// destructor / operator delete of an engine type.
void sb_eng_release(void* host);

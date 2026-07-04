// mtx_seam.h — native reimplementation of the GC MTX / PSMTX math library.
//
// SDK header replaced: reference/sms/include/dolphin/mtx.h
// Used surface: MTX* 11 distinct / 282 calls, PSMTX* 15 distinct / 53 calls.
// Hot: MTXConcat(96), MTXCopy(59), MTXMultVec(47), MTXIdentity(41), PSMTXCopy/Concat.
//
// PURE MATH, NO HARDWARE — the lowest-risk, fully unit-testable seam. PSMTX* are
// the Gekko paired-single (SIMD) variants of the MTX* ops; at f32 precision on a
// host they compute the same result, so PSMTX* may simply forward to MTX*.
//
// Matrices are 3x4 row-major (Mtx = f32[3][4]) and 4x4 (Mtx44) per the SDK. Keep
// the exact element order so the game's stored matrices stay valid. The existing
// renderer math (runtime/ngx/ngx_project.h) is reference for the projection ops.
//
// Architecture-independent: write portable scalar C++ (the compiler autovectorizes);
// do NOT hand-roll x86 intrinsics. A faithful scalar impl is the correctness oracle.
#pragma once
#include "platform_types.h"

namespace sb::platform::mtx {

using Mtx   = f32[3][4];   // 3x4 affine (SDK Mtx)
using Mtx44 = f32[4][4];   // 4x4 (SDK Mtx44, used for projection)
struct Vec  { f32 x, y, z; };

// ---- core (mtx.h) — all TODO phase-2, pure scalar impls ------------------
void Identity(Mtx m);                              // MTXIdentity
void Copy(const Mtx src, Mtx dst);                 // MTXCopy
void Concat(const Mtx a, const Mtx b, Mtx ab);     // MTXConcat (ab = a*b)
void Trans(Mtx m, f32 x, f32 y, f32 z);            // MTXTrans
void Scale(Mtx m, f32 x, f32 y, f32 z);            // MTXScale
void RotRad(Mtx m, char axis, f32 rad);            // MTXRotRad
u32  Inverse(const Mtx src, Mtx inv);              // MTXInverse (returns 0 if singular)
void MultVec(const Mtx m, const Vec* in, Vec* out);            // MTXMultVec
void MultVecSR(const Mtx m, const Vec* in, Vec* out);          // MTXMultVecSR (no translate)
void MultVecArray(const Mtx m, const Vec* in, Vec* out, u32 n);// MTXMultVecArray

// ---- paired-single variants (PSMTX*) ------------------------------------
// Same semantics; forward to the MTX* impls unless a perf profile says otherwise.
// PSMTXIdentity/Copy/Concat/Trans/Scale/RotRad/RotTrig/RotAxisRad/Quat/Inverse/
// MultVec/MultVecSR/MultVecArray/ScaleApply/TransApply. TODO phase-2 (thin forwards).

} // namespace sb::platform::mtx

#ifndef SMSPORT_COMPAT_MSL_SHIM_H_
#define SMSPORT_COMPAT_MSL_SHIM_H_

// ===========================================================================
// MetroWerks Standard Library (MSL) macro shim for the native port.
//
// The vendored decomp ships CodeWarrior's MSL libc headers under
//   reference/sms/include/PowerPC_EABI_Support/Msl/...
// Those headers MUST stay off the system include path: they re-declare the
// C standard library (math.h, stdlib.h, string.h, ...) in ways that clash
// with / shadow the host glibc/libstdc++ and won't compile natively.
//
// But the game *uses* a few non-standard macros and constants that MSL's
// <math.h> defines (DEG_TO_RAD, TAU, HALF_PI, M_PI as a float, ...). We
// provide those host-safe definitions here, on top of the real system libc.
//
// This header is force-included after compat/intrinsics.h.
// ===========================================================================

#include <cmath>    // real system <math.h> / libstdc++ — the libc we DO want
// NOTE: deliberately do NOT pull <cstdio> here. <cstdio> defines EOF as a
// macro (-1), but the decomp uses EOF as an enumerator
// (JSUStreamEnum.hpp: `enum EIoState { GOOD, EOF }`), which a global cstdio
// would break with "expected identifier before '(' token". stdio/stdarg are
// instead provided to the few TUs that need them via shadow <stdio.h> /
// <stdarg.h> headers under port/compat/include (those forward to system libc
// only where the source explicitly asks for them).

// M_PI: glibc's <math.h> defines this as a double under _USE_MATH_DEFINES /
// _GNU_SOURCE. MSL defined it as a FLOAT literal (3.14159265358979323846f),
// and the decomp relies on float-typed M_PI in several expressions. Force the
// MSL value/type so single-precision math matches the original.
#ifdef M_PI
#undef M_PI
#endif
#define M_PI 3.14159265358979323846f

// Angle / trig constants from MSL <math.h>.
#ifndef TAU
#define TAU 6.2831855f
#endif
#ifndef LONG_TAU
#define LONG_TAU 6.2831854820251465
#endif
#ifndef HALF_PI
#define HALF_PI 1.5707964f
#endif
#ifndef THIRD_PI
#define THIRD_PI 1.0471976f
#endif
#ifndef QUARTER_PI
#define QUARTER_PI 0.7853982f
#endif
#ifndef SIN_2_5
#define SIN_2_5 0.43633234f
#endif
#ifndef M_SQRT3
#define M_SQRT3 1.73205f
#endif

// Degree<->radian helpers (MSL <math.h>). RAD_TO_DEG keeps the decomp's
// fakematch +0.000005f term so results are bit-identical to the original.
#ifndef DEG_TO_RAD
#define DEG_TO_RAD(degrees) ((degrees) * (M_PI / 180.0f))
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG(radians) ((radians) * (180.0f / M_PI + 0.000005f))
#endif

#endif // SMSPORT_COMPAT_MSL_SHIM_H_

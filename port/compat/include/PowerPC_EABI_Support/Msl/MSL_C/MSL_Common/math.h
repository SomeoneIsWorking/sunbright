#ifndef SMSPORT_SHADOW_MSL_MATH_H_
#define SMSPORT_SHADOW_MSL_MATH_H_

// ===========================================================================
// SHADOW of reference/sms/include/PowerPC_EABI_Support/Msl/MSL_C/MSL_Common/math.h
//
// A few decomp sources include the MSL <math.h> by its full relative path,
// e.g. JMath.cpp:
//     #include <PowerPC_EABI_Support/Msl/MSL_C/MSL_Common/math.h>
// Since the real Msl tree is kept OFF the include path (it shadows system
// libc), that include would fail. We satisfy it here by routing to the host
// libc and the MSL macro shim, plus the float-domain helpers (sqrt/sqrtf)
// that the original MSL <math.h> defined inline and the decomp depends on.
//
// This shadow lives under port/compat/include, which is EARLIER on the -I
// path than reference/sms/include, so this file wins the resolution while the
// real MSL tree remains unreachable.
// ===========================================================================

#include <cmath>              // real system math
#include "msl_shim_macros.h"  // DEG_TO_RAD/TAU/M_PI etc. (relative, see below)

// __frsqrte/__fres come from the force-included compat/intrinsics.h.

#ifdef __cplusplus
extern "C" {
#endif

// MSL exported these float-domain wrappers; map onto system libc. Declared
// here only to satisfy any TU that includes this header expecting them.
#ifndef NAN
#define NAN (__builtin_nanf(""))
#endif
#ifndef HUGE_VALF
#define HUGE_VALF (__builtin_huge_valf())
#endif

#ifdef __cplusplus
}
#endif

#endif // SMSPORT_SHADOW_MSL_MATH_H_

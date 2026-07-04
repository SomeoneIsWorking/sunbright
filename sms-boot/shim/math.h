// Host-libc forwarding shim for the decomp's `#include <math.h>`.
//
// In the original GameCube build `<math.h>` resolves to Metrowerks MSL's
// PowerPC_EABI_Support/.../MSL_C/MSL_Common/math.h, which (besides the standard
// libm prototypes) defines a set of SMS/MSL-specific constants and macros the
// game code uses directly: DEG_TO_RAD/RAD_TO_DEG, TAU/HALF_PI/THIRD_PI/..., and
// M_PI *as a float*. glibc's <math.h> has none of the MSL extras (and defines
// M_PI only as a double, under _USE_MATH_DEFINES). So we shadow <math.h>: pull
// glibc's real libm prototypes via #include_next, then add the MSL macros.
//
// Values copied verbatim from the MSL header
// (PowerPC_EABI_Support/Msl/MSL_C/MSL_Common/math.h) to stay bit-faithful;
// NAN/HUGE_VALF are intentionally NOT redefined (MSL's reference MSL internals;
// keep glibc's).
#ifndef SUNBRIGHT_SHIM_MATH_H
#define SUNBRIGHT_SHIM_MATH_H

#include_next <math.h>

#define LONG_TAU   6.2831854820251465
#define TAU        6.2831855f
#define HALF_PI    1.5707964f
#define THIRD_PI   1.0471976f
#define QUARTER_PI 0.7853982f
#define SIN_2_5    0.43633234f
#define M_SQRT3    1.73205f

// MSL defines M_PI as a float literal; glibc may define it as a double. Mirror
// MSL exactly so DEG_TO_RAD/RAD_TO_DEG evaluate in float as the game expects.
#ifdef M_PI
#undef M_PI
#endif
#define M_PI 3.14159265358979323846f

#define DEG_TO_RAD(degrees) (degrees * (M_PI / 180.0f))
#define RAD_TO_DEG(radians)                                                    \
	(radians                                                                   \
	 * (180.0f / M_PI + 0.000005f)) // the 0.000005f is probably a fakematch

#endif // SUNBRIGHT_SHIM_MATH_H

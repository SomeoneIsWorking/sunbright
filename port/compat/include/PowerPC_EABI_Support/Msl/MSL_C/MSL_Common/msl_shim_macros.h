#ifndef SMSPORT_SHADOW_MSL_MACROS_H_
#define SMSPORT_SHADOW_MSL_MACROS_H_

// MSL <math.h> macros needed by decomp sources that include MSL math.h
// directly. Kept in sync with port/compat/msl_shim.h (the force-included copy).

#ifdef M_PI
#undef M_PI
#endif
#define M_PI 3.14159265358979323846f

#ifndef TAU
#define TAU 6.2831855f
#endif
#ifndef HALF_PI
#define HALF_PI 1.5707964f
#endif
#ifndef DEG_TO_RAD
#define DEG_TO_RAD(degrees) ((degrees) * (M_PI / 180.0f))
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG(radians) ((radians) * (180.0f / M_PI + 0.000005f))
#endif

#endif // SMSPORT_SHADOW_MSL_MACROS_H_

// Host implementations of the Gekko/PowerPC compiler intrinsics the SMS decomp uses in inline math.
// Force-included into every native-engine TU (-include). The GameCube CPU's estimate ops (frsqrte/fres)
// are replaced with EXACT host math — a native port may be *more* precise than HW; the game's
// Newton-Raphson refiners just converge faster. CRT/runtime internals (__init_cpp, __pformatter, …)
// are intentionally NOT here — those belong to the boot/CRT modules, which the native platform replaces.
// Signatures match the decomp's bundled MSL <math.h> (extern "C", double-typed) so both can coexist.
#ifndef SB_GEKKO_INTRINSICS_H
#define SB_GEKKO_INTRINSICS_H
#include <cstring>

#ifndef __declspec
#define __declspec(x)
#endif

extern "C" {
inline double __frsqrte(double v) { return 1.0 / __builtin_sqrt(v); }
inline float  __fres(float v)     { return 1.0f / v; }
inline double __fabs(double v)    { return __builtin_fabs(v); }
inline float  __fabsf(float v)    { return __builtin_fabsf(v); }
inline int    __cntlzw(unsigned int v) { return v ? __builtin_clz(v) : 32; }
inline void   __sync(void) { __sync_synchronize(); }
inline void   __dcbz(void* p, int off) { std::memset((char*)p + off, 0, 32); }
}
#endif

// Host-libc forwarding shim for the decomp's `#include <printf.h>`.
//
// In the original GameCube build this resolves to Metrowerks MSL's
// PowerPC_EABI_Support/.../MSL_C/MSL_Common/printf.h, which declares the printf
// family (printf/sprintf/snprintf/vsprintf/vsnprintf) and pulls in <stdarg.h>.
// Crucially MSL's <printf.h> carries the printf decls but NOT the <stdio.h>
// object macros (EOF, ...).
//
// On the host include path a bare `<printf.h>` instead resolves to glibc's
// /usr/include/printf.h (GNU register_printf_* extensions) which does NOT
// declare snprintf/vsnprintf at all -> game code (M3DUtil/MActorData.hpp,
// J2D/J2DPrint.cpp) fails with "snprintf must be available".
//
// Forward to our <stdio.h> shim: it pulls glibc's real prototypes at global
// scope (matching exception specifiers, so no redeclaration conflict) and the
// <cstdarg> va_* macros, AND it `#undef`s the glibc EOF macro -- which the
// decomp needs gone, since it uses `EOF` as an enumerator
// (JSUStreamEnum.hpp: enum EIoState { GOOD=0, EOF=1 }), not the stdio sentinel.
#ifndef SUNBRIGHT_SHIM_PRINTF_H
#define SUNBRIGHT_SHIM_PRINTF_H

#include <stdio.h>
#include <cstdarg>

#endif // SUNBRIGHT_SHIM_PRINTF_H

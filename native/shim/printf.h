// Host-libc forwarding shim for the decomp's `#include <printf.h>`.
//
// In the original GameCube build this resolves to Metrowerks MSL's
// PowerPC_EABI_Support/.../MSL_C/MSL_Common/printf.h, which declares the
// printf family (snprintf/vsnprintf/...) and pulls in MSL <stdarg.h>.
//
// On the host include path a bare `<printf.h>` instead resolves to glibc's
// /usr/include/printf.h, which is a GNU register_printf_* extensions header
// and does NOT declare snprintf/vsnprintf at all -> game code (e.g.
// M3DUtil/MActorData.hpp, J2D/J2DPrint.cpp) fails with "snprintf must be
// available" / "vsnprintf was not declared".
//
// Forward to the host C <stdio.h> (the printf family at global scope, via the
// stdio.h shim that chains to glibc and also brings the <cstdarg> va_* macros).
#ifndef SUNBRIGHT_SHIM_PRINTF_H
#define SUNBRIGHT_SHIM_PRINTF_H

#include <stdio.h>
#include <cstdarg>

#endif // SUNBRIGHT_SHIM_PRINTF_H

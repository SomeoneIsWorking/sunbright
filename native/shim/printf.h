// Host-libc forwarding shim for the decomp's `#include <printf.h>`.
//
// In the original GameCube build this resolves to Metrowerks MSL's
// PowerPC_EABI_Support/.../MSL_C/MSL_Common/printf.h, which declares ONLY the
// printf family (printf/sprintf/snprintf/vsprintf/vsnprintf) and pulls in
// <stdarg.h>. Crucially MSL's <printf.h> does NOT define the <stdio.h> object
// macros (EOF, SEEK_*, BUFSIZ, ...) or the FILE type.
//
// That distinction is load-bearing: the decomp has
//   enum EIoState { GOOD = 0, EOF = 1 };   (JSystem/JSupport/JSUStreamEnum.hpp)
// which uses `EOF` as an enumerator. If <printf.h> drags in glibc's
// `#define EOF (-1)` macro (which both <stdio.h> and <cstdio> do), EOF expands
// to `(-1)` and ~17 game files that pull printf.h ahead of JSUStreamEnum fail
// with "expected identifier before '(' token". So this shim must NOT include
// any host stdio header.
//
// On the host include path a bare `<printf.h>` resolves to glibc's
// /usr/include/printf.h (GNU register_printf_* extensions) which does not
// declare snprintf/vsnprintf at all. So we can neither include host <printf.h>
// (no printf family) nor host <stdio.h>/<cstdio> (drags in the EOF macro).
//
// Faithful resolution: declare exactly the printf family at global scope, like
// MSL's <printf.h>, and nothing else. Signatures match glibc's, so if a TU
// later also includes <stdio.h>/<cstdio> these are compatible redeclarations.
#ifndef SUNBRIGHT_SHIM_PRINTF_H
#define SUNBRIGHT_SHIM_PRINTF_H

#include <cstddef>  // size_t
#include <cstdarg>  // va_list (for the v* variants)

extern "C" {
int printf(const char* format, ...);
int sprintf(char* s, const char* format, ...);
int snprintf(char* s, size_t maxlen, const char* format, ...);
int vsprintf(char* s, const char* format, va_list arg);
int vsnprintf(char* s, size_t maxlen, const char* format, va_list arg);
}

#endif // SUNBRIGHT_SHIM_PRINTF_H

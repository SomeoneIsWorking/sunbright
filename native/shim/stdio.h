// Host-libc forwarding shim for the decomp's `#include <stdio.h>`.
//
// In the original GameCube build this resolves to Metrowerks MSL's
// PowerPC_EABI_Support/.../MSL_C/MSL_Common/stdio.h, which (besides the printf
// family) includes MSL <stdarg.h>, so a file that `#include <stdio.h>` also
// gets va_list/va_start/va_arg/va_end. Game code relies on that transitive
// pull -- e.g. JSystem/JUtility/JUTDirectPrint.cpp uses va_start/va_end after
// only `#include <stdio.h>`. The host's /usr/include/stdio.h pulls only the
// __gnuc_va_list typedef, NOT the va_* macros, so those uses fail with
// "va_start was not declared".
//
// This shim is first on the include path, so it shadows the real C <stdio.h>.
// Pull the real glibc <stdio.h> via #include_next (it puts snprintf/etc. at
// global scope, which is what the game expects -- and which libstdc++'s
// <cstdio> wrapper itself needs, so we must NOT redirect to <cstdio> here or
// we would break that wrapper recursively). Then add <cstdarg> to reproduce
// MSL's transitive stdarg (the va_* macros).
#ifndef SUNBRIGHT_SHIM_STDIO_H
#define SUNBRIGHT_SHIM_STDIO_H

#include_next <stdio.h>
#include <cstdarg>

#endif // SUNBRIGHT_SHIM_STDIO_H

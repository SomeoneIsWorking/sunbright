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

// Mirror MSL: the GameCube MSL <stdio.h> does NOT define the EOF macro (only
// MSL <ctype.h> does, as `-1L`, and no JSU-stream TU pulls ctype.h). The decomp
// relies on that: JSUStreamEnum.hpp declares `enum EIoState { GOOD=0, EOF=1 }`
// and game code uses `EOF` as that enumerator (e.g. setState(EOF)) -- never as
// the C stdio sentinel. glibc's <stdio.h> DOES `#define EOF (-1)`, which would
// expand inside the enum to `(-1)=1` ("expected identifier before '('") in
// every TU that includes <stdio.h> ahead of a JSU stream header. Drop it to
// match MSL; nothing in the decomp consumes stdio's EOF value.
#ifdef EOF
#undef EOF
#endif

#endif // SUNBRIGHT_SHIM_STDIO_H

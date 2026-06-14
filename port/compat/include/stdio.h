// Shadow <stdio.h> for the native port.
//
// Several decomp sources do `#include <stdio.h>` for snprintf/vsnprintf. With
// the MetroWerks MSL tree kept off the include path, that include lands on
// glibc's <stdio.h> — whose object-like `EOF` macro (-1) collides with the
// decomp's enumerator `enum EIoState { GOOD, EOF }` (JSUStreamEnum.hpp),
// producing "expected identifier before '(' token".
//
// We use #include_next to pull the REAL system <stdio.h> (the next one on the
// search path after this shadow — NOT <cstdio>, which would recurse back into
// this file), then #undef EOF so the game's enumerator spelling survives.
// Verified: no port-core source compares against the stdio EOF *macro* value,
// so this is behavior-preserving. (port/compat/include is first on -I, so this
// shadow is seen first; #include_next reaches the system header.)
#pragma once
// MSL's <stdio.h> transitively exposed the varargs macros; some decomp TUs
// (e.g. JUTDirectPrint.cpp) use va_start/va_end having included only <stdio.h>.
// glibc's <stdio.h> internally pulls just the va_list typedef (via
// __need___va_list), which sets gcc's stdarg include guard and would suppress
// va_start if we pulled <stdarg.h> AFTERWARD. So pull the full <stdarg.h>
// FIRST, then the system <stdio.h>.
#include <stdarg.h>
#include_next <stdio.h>
#ifdef EOF
#undef EOF
#endif

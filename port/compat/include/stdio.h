// Shadow <stdio.h> for the native port.
//
// Several decomp sources do `#include <stdio.h>` for snprintf/vsnprintf. With
// the MetroWerks MSL tree kept off the include path, that include lands on the
// system <stdio.h>. We #include_next to pull the REAL system header (the next on
// the search path after this shadow — NOT <cstdio>, which would recurse back
// into this file). port/compat/include is first on -I, so this shadow is seen
// first; #include_next reaches the system header.
//
// EOF NOTE: the system `EOF` macro (-1) collides with the decomp's ENUMERATOR
// `enum EIoState { GOOD, EOF }`. We do NOT #undef EOF globally here — that broke
// standard C++ library headers (e.g. libc++ <algorithm> -> char_traits.h) that
// legitimately use the EOF macro. Instead the collision is handled SURGICALLY in
// the shadow JSystem/JSupport/JSUStreamEnum.hpp (push_macro/undef/pop_macro
// around just that one enum definition), leaving the macro intact everywhere else.
#pragma once
// MSL's <stdio.h> transitively exposed the varargs macros; some decomp TUs
// (e.g. JUTDirectPrint.cpp) use va_start/va_end having included only <stdio.h>.
// glibc's <stdio.h> internally pulls just the va_list typedef (via
// __need___va_list), which sets gcc's stdarg include guard and would suppress
// va_start if we pulled <stdarg.h> AFTERWARD. So pull the full <stdarg.h>
// FIRST, then the system <stdio.h>.
#include <stdarg.h>
#include_next <stdio.h>
// (EOF is intentionally left defined — see the EOF NOTE above.)

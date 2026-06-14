// Shadow <stdarg.h> for the native port: pull the REAL system <stdarg.h> via
// #include_next (va_list / va_start / va_arg / va_end). The MSL <stdarg.h> is
// off the include path; the decomp's varargs printf helpers need these.
// #include_next (not <cstdarg>) avoids recursing back into this shadow.
#pragma once
#include_next <stdarg.h>

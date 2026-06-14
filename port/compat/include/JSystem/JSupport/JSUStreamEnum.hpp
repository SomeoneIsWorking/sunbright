// Shadow of reference/sms/include/JSystem/JSupport/JSUStreamEnum.hpp.
//
// The decomp uses `EOF` as an ENUMERATOR (EIoState::EOF == 1), but the system
// <stdio.h> defines `EOF` as a macro (-1). The shadow <stdio.h> no longer #undef's
// EOF globally (that broke standard C++ library headers — e.g. libc++ <algorithm>
// -> char_traits.h — which legitimately use the EOF macro). So hide the macro
// LOCALLY, only around this one enum definition, then restore it immediately:
// #pragma push_macro/pop_macro is supported by both GCC and Clang.
//
// Keep in sync with the pristine header (same include guard so this shadow, first
// on the -I path, replaces it).
#ifndef JSU_STREAM_ENUM_H
#define JSU_STREAM_ENUM_H

enum JSUStreamSeekFrom {
	JSUStreamSeekFrom_SET = 0,
	JSUStreamSeekFrom_CUR = 1,
	JSUStreamSeekFrom_END = 2
};

#pragma push_macro("EOF")
#undef EOF
enum EIoState { GOOD = 0, EOF = 1 };
#pragma pop_macro("EOF")

#endif

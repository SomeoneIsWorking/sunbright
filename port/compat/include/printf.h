// PORT SHADOW for the decomp's `#include <printf.h>` (e.g. M3DUtil/MActorData.hpp).
//
// On the GameCube/CodeWarrior toolchain, <printf.h> was the MetroWerks MSL
// header (reference/.../Msl/MSL_C/MSL_Common/printf.h) that declared the whole
// printf family — including `snprintf`/`vsnprintf` (verified in that file). The
// MSL tree is kept off the native include path (it shadows host libc), so on
// the host `#include <printf.h>` otherwise lands on glibc's *internal*
// /usr/include/printf.h, which declares the parse-format internals but NOT
// snprintf. Inside a template body (MActorAnmDataEach::loadAnmPtrArray) g++ then
// rejects the unqualified `snprintf` call: "there are no arguments to 'snprintf'
// that depend on a template parameter, so a declaration of 'snprintf' must be
// available" [-Wtemplate-body] (two-phase lookup needs the decl at definition).
//
// Faithful fix: route <printf.h> to the same place MSL's did — the standard C
// stdio declarations. We reuse the existing shadow <stdio.h> (which pulls the
// real system header via #include_next and #undef's the EOF macro so the
// decomp's `enum EIoState { GOOD, EOF }` survives). No semantic change: the
// printf family is declared exactly as MSL declared it.
#pragma once
#include <stdio.h>

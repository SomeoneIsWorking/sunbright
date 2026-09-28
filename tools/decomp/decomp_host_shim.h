/*
 * decomp_host_shim.h — force-included ONLY by tools/decomp/hostcheck.py.
 *
 * Purpose: let a host C++ compiler PARSE the decompiled sources so their
 * correctness can be checked at all. It is not a build of the game, it never
 * runs game code, and it is never compiled into anything shipped.
 *
 * Why it is needed: the decomp targets Metrowerks CodeWarrior for PowerPC, which
 * supplies a few things no host compiler has. Measured over all 580 game and
 * middleware translation units the checker discovers, the entire non-source
 * error surface is these five:
 *
 *   __frsqrte / __fres /   Gekko reciprocal-square-root, reciprocal and
 *   __cntlzw               count-leading-zeros instructions. MWCC exposes them as
 *                          builtins. The port's own native build supplies the
 *                          first two from the SDK header it force-includes
 *                          (extern/aurora/include/dolphin/ppc_math.h, pulled in by
 *                          extern/aurora/include/dolphin/types.h, which
 *                          sms-boot/CMakeLists.txt:198 force-includes), so the
 *                          checker's spelling matches the build it stands in for.
 *   AT_ADDRESS(addr)       places a declaration in a fixed MMIO window. On a host
 *                          there is no such window; the declaration is an
 *                          ordinary object.
 *   VERSION_SELECT(...)    picks the build's version constant. The decomp
 *                          defines it in include/version.h, which not every
 *                          translation unit includes, so the header is pulled in
 *                          here rather than edited into every file.
 *
 * What this file deliberately does NOT do:
 *
 *   It does not stub a missing field, header, or type. Those are DEFECTS in the
 *   decompiled source -- code that references a name which does not exist, or
 *   includes a header upstream deleted -- and hiding them behind a shim would
 *   turn a compile gate into a green light for broken code. That is how the two
 *   audio files in JAISystemInterface.cpp went unnoticed until they were read by
 *   hand: the tree had nothing that would have failed.
 *
 *   It does not touch decomp/sms. decomp/sms is a submodule tracking upstream
 *   plus this port's native deltas; host-only compatibility belongs to the
 *   checker, not to the recovered source.
 */

#ifndef DECOMP_HOST_SHIM_H
#define DECOMP_HOST_SHIM_H

/* VERSION_SELECT / VERSION_SELECT_JOIN live in the decomp's own version.h. */
#include <version.h>

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Gekko builtins. Real semantics, not stubs: a checker that compiled the wrong
 * expression would be able to hide a defect rather than find one. */
#ifndef __frsqrte
#define __frsqrte(x) (1.0f / sqrtf((float)(x)))
#endif
/* Gekko's fres is the reciprocal ESTIMATE. The port's own native build already
 * has to spell this one, in ppc_math.h: 0 stays 0, everything else is 1/x. The
 * checker uses the same expression so the two builds agree on what a call site
 * means. */
#ifndef __fres
#define __fres(x) ((x) == 0.0f ? (x) : 1.0f / (x))
#endif
#ifndef __cntlzw
#define __cntlzw(x) ((u32)__builtin_clz((u32)(x)))
#endif

/* No MMIO window on a host, so the qualifier has nothing to place. */
#ifndef AT_ADDRESS
#define AT_ADDRESS(addr)
#endif

#endif /* DECOMP_HOST_SHIM_H */

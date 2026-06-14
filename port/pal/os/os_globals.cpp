// PAL: single real definitions for the GameCube hardware-placeholder globals the
// decomp declares (via AT_ADDRESS) in <dolphin/os.h>. On GC these live at fixed
// MMIO/low-RAM addresses; on the host they are plain process globals.
//
// The port compiles the core with -fcommon, so each TU's header-declared copy of
// these globals is a COMMON symbol (restoring the decomp's MWCC single-instance
// model — without -fcommon, GCC's default -fno-common emits a definition per TU
// and the multi-TU link fails with "multiple definition of __OSBusClock"). The
// clocks below are real initialized definitions, so ld resolves all the COMMON
// references to these correct GameCube values (OS_TIMER_CLOCK = bus/4 etc.).
//
// Flagged independently by the OS-PAL and heap-PAL bring-up developers.
#include <dolphin/types.h>

u32 __OSBusClock  = 162000000u;   // 162 MHz GameCube bus clock
u32 __OSCoreClock = 486000000u;   // 486 MHz Gekko core clock

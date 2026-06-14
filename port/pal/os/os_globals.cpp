// PAL: single real definitions for the GameCube hardware-placeholder globals the
// decomp declares (via AT_ADDRESS) in <dolphin/os.h> and <dolphin/hw_regs.h>.
//
// On GC these live at fixed low-RAM / MMIO addresses; on the host they are plain
// process globals. Because the host AT_ADDRESS macro expands to nothing, the
// upstream header form is a C++ DEFINITION, which every TU that includes the
// header emits -> "multiple definition" at the smsport_main --whole-archive link
// (-fcommon does NOT help: it applies only to C tentative defs, not C++).
//
// The proper fix: the SHADOW headers (port/compat/include/dolphin/os.h and
// .../hw_regs.h) make every AT_ADDRESS line `extern` (a declaration only), and
// this TU provides the one-and-only definition of each. The clocks carry the
// real GameCube values; the rest are placeholder hardware state the native port
// does not touch, so zero/null/default is correct.
//
// Flagged independently by the OS-PAL and heap-PAL bring-up developers.
#include <dolphin/os.h>      // __OS* / __g* low-RAM globals + OSThread/OSThreadQueue types
#include <dolphin/hw_regs.h> // __VIRegs/__PIRegs/... MMIO register arrays

// --- dolphin/os.h low-RAM globals -----------------------------------------
u32 __OSPhysicalMemSize             = 0x01800000u; // 24 MB main RAM (GameCube)
volatile u32 __OSTVMode             = 0u;          // 0 = NTSC
OSThread* __gUnkThread1             = nullptr;
OSThreadQueue __OSActiveThreadQueue = {nullptr, nullptr};
OSThread* __gCurrentThread          = nullptr;
u32 __OSSimulatedMemSize            = 0x01800000u; // 24 MB
u32 __OSBusClock                    = 162000000u;  // 162 MHz GameCube bus clock
u32 __OSCoreClock                   = 486000000u;  // 486 MHz Gekko core clock
unsigned int __gUnknown800030C0[2]  = {0u, 0u};

// --- dolphin/hw_regs.h MMIO register arrays (placeholder backing store) ----
volatile u16 __VIRegs[59]    = {};
volatile u32 __PIRegs[12]    = {};
volatile u16 __MEMRegs[64]   = {};
volatile u16 __DSPRegs[32]   = {};
volatile u32 __DIRegs[16]    = {};
volatile u32 __SIRegs[0x100] = {};
volatile u32 __EXIRegs[0x40] = {};
volatile u32 __AIRegs[8]     = {};

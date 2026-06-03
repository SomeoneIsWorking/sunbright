#pragma once
// Frame watchdog — catches silent freezes/deadlocks (the game stops with no error log) and dumps
// the context needed to locate them to a file under scratch/watchdog/.
//
// A background thread watches a per-VI-field heartbeat. If the game stops advancing fields for
// SUNBRIGHT_WATCHDOG_SEC (default 10s) after it has started running, the watchdog fires: it samples
// the recomp dispatch counters (to tell a spin from a true block), dumps the guest CPU state, and
// signals the emu thread to capture its native backtrace (the recomp call chain = the stuck point).
// On by default; SUNBRIGHT_WATCHDOG=0 disables, SUNBRIGHT_WATCHDOG_SEC=N sets the timeout.
void watchdog_init();                  // install handler, subscribe to VI fields, start the thread
void watchdog_register_emu_thread();   // call from the first recomp entry — records the emu thread

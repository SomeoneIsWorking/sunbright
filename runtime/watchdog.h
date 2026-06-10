#pragma once
// Frame watchdog — catches silent freezes/deadlocks (the game stops with no error log) and dumps
// the context needed to locate them to a file under scratch/watchdog/.
//
// A background thread watches a guest-progress heartbeat: a VI field presented (running phase) OR a
// CoreTiming tick (boot phase, before the first frame). If neither advances for
// SUNBRIGHT_WATCHDOG_SEC (default 5s) after the core starts, the watchdog fires: it samples the
// recomp dispatch counters (to tell a spin from a true block), dumps the guest CPU state, signals
// the emu thread to capture its native backtrace (the recomp call chain = the stuck point), then
// KILLS the process (exit 137) — a freeze is always a bug. Covering CoreTiming (not just VI fields)
// is what lets it catch pre-first-frame freezes like the audio-init spin after w1stLoad_0.aw.
// On by default; SUNBRIGHT_WATCHDOG=0 disables, SUNBRIGHT_WATCHDOG_SEC=N sets the timeout.
void watchdog_init();                  // install handler, subscribe to VI fields, start the thread
void watchdog_register_emu_thread();   // call from the first recomp entry — records the emu thread

// Deliberate diagnostic park: the caller has detected an unrecoverable runtime bug, dumped its
// context, and now parks its thread CPU-idle (sleep loop) so the SUNBRIGHT_PROBE REPL stays
// available for live inspection. Marks the process "parked" so the watchdog does NOT treat the
// stopped frame flow as a freeze-and-kill — a park must never busy-spin OR get reaped before
// it can be inspected. [[noreturn]] for the calling thread.
[[noreturn]] void sunbright_park(const char* reason);

// VI-field heartbeat counter (presented fields, pet by vi_end_field_event). Used by the idle
// driver to pace the native retrace transaction when every guest thread is blocked.
unsigned long long watchdog_vi_fields();

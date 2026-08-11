---
id: I020
kind: instrument
status: trusted
created: 2026-08-11
---

## Instrument

rt_install_crash_handler (sms-recomp/runtime/rt_core.cpp) — on SIGSEGV/SIGBUS/SIGFPE/SIGILL prints the fault address, the guest call stack (func_<addr> frames) and the FULL host call stack, then re-raises so the exit status and core dump are unchanged. Before it, a crash produced exit code 139 and nothing else, which is why issue #2 stayed open for an intermittent fault nobody could attribute. Blind to: anything that corrupts state without faulting, and to a fault inside the handler itself (it re-enters once and then dies plainly).

## Validated by

SBR_CRASH_SELFTEST=1 dereferences a null pointer on purpose; verified 2026-08-11 that it prints the report and dies by the real signal. It named the aurora render-worker race on the FIRST reproduction after being installed.

## Known failure modes

(none recorded yet)

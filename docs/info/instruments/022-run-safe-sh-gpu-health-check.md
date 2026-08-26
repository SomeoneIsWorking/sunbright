---
id: I022
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

run-safe.sh GPU health check

## Validated by

`run-safe.sh` preflights the current-boot journal, then `tools/render/gpu_watch.py` follows it live
from an end-of-journal cursor while the guarded command runs in its own process group. The FIRST new
illegal-command-stream / VM-fault / ring-timeout / reset line immediately sends SIGKILL to that
exact group before any filesystem sync; the watcher then writes a durable cooldown stamp and
incident file, and the incident gains the
following process/ring lines, static CPU-only host and run settings, process-output tail, kernel
tail, and matching `gpu_flight_dump` analysis. The exact timezone-aware fractional kernel timestamp
is converted to epoch nanoseconds without a floating-point/microsecond round trip and passed as
`--kernel-real-ns`, so the reader emits its outstanding-at-event `CAUSAL-WINDOW` rather than treating
later missing callbacks as origins. Signal protection is installed before either owned process or
pump thread is created and remains installed through process reaping and thread cleanup. A dead
journal watcher also kills the group rather
than allowing an unmonitored run. This is an EXTERNAL check that does not depend on the process being
checked.

The planted controls cover both answers: a harmless journal line lets a CPU-only command exit zero
without a stamp; the exact 2026-08-26 first-error shape (`Illegal register access in command stream`)
kills the launched parent+child process group before the first durable write, returns 86, and preserves its exact timestamp plus
subsequent ring/process lines; deliberate watcher exit kills the command and returns 85. A real
CPU-only command under the system journal follower exits zero. Earlier before/after controls remain
historical evidence: the reset-heavy 2026-08-12 boot scored positive while idle and later guarded
Aurora/decomp runs stayed unchanged. A fake flight reader asserts the exact planted epoch-nanosecond
argument and returns `CAUSAL-WINDOW` text that must survive in the incident; a patched-fsync control
observes both file and parent-directory sync calls. A SIGTERM control proves the child leader,
descendant, and journal follower are gone before the original handlers are restored.

## Known failure modes

Preflight uses journal boot-monotonic timestamps, not the wall clock (which stepped backward on this
machine). A kernel fault names the first externally visible failure, ring, and process; it does not
prove which recorded submit or draw emitted the illegal command. Pending submits in the flight tail
are aftermath candidates, not automatically the cause. Both guarded launchers, `run-safe.sh` and
`run-render.sh`, route through the same live watcher.

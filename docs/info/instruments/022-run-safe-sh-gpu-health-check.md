---
id: I022
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

Default `run.sh` GPU health check

## Validated by

`run.sh` enters through `tools/launch/run.py`, preflights the current-boot journal, then
`tools/render/gpu_watch.py` follows it live
from an end-of-journal cursor while the guarded command runs in its own process group. The FIRST new
illegal-command-stream / VM-fault / ring-timeout / reset line immediately sends SIGKILL to that
exact group before any filesystem sync; the watcher then writes a durable cooldown stamp and
incident file, and the incident gains the
following process/ring lines, static CPU-only host and run settings, process-output tail, kernel
tail, Linux device-coredump disposition, and matching `gpu_flight_dump` analysis. The device-
coredump scan snapshots `/sys/class/devcoredump` before launch and only considers a new node after
the fault. When kernel context names a PCI BDF, a node whose `failing_device` resolves to a different
BDF is reported as `UNRELATED` and is not captured. After SIGKILL it preserves readable `data` into
a sibling artifact under bounded time/byte/node caps without writing to sysfs. The potentially
blocking sysfs open/read runs in an exact, killable child process; the watcher owns its deadline,
sends SIGKILL on expiry, reaps it, and publishes only a completed, validated staging file. Stale,
empty, truncated, timed-out, permission-denied, disabled,
unavailable, and expired-or-consumed outcomes are named in the incident instead of being omitted or
reported as a successful capture. The exact timezone-aware fractional kernel timestamp
is converted to epoch nanoseconds without a floating-point/microsecond round trip and passed as
`--kernel-real-ns`, so the reader emits its outstanding-at-event `CAUSAL-WINDOW` rather than treating
later missing callbacks as origins. Signal protection is installed before either owned process or
pump thread is created and remains installed through process reaping and thread cleanup. A dead
journal watcher also kills the group rather
than allowing an unmonitored run. This is an EXTERNAL check that does not depend on the process being
checked.

The planted controls cover both answers: a harmless journal line lets a CPU-only command exit zero
without a stamp; the exact 2026-08-26 first-error shape (`Illegal register access in command stream`)
kills the launched parent+child process group before the first durable write, returns 86, and
preserves its exact timestamp plus subsequent ring/process lines; its fixture-backed new device-
coredump node is copied byte-exactly. Independent fixture controls force readable, empty, truncated,
permission-denied, expired, announced-but-consumed, disabled, absent-class, stale-prelaunch-node,
and mismatched-PCI-device results. Deliberate watcher exit kills the command and returns 85. A real
CPU-only command under the system journal follower exits zero. Earlier before/after controls remain
historical evidence: the reset-heavy 2026-08-12 boot scored positive while idle and later guarded
Aurora/decomp runs stayed unchanged. A fake flight reader asserts the exact planted epoch-nanosecond
argument and returns `CAUSAL-WINDOW` text that must survive in the incident; a patched-fsync control
observes both file and parent-directory sync calls. A SIGTERM control proves the child leader,
descendant, and journal follower are gone before the original handlers are restored. A FIFO blocked
inside the coredump worker's `open()` proves the parent deadline kills and reaps the exact reader and
leaves neither a staging file nor a capture artifact.

## Known failure modes

Preflight uses journal boot-monotonic timestamps, not the wall clock (which stepped backward on this
machine). A kernel fault names the first externally visible failure, ring, and process; it does not
prove which recorded submit or draw emitted the illegal command. Pending submits in the flight tail
are aftermath candidates, not automatically the cause. Both guarded launchers, `run.sh` and
`run-render.sh`, route through the same live watcher. Linux's generic device-coredump
[`data` node is mode `0600`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/base/devcoredump.c);
an unprivileged game watcher therefore cannot guarantee payload access. The
incident preserves node metadata and says `PERMISSION-DENIED` when no administrator-installed ACL
or external udev collector makes it readable. [Kernel documentation](https://docs.kernel.org/process/debugging/driver_development_debugging_guide.html#device-coredump)
also says the node expires on a timer whose exact duration must not be relied upon; an announcement with no surviving new node is
reported as `EXPIRED-OR-CONSUMED`, never as "no dump."

`SBR_RADV_HANG_DIAG=1` is a separate explicit diagnostic lane. The launcher then preserves the
effective `RADV_DEBUG=hang` value across `.env`, and the watcher snapshots RADV dump identities
immediately before launch. It accepts only a new `radv_dumps_<exact-child-pid>_*` directory, copies
it under bounded file/byte/time limits, and reports the last reached plus first not-reached
command-processor trace IDs when `trace.log` contains them. Collection runs on fault, clean/nonzero
exit, signal, and wall-cap paths; even an absent dump produces a durable `UNKNOWN` terminal report.
The wall-cap path repeats the final kernel cursor barrier after killing the child and making a
bounded reap attempt, so a fault arriving at the timeout boundary becomes an incident instead of an
apparently clean timeout. Cleanup retries the reap if the bounded attempt expires.
Without the explicit opt-in, a caller-supplied `RADV_DEBUG=hang` is rejected. I034 validates the
collector/parser's positive and negative CPU fixtures and real RADV 26.1.8 activation. RADV's
inserted synchronization can mask a lifetime/order defect, so this lane is independent diagnostic
evidence and never normal-run equivalence.

The unified default-launch control on 2026-08-27 ran `run.sh --diagnostic --stage 1 --quit-after
60 --run-secs 90`. The optimized-Debug game exited 0 after 60 presents, the flight reader decoded
60/60 successful-complete submits with no pending/error callback or corrupt/bounds record, and the
watcher crossed its final kernel barrier without a GPU fault. The unlimited-mode CPU control uses
the same `run_guarded(..., None)` path and exits cleanly when its child does; this proves ordinary
interactive play does not secretly inherit the diagnostic wall cap.

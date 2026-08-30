# Device-coredump worker deadline was measuring process startup

The pre-commit gate intermittently failed the GPU-event instrument's known-answer controls while
the workstation was carrying a load average near 40 from unrelated builds. A regular 16-byte file
occasionally reported `timeout` or `timeout-unreaped`; the changing fixture (`captured`,
`truncated`, or `empty`) only changed which assertion exposed the same defect.

Root cause: `_copy_device_coredump` started `communicate(timeout=read_secs)` immediately after
launching a fresh Python interpreter. The parameter named the potentially blocking sysfs read
deadline, but actually included interpreter startup and runnable-process scheduling. A first fix
printed readiness on stdout; that was also wrong because buffered `readline()` could prefetch the
following JSON result and leave `communicate()` an empty stream.

The worker now announces readiness through a dedicated inherited one-byte pipe. Stdout remains the
exclusive result channel, and the parent begins the bounded device read only after receiving the
readiness byte. The timeout path still sends SIGKILL only to the recorded child PID, reaps it, and
requires no artifact. Non-timeout semantic fixtures receive a separate bounded scheduling
allowance; the planted blocking FIFO remains the deliberately short timeout control.

Five consecutive complete `gpu_events.py --selftest` runs passed under the same machine load. This
is falsified if any known-answer fixture changes status, if the FIFO reader escapes its deadline,
if the child remains alive/unreaped, or if a timed-out read leaves an artifact.

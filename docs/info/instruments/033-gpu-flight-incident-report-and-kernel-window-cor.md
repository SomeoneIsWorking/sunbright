---
id: I033
kind: instrument
status: trusted
created: 2026-08-26
---

## Instrument

The recomp GPU incident pipeline: Aurora's append-only `AuroraGpuSubmitInfo` probe, the durable
`sms-recomp/runtime/gpu_incident_recorder` flight/report pair, and
`build-sms-recomp/gpu_flight_dump --kernel-real-ns` correlated by the live kernel guard.

## Validated by

The historical known-positive is the real 2026-08-26 reset. Supplying the first illegal-command-
stream timestamp `1787773714689976000` selects only submit 1608, whose BEGIN and RETURN precede the
event, and excludes submit 1609, whose BEGIN follows it. The synthetic positive independently places
a fault while one submit is outstanding; timestamps before BEGIN and between BEGIN/RETURN are the
negative/state controls. The reader rejects corrupt, torn, truncated, stale-process, and stale-
session records. Fork/abort proves the fixed ring survives an unsynchronized process death. A v2
device-loss fixture proves pass, cache/resource, readback-lifecycle, and bounded draw-tail fields
reach both the binary record and synchronized text sidecar. Historical v1 data explicitly reports
that v2 fields are unavailable instead of interpreting zero as evidence. Success/error/cancelled
callback controls prove that only Dawn `Success` becomes a completed baseline. The destination-alpha
flag control covers both the disabled `UINT32_MAX` sentinel and enabled alpha zero.

## Known failure modes

The first kernel timestamp narrows which submitted command buffers were outstanding; it does not
prove which draw the GPU was executing. Dawn's `OnSubmittedWorkDone` callback can lag GPU completion,
so missing COMPLETE means callback not observed, not necessarily GPU-incomplete. The draw tail is
bounded and fingerprints semantic GX/Rml/Clear gfx-pass draws, not decoded driver commands. It
excludes later end-frame readback-copy, present-blit, ImGui, and profiler commands; readback fields
are aggregate lifecycle evidence. A driver coredump or finer GPU marker trace is still required to
prove the exact illegal packet. The live guard now attempts to preserve a newly-created Linux
device-coredump after killing the submitting process group, but the generic sysfs `data` attribute
is normally mode `0600`. Its incident therefore distinguishes a captured artifact from
`PERMISSION-DENIED`, `EXPIRED-OR-CONSUMED`, `EMPTY`, `TRUNCATED`, `TIMEOUT`, `DISABLED`, and
`UNAVAILABLE`. The sysfs read is isolated in an exact killable child, so even a driver callback
blocked in `open()` cannot exceed the parent-owned capture deadline; none of those negative states
is GPU-packet evidence. Without a readable kernel journal, the live
guard fails closed and the reader must not call the newest pending submit causal.

Aurora also persists `UNCAPTURED_ERROR` before its fatal abort, with Dawn's bounded error type/text
and the latest submit explicitly labeled as temporal context. This is API-side explanation, not a
hardware-progress breadcrumb. The independent opt-in RADV collector (I034) supplies the latter when
the driver emits a readable exact-child trace; its synchronization means that absence or changed
behavior cannot be treated as a normal-run result.

The integrated 2026-08-27 negative control ran 100 headless stage-1 Interpolated 60 FPS presents:
the v2 flight decoded 100 successful callbacks, zero pending submissions, and zero corrupt/bounds
records, while the external post-run preflight remained kernel-clean. This proves the live recorder
can produce the non-fault answer; it does not prove the earlier nondeterministic reset is resolved.

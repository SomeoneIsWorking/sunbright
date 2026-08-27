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
reach both the binary record and synchronized text sidecar. V3 fixtures prove exact append-only ABI
bounds, explicit replay-source lookup independent of the completed baseline, a known-different
uniform hash, and that an ordinary zero-lineage frame is reported as unpopulated rather than source
ownership. Historical v1/v2 data explicitly reports unavailable newer fields instead of interpreting
zero as evidence. Success/error/cancelled
callback controls prove that only Dawn `Success` becomes a completed baseline. The destination-alpha
flag control covers both the disabled `UINT32_MAX` sentinel and enabled alpha zero.

## Known failure modes

The first kernel timestamp narrows which submitted command buffers were outstanding; it does not
prove which draw the GPU was executing. Dawn's `OnSubmittedWorkDone` callback can lag GPU completion,
so missing COMPLETE means callback not observed, not necessarily GPU-incomplete. The draw tail is
bounded and fingerprints semantic GX/Rml/Clear gfx-pass draws, not decoded driver commands. The v3
selected pass-stream hash additionally covers pass metadata, debug-marker bytes, palette
conversions, recorded commands, and resolves with source/destination generations, path, and actual
source sample count. A pass-owned-marker deep-copy control covers the former global-index lifetime
failure; the live multi-presentation run covers shipping worker ordering. It excludes standalone texture-copy FrameOps, attachment load/store and clear
values, stencil clear, vertex/index/storage bytes, and later end-frame readback-copy, present-blit,
ImGui, and profiler commands; readback fields are aggregate lifecycle evidence. A driver coredump or finer GPU marker trace is still required to
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

After v3 landed locally, a 120 Hz match-refresh control exercised four presentations per 30 Hz tick:
40 submits completed successfully, including 30 retained intermediate samples. The reader resolved
a replay to its explicit real source and reported source frame, selected pass-stream hash, and full
uniform hash `SAME`. This is both the no-op control and the supported-more-than-two-presentations
control for install-before-interpolate lineage; it remains a bounded negative GPU sample.

The subsequent 600-second normal-paced Interpolated 60 control reached submit 9,311 after 4,650
simulation ticks and 4,650 retained presentations. The ring held 171 successful completions and
only the final wall-cap-cut returned submit without a completion callback, with zero
corrupt/torn/bounds records. The final watcher barrier and independent kernel preflight were clean.
This proves the instrument sustained the user's full random-failure window under a
multi-million-draw workload; it does not prove the intermittent reset is resolved.

The corrected worker-side lineage gate also produced its required opposite answer: an integration
version accidentally admitted the real source emission into the replay-only final-uniform check and
aborted on its unset expected hash versus the observed nonzero hash. After narrowing the gate to
replay emissions, a 120-presentation Match Refresh run completed 30 simulation ticks plus 90
retained samples, with 120/120 successful submit callbacks and no corrupt records.

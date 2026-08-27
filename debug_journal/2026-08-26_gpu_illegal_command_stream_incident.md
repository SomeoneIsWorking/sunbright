# GPU illegal-command-stream incident and reporting correction (2026-08-26)

## What the kernel proved

The 2026-08-26 interpolated capture did reset the GPU. The first relevant kernel event was
`22:48:34.689976`:

```
[drm:gfx_v10_0_priv_reg_irq [amdgpu]] *ERROR* Illegal register access in command stream
```

The following ring-timeout line named `sms-recomp` PID 1759510 on `gfx_0.0.0`; reset and VRAM loss
followed. Later KWin/browser faults occurred after that reset and are collateral. This is an illegal
privileged-register command-stream fault, not an initial GPUVM page fault. The host was Navi 22 / RX
6700 on amdgpu, kernel 7.1.9-200.fc44, Mesa RADV 26.1.7, Vulkan loader 1.4.341.0.

## The causal submit window

The surviving flight file is
`scratch/gpu_crash/session_1759510_18ce5cd3829e5b1b_recomp-aurora.flight`. Correlating its stored
realtime timestamps against the first kernel event changes the diagnosis from what the old reader
printed:

- submit 1607 COMPLETE callback: `22:48:34.674426`;
- submit 1608 BEGIN: `22:48:34.685945`;
- submit 1608 RETURN: `22:48:34.686865`;
- kernel illegal command stream: `22:48:34.689976`;
- submit 1609 BEGIN: `22:48:34.699069`.

Submit 1608 was the only Aurora submit in flight when the kernel detected the illegal access.
Submits 1609–1614 were submitted after the first fault and are aftermath, not the origin. Dawn's
`VK_ERROR_DEVICE_LOST` arrived 4.279 seconds after the kernel event, so selecting the newest pending
submit at the device-loss callback blamed the wrong frame.

Post-mortem `--submit 1608` inspection also found a Dawn `QueueWorkDoneStatus::Success` callback at
`22:48:38.955430`, 4.265 seconds after the first kernel event. That callback was not present at the
event-time boundary and cannot retroactively remove submit 1608 from the causal window. Conversely,
it does not prove that one of submit 1608's draws emitted the illegal packet: a delayed success
callback and the kernel's command-stream rejection are different observations.

Submit 1608 was interpolated replay emission `frameId=1608`, `frameIndex=1607`: five EFB passes,
1,483 draws, pass draw counts `0 / 4 / 71 / 1255 / 153`, and vertex/uniform/index byte totals
`1,240,532 / 3,167,552 / 764,916`. Its pipeline topology and hashes exactly match both the preceding
completed real emission and submit 1608 from an earlier successful interpolated capture. This
falsifies a uniquely bad pipeline set. Only aggregate dynamic command hashes differ, leaving a
replay-specific dynamic command/resource-lifetime failure or nondeterministic driver behavior as
the unresolved frontier.

## Two instrument mistakes corrected

The old reader called BEGIN+RETURN without COMPLETE `GPU-PENDING`. Aurora's COMPLETE boundary is a
Dawn `OnSubmittedWorkDone(AllowSpontaneous)` callback. Frame-dump MapAsync also uses a spontaneous
callback and performed a 4.9 MiB conversion and file write inside it. A missing COMPLETE therefore
means **completion callback not observed**, not proof that the GPU had not completed. The kernel ring
sequence still proves the real hang; the callback record alone does not.

The callback status is also part of that boundary: only Dawn `Success` is a completed-work
watermark. `Error` and `CallbackCancelled` records prove callback delivery but not successful GPU
completion, and are excluded from comparison baselines. Baseline selection uses recorder sequence,
not wall-clock ordering; realtime is reserved for the external kernel-event window.

The old `run-safe.sh` matcher also omitted `Illegal register access in command stream`, the first and
most specific error, and printed only secondary timeout/reset lines. It checked only after exit, so
the game continued submitting for seconds after the kernel had already identified the fault.

## Dump-path audit

Dense `SB_DUMP_FRAME_EVERY=1` capture was active, but the evidence does not attribute the reset to
it. The causal submit followed a COMPLETE callback for submit 1607; the later six queued writes were
aftermath. The causal submit contained one 1280x960 readback copy. Its refcounted source/destination
lifetime, 5,120-byte aligned row pitch, and exact 4,915,200-byte extent are valid, and an equivalent
earlier capture completed. No speculative readback cap was added.

The wider static replay audit found one real but non-causal ordering defect: persistent indexed-array
uploads call `Queue::WriteBuffer` on the game thread while submits are owned asynchronously by the
render worker, so a next-tick write can overtake an older replay submit. No persistent write occurs
between causal replay 1608 and its completed paired real emission, so that defect cannot explain this
incident. Issue 16 is now corrected: producer bytes are copied into the render-worker FIFO and the
Dawn `WriteBuffer` sink asserts worker ownership, preserving `older submit → upload → current
submit` without a GPU wait. The audit found no indirect draw/dispatch arguments,
encoder reuse, mapped staging-slot reuse, out-of-range replay staging range, or asynchronous resource
destruction in the causal pair.

The audit also excluded WSI acquire/present, present blit, ImGui, profiler queries, texture uploads,
texture copies, and persistent-arena changes from the headless causal submit. No static Aurora or
WebGPU contract violation has been proven for submit 1608. The strongest remaining application-side
gap is narrower: replay `DrawData` carries individual vertex, index, uniform, and storage references,
while the old encode seam checked only aggregate high-water marks. That is issue 17, a validation
coverage defect rather than evidence that the historical v1 flight contained an out-of-range draw.
The shipping encode seam now rejects GX/RML vertex, index, uniform, alignment, count/byte,
replay-prefix, high-water, and interpolation-span violations. Issue 17 subsequently closed the last
metadata gaps: GX draw records retain indexed-array byte extents and used slots, while RmlUi records
retain dynamic binding sizes. Their range ends are validated before Dawn encoding and included in
the durable submit fingerprint. Historical v1 submit 1608 did not record these fields, so this
closes future diagnostic coverage without retroactively attributing that incident.

## Reporting changes

Aurora's append-only submit probe now retains a bounded tail of draw fingerprints, pipeline IDs,
game tags/populations, state flags, draw ordinals, and vertex/index/uniform ranges, plus readback
queue/map lifecycle counters. The runtime writes a durable human-readable report on device loss and
the post-mortem reader prints realtime timestamps and accepts the first kernel-event time to select
the submits that were actually outstanding then. It explicitly reports coverage and causality
limits.

The bounded tail covers Aurora's semantic GX/Rml/Clear gfx-pass draws. It does not claim coverage of
commands encoded later by the host end-frame path: framebuffer readback copies, present blit, ImGui,
or profiler commands. Readback count/bytes and callback lifetime are aggregate evidence only.

Aurora's uncaptured-error callback now appends a distinct, crash-surviving phase before `FATAL` and
synchronizes Dawn's bounded error type/text into the human-readable sidecar. The latest submit is
labeled temporal context rather than cause. All Dawn callback strings resolve `WGPU_STRLEN` before
copying, so a sentinel length cannot turn crash reporting into an over-read.

The external diagnostic guard now follows the kernel journal live from an end cursor. The first new
illegal-command-stream, GPUVM, timeout, or reset event writes the durable fault stamp and incident
bundle, preserves the exact first line and subsequent ring/process context, and terminates only the
guarded process group. A dead watcher fails closed. CPU-only controls inject both harmless and known
fault lines, including the exact 2026-08-26 illegal-register shape; no GPU workload was used to
verify these reporting changes.

After that immediate kill, the watcher now snapshots and preserves a newly created Linux device
coredump when it can correlate the dump's `failing_device` PCI identity with the kernel fault. It
never writes to sysfs and bounds the read by time, bytes, and node count. Because sysfs open/read can
block inside the kernel, those calls run in an exact child process which the watcher kills and reaps
at the deadline; only a completed, schema-checked staging file is published. A captured artifact records
its SHA-256 and metadata; stale, unrelated, empty, truncated, permission-denied, disabled,
unavailable, and expired-or-consumed states remain distinct. The generic sysfs payload is normally
mode `0600`, so an unprivileged watcher cannot promise the bytes. This corrects the old binary
"captured or nothing" assumption without weakening the immediate stop.

## Remaining frontier

The old v1 flight cannot identify the exact draw inside submit 1608. The AMD device coredump was
root-only, was not captured, and expired. A future recurrence will preserve the v2 draw/readback
tail, kernel-time-correlated incident, and an explicit device-coredump disposition automatically.
That evidence can distinguish dynamic draw state and resource-lifetime pressure; it still cannot
prove which draw the GPU executed. Pinned Dawn exposes neither the Vulkan device/queue/command
buffer nor AMD device-fault/checkpoint extensions. Issue 19 removed the build-mode coupling:
optimized Debug and Release now both retain GX debug groups, API validation, and robustness, while
the packaged Dawn explicitly reports that Vulkan backend-validation layers were not built in.
Issue 18 now has a separate opt-in `SBR_RADV_HANG_DIAG=1` lane, which activates
`RADV_DEBUG=hang`, snapshots pre-launch dumps, and accepts only a new exact-child-PID trace after the
watcher stops the process. I034 validates byte preservation, last-reached/not-reached parsing, and
the absent/stale/wrong-PID/partial/denied answers on CPU fixtures. Real driver activation remains
unverified. RADV adds synchronization, so it may mask an ordering/lifetime defect and must never be
treated as normal-run equivalence.

## Integrated bounded runtime control (2026-08-27)

After the CPU controls and Clang build passed, `run-safe.sh` completed a headless stage-1
Interpolated 60 FPS run capped at 100 presents. Its v2 flight retained exactly 100 submits and 300
boundaries: all 100 received successful completion callbacks, none remained API- or callback-
pending, and no record was corrupt or out of bounds. The external post-run preflight found no new
illegal-command-stream, timeout, reset, or fault line. This verifies that the combined queue-owner,
replay-validator, recorder, and watcher path can run the live workload without disturbing the card.
It is a bounded negative control, not proof that the nondeterministic reset is resolved.

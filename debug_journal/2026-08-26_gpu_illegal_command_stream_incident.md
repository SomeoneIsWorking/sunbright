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
incident. It is tracked separately as issue 16. The audit found no indirect draw/dispatch arguments,
encoder reuse, mapped staging-slot reuse, out-of-range replay staging range, or asynchronous resource
destruction in the causal pair.

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

The external diagnostic guard now follows the kernel journal live from an end cursor. The first new
illegal-command-stream, GPUVM, timeout, or reset event writes the durable fault stamp and incident
bundle, preserves the exact first line and subsequent ring/process context, and terminates only the
guarded process group. A dead watcher fails closed. CPU-only controls inject both harmless and known
fault lines, including the exact 2026-08-26 illegal-register shape; no GPU workload was used to
verify these reporting changes.

## Remaining frontier

The old v1 flight cannot identify the exact draw inside submit 1608. The AMD device coredump was
root-only, was not captured, and expired. A future recurrence will preserve the v2 draw/readback
tail and kernel-time-correlated incident automatically. That evidence can distinguish dynamic draw
state and resource-lifetime pressure; it still cannot prove which draw the GPU executed without a
driver coredump or finer GPU debug-marker evidence.

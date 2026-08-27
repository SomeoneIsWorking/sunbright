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
standalone `TextureCopy` FrameOps, and persistent-arena changes from the headless causal submit.
The submit did contain post-pass EFB resolve operations on passes 0 through 3; the old
`textureCopies=0` aggregate did not count those and therefore never excluded them. No static Aurora
or WebGPU contract violation has been proven for submit 1608. The strongest remaining application-side
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
tail plus v3 replay-source lineage, kernel-time-correlated incident, and an explicit
device-coredump disposition automatically.
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

## Default-launch integration (2026-08-27)

The split between ordinary play and guarded diagnostics was itself a reporting hole: `run.sh`
armed the in-process flight recorder but did not run the external kernel watcher, so a user-observed
hard reset could lose the transient device-coredump opportunity. The separate `run-safe.sh` entry
point is removed. `run.sh` is now a slim locked-Python shim into `tools/launch/run.py`; both normal
unlimited windowed play and bounded `--diagnostic` runs use the same watcher. The compatibility
`play.sh` contains no policy and delegates to `run.sh`. This makes the first user-facing launch the
instrumented path rather than requiring the user to predict a crash and choose a diagnostic binary.

The integrated control used that exact entry point with `--diagnostic --stage 1 --quit-after 60
--run-secs 90`. It configured Clang optimized Debug, armed flight session
`18cecd6e47a7b756`, completed all 60 Aurora submits successfully with 0 pending/error callbacks and
0 corrupt/bounds records, exited 0, and crossed the watcher's final kernel barrier without a GPU
fault. This is a clean-path control for the unified launcher, not evidence that the nondeterministic
fault cannot recur.

## Random-window and RADV activation controls (2026-08-27)

The user clarified that the reset is random over roughly one to ten minutes. A 30-present
`SBR_RADV_HANG_DIAG=1` control is therefore only an activation test: Mesa RADV 26.1.8 printed its
own costly-mode warning and `Enabled debug options: syncshaders, hang`, proving the flag reached the
real driver. It exited with 30 successful submit callbacks and no kernel fault, so it did not test
hang-only trace production.

A separate normal-timing run kept the synchronized diagnostic disabled and used the guarded default
launcher for the full 600-second reported window. It reached submit 4,859, then the launcher killed
the process at its wall cap. Post-run kernel preflight was clean. The flight ring retained 170 recent
successful completions plus the final returned submit and final API-pending submit cut off by the
forced termination; those two are expected wall-cap teardown, not a fault signature. This is one
clean random-window sample, not crash-solidity evidence.

Auditing the hang lane exposed a remaining preservation boundary: the watcher kills the exact game
process at the first kernel fault, while RADV writes `radv_dumps_*` inside that process after it
detects the hang. The safety kill can therefore win the race and leave no complete `trace.log`.
Collecting on every terminal path closes missed-exit reporting, but cannot remove this fundamental
in-process writer race. Pinned Dawn exposes only the Vulkan instance, not its device/queue/command
buffer, so obtaining crash-surviving `VK_EXT_device_fault` vendor data requires extending the Dawn
backend boundary or an equivalent driver-owned facility, not merely an Aurora-side call.

After the every-terminal and timeout-final-barrier controls passed, the real synchronized lane ran
for the full 600 seconds. RADV again printed `syncshaders, hang`; the workload reached submit 5,403
before the wall cap killed it. The final kernel barrier and subsequent preflight were clean. The
watcher persisted `scratch/gpu_crash/radv_20260827T182601.995468Z_2940588.txt` with `status:
UNKNOWN`, zero eligible exact-child dumps, and no artifact. That is the correct answer for a run
where no hang occurred. It validates activation plus the absent real-driver terminal path, not real
hang report production. The two wall-capped submit totals do not measure throughput because launcher
and build time are included; the synchronized lane is timing-incomparable by construction.

## Replay-source lineage and current Aurora race audit (2026-08-27)

The historical comparison between completed submit 1607 and causal replay submit 1608 was not
source lineage: 1607 was the interpolated first emission, while 1608 replayed the untouched
pre-interpolation packet. Probe v3 now carries the source frame ID, source selected-pass-stream
hash, and source full-uniform hash explicitly. The recorder resolves that frame's real-emission
BEGIN independently of the latest successful completion baseline. A four-present-per-tick live
control retained 40 successful submits and 30 intermediate samples; a replay record resolved its
real source and reported all three lineage fields `SAME`.

Installation is the protected boundary. Aurora deep-copies the source passes and uniforms, checks
their hashes immediately, then the render worker independently re-observes that installed packet
before encoding any copied pass. Non-final retained samples may then interpolate their own packet;
immediately before unmap and submission the worker re-hashes the final selected command stream and
expected uniform prefix and proves the
global vertex/index/storage prefix writer epochs still belong to the source frame. This distinction is
required for display-refresh ratios above two presentations per 30 Hz simulation tick; validating
the intentionally interpolated packet against the untouched hash would deterministically abort
every intermediate.

The audit also found a concrete current lifetime race. `DebugMarker` commands stored an index into
a global marker-string vector; end-of-frame cleanup could repopulate that vector before the serial
render worker encoded the older packet or its replay. Marker labels are now owned by each
`RenderPass`, and replay copies that storage with the command. A deep-copy control preserves the
original long label after later marker repopulation; the live multi-presentation control exercises
the shipping worker ordering. This race is not assigned as the
historical fault because the old Release configuration did not enable debug groups.

The selected pass-stream hash now covers marker label bytes, palette conversions, resolve source
and destination resource generations, actual source sample count, and resolve path. It still does
not cover standalone texture-copy FrameOp ordering, attachment load/store and clear values, stencil
clear, vertex/index/storage bytes, or host-added present/readback/ImGui commands. The report names
those limits rather than turning hash equality into a whole-command-buffer claim.

The worker gate demonstrated the other answer during integration: its first live version applied
the replay-only final-uniform check to the real source emission, whose expected replay hash was
deliberately unset, and aborted with the observed nonzero hash. Restricting that gate to replay
emissions made the same 120 Hz path pass. This was a scope defect in the new instrument, not a GPU
fault, but it proves the shipping failure branch is reachable rather than decorative.

A normal-paced headless Interpolated 60 FPS control then ran for the complete 600-second reported
failure window. It rendered 4,650 simulation ticks plus 4,650 retained presentations and reached
submit 9,311. The fixed ring held 512 valid records: 171 retained submits had successful completion
callbacks and the final returned submit alone lacked its callback because the wall-cap terminated
the process. No record was corrupt, torn, or out of bounds. The watcher's final kernel barrier and
an independent 15-minute preflight both found no illegal-command-stream event, timeout, reset, or
fault stamp. The control processed about 5.5 million audited draws. It is a high-work negative
sample, not evidence that a random fault observed over the same interval cannot recur.

---
id: 4
title: the ablation sweep hangs the GPU ring: amdgpu resets the device mid-run, no attribution table ever printed
status: open
symptom: VK_ERROR_DEVICE_LOST during ./run-render.sh SBR_ABLATE=1; radv GPUVM fault at 0x800000000000; 'XIO: fatal IO error 2 on X server :0' at startup afterwards
tags: render,gpu,ablation,environment
created: 2026-08-12
updated: 2026-08-25
---

## What happens

`./run-render.sh SBR_ABLATE=1 SBR_AB=1 SBR_QUIT_AFTER=2000` renders two full 16-variant sweeps,
prints their checksums, and then dies:

    radv/amdgpu: The CS has been cancelled because the context is lost. This context is innocent.
    radv: GPUVM fault detected at address 0x800000000000.  ... RW: 1, CLIENT_ID: (CPG)
    ERROR: vkQueueSubmit VK_ERROR_DEVICE_LOST
    [fatal] [aurora::gpu] Device lost: vkQueueSubmit failed with VK_ERROR_DEVICE_LOST

A run started shortly after fails at SDL video init with `XIO: fatal IO error 2 ... after 241
requests` — the display session is still recovering from the reset.

## It is a kernel-level ring timeout, not an X problem

I first attributed this to the X server dropping. That was wrong; `journalctl -k` names it:

    amdgpu: ring gfx_0.0.0 timeout, signaled seq=4053345, emitted seq=4053347
    amdgpu:  Process sms-recomp pid 425515 ...
    amdgpu: Starting gfx_0.0.0 ring reset / reset succeeded / device wedged, but no recovery needed

`sms-recomp` is named as the guilty process on 3 timeouts today. So is `kwin_wayland` (17),
`plasmashell` (3), and two unrelated GPU programs on this box (`xenia_oracle`, `lf2`) — the machine
has broader GPU instability today, so ours is not the only offender, but our render runs do hang
rings. The X failure is downstream of the reset, not its cause.

## What is ruled out

* SDL3 GPU shader resource counts (the known previous cause of device loss here): the fragment
  shader declares 8 samplers + 1 uniform buffer and uses exactly `set=2 binding=0..7` plus
  `set=3 binding=0`. They match.
* API misuse the validation layers can see: the device is created with `debug=true` and
  `vulkan-validation-layers` is installed; the layers report nothing before the fault.
* A resource leak in the sweep: EFB-copy destinations are cached in `g_copyTex` by guest address,
  the vertex/transfer buffers are reused, and each pass fences and waits before the next.
* Non-reproducible re-render: `pass reproducibility: first ... second ... -> IDENTICAL` on every
  frame, and the `control:no-op` ablation checksums byte-identical to the baseline.

Untested, and the most likely remaining shape: 17 submit-and-wait passes per scored frame (179
draws each) is simply enough sustained work to trip the driver's timeout on this card while the
compositor is also submitting.

## Consequence

The operation-attribution table has never printed. The plain A/B run (no sweep) survives 4000
presents and reaches `COMPARABLE @ N=59`, so the renderer measurement itself is fine — it is only
the 16x re-render that takes the device down.

## What landed anyway

`sbr_compare_report_attribution` now (a) reports PAIRED deltas — variant minus baseline on the same
frame against the same aurora capture — instead of subtracting two means taken over different frame
sets, and (b) emits the table as it accumulates rather than only at shutdown, so an aborted run
still yields whatever it measured.

## Next step when the machine is stable

Sweep one variant per scored frame instead of all sixteen (16x fewer passes per frame, same paired
deltas, just spread over more frames), and re-run. That is the change that makes the tool fit inside
the GPU's timeout budget rather than working around the symptom.

### Note (2026-08-12)
Recurred WITHOUT the sweep: a plain native-renderer run at 600 presents died the same way (radv GPUVM fault at 0x800000040000 -> VK_ERROR_DEVICE_LOST), and after each loss the next process could not reach the display (XIO fatal IO error 2 at SDL init). The same run survived 4000 presents earlier the same day, so the card/driver state had degraded over the session rather than a code change causing it. The round-robin sweep fix (one variant per scored frame) was still correct and did let a 1500-present sweep complete; it was not sufficient on a degraded device.

### Resolution (2026-08-12)
Root-caused and guarded, 2026-08-12. Three unbounded things in the render path: a failed submit did not stop the frame loop (fifteen consecutive DEVICE_LOST submits into a resetting card), every fence wait was unbounded, and per-frame passes were unbounded (16 full re-renders per scored frame). All three are now bounded — gpu_disable() latches the renderer off permanently on first failure, wait_fence_bounded() caps the wait at 5s (SBR_GPU_FENCE_TIMEOUT), kMaxPassesPerFrame=4 refuses the surplus loudly. A fourth defect found by reading: g_copyTex cached EFB-copy targets by guest address at first-seen size, so a later larger copy blitted out of bounds — a GPU write fault, matching the reported signature; it now records the size and reallocates. Guards proven firing (scratch/logs/guard.log, guard2.log). Crucially the failure now OUTLIVES the process: gpu_disable writes scratch/gpu_fault.stamp and tools/render/gpu_preflight.py, wired into run-render.sh, refuses to start during a 15-minute cooldown after a stamp or an amdgpu reset in the kernel log. Full write-up: debug_journal/2026-08-12_gpu_hang_guards.md.

### Follow-up (2026-08-24)

The sustained-rate guard itself admitted too late: `sbr_render_end` submitted the reusable vertex
upload and then returned when rate-limited, omitting the fenced render/readback that ordinarily
made reuse safe. The next turbo frame could map the same upload buffer while that command remained
in flight. Admission now happens before any GPU operation and is owned by the CPU-tested
`native_gpu_admission` module. This closes another concrete reset mechanism; it does not retroactively
prove which mechanism caused a historical reset whose submit timeline was not captured.

The same audit also found lifecycle and command-contract defects outside the Native rate limiter:
recomp could replay a FIFO while no Aurora frame packet was active; failed WebGPU initialization
left partial global state for the next backend attempt; shutdown could destroy buffers while
spontaneous map/depth callbacks were still outstanding; and both producer and consumer accepted
unproven array/display-list/texture spans. Those boundaries are now authoritative, bounded, and
covered by failing controls. This broadens the set of removed reset/crash mechanisms; it still does
not retroactively attribute the old kernel reset to one mechanism without a surviving GPU trace.

### Reopened (2026-08-25)
Recurred on 2026-08-25 in the default recomp + Aurora path, not Native or the ablation sweep: after roughly 4,202 PAD polls, RADV cancelled the innocent context and Dawn aborted at vkQueueSubmit with VK_ERROR_DEVICE_LOST. This falsifies the short-run crash-solidity conclusion. The promised gpu_crash_watch.py/submit-tail instrument is absent from HEAD, so this recurrence has no new incident bundle.

### Instrument landed (2026-08-25, same day)

The submit-tail instrument now exists as an IN-PROCESS recorder, not a watcher:
`sms-recomp/runtime/gpu_incident_recorder.{h,cpp}` + reader `build-sms-recomp/gpu_flight_dump`.
Aurora emits BEGIN/RETURN/COMPLETE/DEVICE_LOST through `AuroraConfig::gpuProbeCallback`; the
recorder persists each boundary into a crash-surviving pwrite ring under
`scratch/gpu_crash/*.flight` before returning to Aurora, armed before Aurora init. After a loss,
`gpu_flight_dump <file>` names the in-flight submit — API-PENDING means the process died inside
the host queue call; GPU-PENDING means the queue accepted it and the device never finished (ring
hang). Verified both classes: fork-abort survival, corrupt/torn/truncated/stale rejection in unit
controls, and a clean 150-present run producing 450/450 records with all submits COMPLETED.
Full write-up: debug_journal/2026-08-25_gpu_submit_flight_recorder.md. The NEXT recurrence of
this issue must start with gpu_flight_dump on the newest flight file.

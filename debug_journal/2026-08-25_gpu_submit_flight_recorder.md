# GPU submit flight recorder — evidence that survives the crash (2026-08-25)

## Why

Claim C068 ("integrated recomp + Aurora is crash-solid") was FALSIFIED on 2026-08-25: a default
recomp + Aurora run died at ~4,202 PAD polls with RADV cancelling an innocent context and Dawn
aborting `vkQueueSubmit` with `VK_ERROR_DEVICE_LOST`
(docs/issues/0004-the-ablation-sweep-hangs-the-gpu-ring-amdgpu-res.md, "Reopened").

Every previous incident shared one property: **no surviving submit timeline**. The kernel log
names the process and the ring; the empty `devcoredump_*.txt` capture shows the GPU-side trace
never materialized; and nothing inside the process recorded what IT had submitted when the
device went away. Attribution was impossible by construction. The promised
gpu_crash_watch.py/submit-tail instrument was absent from HEAD.

## What landed

1. **Aurora probe API** (`extern/aurora`, uncommitted → committed with this work):
   `AuroraConfig::gpuProbeCallback/user`. Aurora emits `SUBMIT_BEGIN` before `queue.Submit`,
   `SUBMIT_RETURN` after it returns, and `SUBMIT_COMPLETE` from `OnSubmittedWorkDone`
   (`AllowSpontaneous` — the independent GPU-completion watermark), plus `DEVICE_LOST` from the
   device-lost callback while Running. `AuroraGpuSubmitInfo` is POD and bounded: submitId,
   frame identity, pass/draw/op counts, byte volumes, cache sizes, per-pass label/command/pipeline
   hashes (up to 16 passes). All submits now flow through `webgpu::submit_command_buffer`
   (frame AND imgui uploads) so no queue boundary escapes the probe.
2. **A real race fixed on the way**: `end_frame_impl` took `webgpu::present_source()` by
   const-ref and reread the mutable global inside the render worker while `GXCopyDisp` replaced
   it on the game thread — racing WebGPU handle assignment/destruction. It is now a value copy of
   the refcounted source selected at this frame boundary.
3. **The recorder** (the runtime's `gpu_incident_recorder` owner): armed BEFORE
   `aurora_initialize` in `host/main.cpp`; writes 2 KiB records into a 512-slot pwrite ring
   (`scratch/gpu_crash/session_<pid>_<session>_<label>.flight`, ~1 MiB). Commit markers +
   FNV-1a checksums distinguish torn from committed records; a wrapped slot is invalidated
   BEFORE its rewrite so a crash mid-write can never leave an older record masquerading as the
   latest. Mutex-guarded, allocation-free on the probe path (Dawn spontaneous callbacks may
   arrive on a driver thread). Post-mortem analysis (`analyze_file`) pairs BEGIN/RETURN/COMPLETE
   by submitId and rejects stale files by pid+session so an old flight file cannot impersonate a
   new incident.
4. **The reader** (the runtime's `gpu_flight_dump` tool):
   prints session identity, corrupt counts, submit-state census, any IN-FLIGHT submit, and the
   record tail. The discriminator: **API-PENDING** = the process died inside the host queue call;
   **GPU-PENDING** = accepted by the queue, never completed on the device (ring hang).

## Verification (all classes)

- Unit controls (`gpu_incident_recorder_test`, ctest `gpu_incident_recorder_test`): fork-and-abort
  between BEGIN and RETURN leaves ONE analyzable API-pending submit; RETURN vs COMPLETE are
  distinct; checksum rejects a flipped payload byte; cleared commit marker rejects a torn record;
  a truncated file is InvalidHeader WITH a named reason (found because the reader printed an
  EMPTY error for garbage input — fixed); wrap retains exactly 512 chronological records; wrong
  pid/session cannot consume a stale file. 18/18 ctest targets green.
- End-to-end positive: `./run-safe.sh SBR_QUIT_AFTER=150` (the lane that crashed) exited 0 with
  amdgpu delta 0, and its flight file analyzed clean: 450/450 records, 0 torn, 150 submits all
  COMPLETED, per-submit workload visible (~1215 draws, ~1.18 MB verts, 6 passes/frame).
- Reader negatives: missing file -> exit 2 with message; garbage file -> exit 1 naming the defect.

## What this does NOT do

It does not prevent or reduce crashes; it converts the NEXT one into an attribution. Kernel-side
evidence remains kernel-owned (`journalctl -k`); the flight file records exactly what this
process submitted up to the loss. If another DEVICE_LOST arrives, run gpu_flight_dump on the
newest `scratch/gpu_crash/*.flight` FIRST, then touch nothing else.

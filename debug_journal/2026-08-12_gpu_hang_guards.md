# The native renderer hung the GPU and took the desktop down twice (2026-08-12)

**One line:** the SDL3-GPU render path submitted unbounded work with unbounded waits and no reaction
to failure, hung the graphics ring seven times, and the second collapse needed a reboot. The code
changes below make each of those three bounded; the process change is that a device loss now ends
GPU work for the session instead of being retried.

## What happened, in order

| time | event |
|---|---|
| ~00:41 | first `VK_ERROR_DEVICE_LOST` during the 16-variant ablation sweep; `radv: GPUVM fault at 0x800000000000`, `RW: 1` |
| 00:41–01:25 | **five further render runs started**, each after a reset, several failing at SDL init with `XIO: fatal IO error 2` |
| — | amdgpu names `sms-recomp` on **7 ring timeouts**, alongside `kwin_wayland` (27), `plasma-systemmonitor` (8), `plasmashell` (3), `lf2` (3), `tomba2_port` (2), `xenia_oracle` (1) |
| 01:25:17 | last render run of mine ends |
| 01:25:48 | `watchdog: BUG: soft lockup - CPU#7 stuck for 26s! [kwin_wayland]`, kernel already `Tainted: [D]=DIE`, spinning in `native_queued_spin_lock_slowpath`; `lf2` follows |
| 01:34 | reboot |

The fatal lockup names kwin and lf2, not us, and `sms-recomp` appears nowhere in that boot's log.
That is not a defence. Our runs hung the ring seven times over the preceding session and left the
driver resetting repeatedly, and **the runs after the first device loss were the avoidable part** —
the project's own standing rule says a device loss stops GPU runs, and it was ignored five times
because each `XIO` failure was read as a fresh environmental problem rather than the tail of the
previous reset.

## The three defects in the render path

**1. A failed submit did not stop anything.** `render_pass_into_cpu` logged `submit+fence FAILED`
and fell through: it mapped the download buffer anyway, copied stale bytes into `g_cpu`, and the
frame loop called it again next frame. One run logged fifteen consecutive `DEVICE_LOST` submits into
a card the kernel was resetting.

*Fixed:* `gpu_disable()` latches the renderer off for the whole process on the first failure, and
every entry point (`init`, `begin`, `tris`, `end`, `readback`, `ablation_render`, `perform_copy`)
checks the latch. `readback` refuses too — `g_cpu` holds the last good frame, and returning it would
feed the A/B comparator a frozen picture scored as a live one.

**2. Every fence wait was unbounded.** `SDL_WaitForGPUFences` blocks forever, so a hung ring meant
the process sat in an uninterruptible wait while the kernel reset the card underneath it, and the
run had to be killed from outside.

*Fixed:* `wait_fence_bounded()` polls `SDL_QueryGPUFence` against a wall-clock budget (5 s default,
`SBR_GPU_FENCE_TIMEOUT`). A timeout is treated as a hung device and latches the renderer off.

**3. Per-frame work was unbounded.** The attribution sweep re-rendered the whole frame sixteen
times per scored frame — sixteen 179-draw passes, each with a fenced full-target readback, at turbo
speed with no pacing. That burst was a concrete timeout mechanism, but the surviving evidence does
not prove it was the historical reset's sole trigger.

*Fixed:* `kMaxPassesPerFrame = 4`, refused loudly above it. The sweep already spreads one variant
per frame; the checksum block, which used to sweep all fifteen at once on two frames, now spreads
too — each checksum was always a comparison against its own frame's baseline, never a cross-frame
quantity, so nothing was lost.

## A fourth defect, found while reading rather than running

`g_copyTex` cached EFB-copy destination textures **by guest address alone**, created at the size
first seen. A later copy to the same address with a larger rect blitted outside that allocation — a
GPU-side out-of-bounds **write**, which is exactly the fault signature the driver reported
(`GPUVM fault`, `RW: 1`). The cache now records each texture's size and reallocates when the
destination changes, with the blit rect clamped to the bound allocation as a second line. The
`dump-copy` path had the same bug in read form: it took the size from a copy point (or a hardcoded
320x224 fallback) rather than from the texture, and now asks the allocation.

Whether this was *the* fault or *a* fault is unproven — proving it needs a run, and runs are what
this entry is about.

## Proof the guards fire

Not reasoned about — run, before the moratorium:

* **fence timeout + latch** — `SBR_GPU_FENCE_TIMEOUT=0`: `the GPU has not signalled its fence in
  0.0s` → `NATIVE RENDERER DISABLED FOR THE REST OF THIS RUN`, and the run then completed normally
  at 200 presents with aurora still presenting (`scratch/logs/guard.log`).
* **pass cap** — `SBR_GPU_GUARD_SELFTEST=1`: `REFUSING a 5th offscreen pass this frame (cap 4)`,
  self-test confirms `asked for 6 passes with a cap of 4 ... REFUSED the surplus, as required`
  (`scratch/logs/guard2.log`).
* **clean run** — shutdown reports `native renderer ran to the end with no GPU fault; fence budget
  5.0s, at most 4 offscreen passes per frame`. A run that lost its device says so instead, because
  silence and success must not look alike.

The self-test states outright that it does **not** cover the fence path (the budget is cached before
it runs, by the white-texture upload inside `sbr_render_init`) and names the env var that does.

## The interlock — the part that actually prevents a repeat

The guards above protect one process. The damage came from the runs *after* the first loss, so the
failure has to outlive the process:

* `gpu_disable()` writes `scratch/gpu_fault.stamp` naming the fault.
* `tools/render/gpu_preflight.py` refuses to start when that stamp is recent, or when
  `journalctl -k` shows an amdgpu ring timeout / reset / `device wedged` / GPUVM fault inside a
  15-minute cooldown. It cannot pass vacuously: an unreadable kernel log is reported as *no result*
  and refuses, because "I could not look" and "I looked and it was clean" must not share an output.
* `run-render.sh` calls it and exits on refusal. `--force` / `SBR_GPU_PREFLIGHT=off` overrides, out
  loud, printing what is being overridden.

Verified both ways: a planted stamp refuses with exit 1, the override runs with exit 0 and says so.

## The gate, added after it happened a second time

The interlock above was not enough, and the reason is worth recording: it is a COOLDOWN, and a
cooldown only defends against the run that comes too soon. The user's report was flatter than that
— the machine becomes unusable when this renderer runs, full stop — and that observation outranks
our logs, which had just said "ran to the end with no GPU fault".

So `SBR_RENDERER=native` additionally requires per-session approval, checked inside
`sbr_render_init` before the device is created. The check lives there rather than only in
`run-render.sh` because the runs that did the damage bypassed that script entirely: they set
the renderer selector on `run-recomp.sh` directly. A gate in the launcher guards the launcher; a gate at
device creation guards the device.

The later user directive removed the former “no agent may approve it” rule: approval is an accident
gate, not a substitute for engineering or for doing the requested work. `run-render.sh` owns the
complete guarded invocation. A denial is not cached as an initialization attempt, so selecting and
approving Native from the in-game settings UI can retry in the same process.

## And the load itself, not just the failure handling

Everything above is about reacting to a fault. The sustained load that provoked it was never
addressed: the baseline path did a full offscreen re-render plus a **fenced full-target readback
every single frame**, and under `SB_TURBO` the game is unpaced — so that was thousands of fenced
submits per second, back to back, with no gap for anyone else's work. kwin's own submissions timing
out is the expected outcome of that, not a coincidence.

`SBR_RENDER_MAX_HZ` (default 10) now bounds the sustained rate. The comparator scores one frame in
sixty; it never needed a pass per frame. Frames whose pass is skipped mark `g_cpu` STALE and
`sbr_render_readback` refuses until a real one lands — a gap in the measurement, never a previous
frame handed over as a current one.

## Follow-up: rate admission originally happened after a submit (2026-08-24)

The first rate limiter bounded render/readback passes but made its admission decision too late.
`sbr_render_end` had already decoded/uploaded textures, mapped the reusable vertex transfer buffer,
submitted its copy command, and only then returned for a rate-limited frame. Because the fenced
render/readback did not run on that branch, the next turbo frame could map and reuse the same upload
buffer while the prior command was still in flight. Thus “skipped” meant “submitted GPU work but
skipped the synchronization that made its resource lifetime safe.”

The shipping admission rule now lives in `native_gpu_admission.{h,cpp}` and runs before every GPU
operation in `sbr_render_end`. Its CPU control proves the 10 Hz boundary and the explicit unlimited
mode through the same `NativeGpuRateLimiter` used by production. This is a real source-level reset
mechanism and is fixed; it is not claimed as the historical reset’s proven cause because no command
timeline survived that incident.

Runtime check after the rewrite: a 30-present `run-render.sh` plaza run completed with no native
GPU fault, reported 19 frames rejected by the 10 Hz admission boundary, and repeated-pass hashes
were identical. The unfiltered amdgpu anomaly count was 42 before and 42 after (delta 0). The run
overrode only the cooldown stamp created by the deliberate zero-timeout control 13 minutes earlier;
the override was printed by the preflight.

After extracting shader/pipeline ownership into `native_gpu_pipeline`, a second 15-present run
passed the preflight without override, produced identical repeat-pass hashes, reported nine
rate-rejected frames, and again left the kernel count at 42 (delta 0).

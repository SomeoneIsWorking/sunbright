---
id: 2
title: Stage 22 / 24 occasionally SIGSEGV at shutdown, after the run has completed
status: resolved
symptom: sms-recomp exits 139 (SIGSEGV) at the end of a run; the log shows the full end-of-run reports printing and the crash lands mid-report, after the last frame
tags: recomp,shutdown,intermittent
created: 2026-08-11
updated: 2026-08-11
---

## What is measured

`SBR_STAGE=22 SBR_LERP60=1 SBR_QUIT_AFTER=300` exited 139 once and 0 on the two runs after it with
the identical command; stage 24 did the same thing once. The failing log's last lines are the camera
cut diagnostic's per-tick eye positions, i.e. the crash is in the SHUTDOWN report path, after the
run has produced everything it was started for. The registry was flushed twice in that run, so the
frame loop completed normally.

## Why it is not being chased now

Every run's output is already on disk when it happens, so it costs nothing but a nonzero exit code —
and an intermittent fault reproduced 1 time in 3 cannot be bisected without a reproducer. Recorded
so the next person who sees a 139 does not read it as "the stage does not work": stages 22 and 24
both render and both populate the graphics registry.

## Where to start if it recurs

The shutdown path is `present_and_reopen`'s exit branch in `sms-recomp/overrides/native_frame.cpp`:
`final_reports()` then `aurora_shutdown()` then `std::_Exit(0)`. Suspicion, unverified: the reports
walk aurora-side arrays (`interp.cpp` population tables) while the GPU device is being torn down by
another thread — the same log ends with "Buffer mapping Aborted" and "Device lost: Device was
destroyed". Run under ASan with the reports forced every present to raise the hit rate.

### Note (2026-08-11)
Did not reproduce (2026-08-11) in a 15-stage sweep run after the arena fix (issue #1): stages 22, 23
and 24 each completed 150 presents and exited 0. That is NOT a fix — this issue is recorded as
intermittent, and three clean runs cannot distinguish "fixed" from "did not happen this time".

It is worth re-testing deliberately, though, because the arena bug was exactly the kind of cause that
produces intermittent memory faults: the heap and the main stack overlapped, so which allocation got
smashed depended on how deep the stack happened to be. If this stops reproducing over many runs,
issue #1 is the likely explanation.

### Note (2026-08-11)
ROOT CAUSE FOUND AND FIXED (2026-08-11). Two corrections to this issue's own description first:

  * it is NOT at shutdown. The runs that made it look that way were reading the log's tail; with a
    crash handler installed the fault lands mid-run, at whatever present the race happens to bite.
  * it is NOT stage-specific. It needs SBR_LERP60=1, and stage 24 simply loses the race most often.
    Twelve runs without interpolation: clean. With it: 3 of 3 on stage 24, and stage 22 too.

## The instrument that was missing

A SIGSEGV produced exit code 139 and nothing else, which is why this sat open — an intermittent
fault nobody can attribute cannot be bisected. `rt_install_crash_handler` (rt_core.cpp) now prints
the fault address and both the guest and the HOST call stack on SIGSEGV/SIGBUS/SIGFPE/SIGILL, then
re-raises so the exit status and core are unchanged. `SBR_CRASH_SELFTEST=1` faults on purpose and is
verified to fire. It named the crash on the first reproduction:

    SIGSEGV at fault address 0x10
    aurora::gfx::tex_copy_conv::execute  <-  aurora::gfx::render  <-  render_worker::worker_main

## The race

`interpolate_recorded_frame` (aurora, common.cpp) walks `frame.renderPasses` and writes
`pass.resolveTarget = {}` to suppress cross-frame feedback copies on the interpolated emission. Those
RenderPass objects were handed to the render worker as each pass was sealed, long before this runs.
So the main thread nulls a TextureHandle while the worker is encoding the same pass, and
tex_copy_conv::execute dereferences it: `mov 0x18(%rbx),%rax; mov 0x10(%rax),%rdi` — req.dst.get()
is 0 and 0x10 is offsetof(TextureRef, attachmentTextureView).

Proven, not inferred: reading `passInfo.resolveTarget.get()` into a local at the `if` and comparing
it with `convReq.dst.get()` a few instructions later gave **0x12825140 when tested and 0x0 when the
request was built, in the same function with no store between**. That check stays in the code as a
Log.fatal.

## The quiet half

The crash was the visible symptom. The same ordering meant whether a copy was suppressed AT ALL
depended on whether the worker had already encoded that pass — the classifier's decision applied to
some passes and was silently lost on others, differently every run. A frame that renders correctly
by winning a race is not a frame that renders correctly.

## The fix

`render_worker::synchronize()` before the mutation loop. It costs a pipeline stall once per
interpolated tick, which is stated at the code rather than hidden, and it is the only ordering under
which the mutation means anything.

Verified: stage 24 at 400 presents with interpolation, 4 of 4 clean where it had been failing about
one run in two.

### Resolution (2026-08-11)
interpolation nulled RenderPass::resolveTarget while the render worker was encoding that pass; synchronize the worker first

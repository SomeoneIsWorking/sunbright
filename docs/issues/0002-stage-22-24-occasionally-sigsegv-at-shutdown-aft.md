---
id: 2
title: Stage 22 / 24 occasionally SIGSEGV at shutdown, after the run has completed
status: open
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

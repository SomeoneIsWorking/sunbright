---
id: 34
title: Device-coredump controls conflated worker startup with read timeout
status: resolved
symptom: The commit gate intermittently reported captured, truncated, or empty device-coredump fixtures as timeout or capture-worker-failed while the host was under heavy compile load.
tags: tooling,gpu,diagnostic,selftest,process
created: 2026-08-30
updated: 2026-08-30
---

Root cause: the device-coredump copier started its read deadline immediately after launching the child, so Python startup and runnable-process scheduling consumed a deadline intended to bound potentially blocking sysfs reads. A first stdout readiness line also proved invalid because buffered line reading could prefetch the following JSON result. Resolution: use a dedicated inherited one-byte readiness pipe, begin the read deadline only after readiness, keep stdout exclusively for the result, retain exact-PID SIGKILL and reap behavior, and give non-timeout semantic fixtures a separate bounded scheduling allowance. The planted blocked FIFO remains the short timeout control. Five consecutive full GPU-event selftests passed under load.

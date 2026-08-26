---
id: 16
title: Persistent indexed-array uploads bypass the render worker
status: open
symptom: a next-tick Queue::WriteBuffer can overtake an older asynchronous replay Submit that reads the same persistent storage buffer
tags: recomp,aurora,gpu,ordering,interpolation,indexed-geometry
created: 2026-08-26
updated: 2026-08-26
---

## Root cause

`gx::fifo::drain()` runs on the game thread. Indexed-array handling reaches
`push_storage_persistent()`, whose storage-region writer calls Dawn `Queue::WriteBuffer` directly on
that thread. Real/replay command submission is asynchronous on the render worker. The game thread
can therefore issue a next-tick persistent-buffer write before the worker has submitted an older
replay command buffer that reads that region. There is no single owner that orders all queue calls.

This is a proven state/visual ordering defect, but it is **not attributed as the cause of the
2026-08-26 illegal-command-stream reset**. Causal replay submit 1608 had no game execution or
persistent write between it and completed real submit 1607, which consumed the same resources.
Static audit also excluded indirect command arguments, encoder reuse, mapped staging-buffer reuse,
out-of-range replay staging ranges, and asynchronous resource destruction in that pair.

## Proper fix and control

Keep content hashing and range allocation on the game thread, but copy the bytes into an owned
upload record and enqueue the write through the render worker. The worker must be the sole Dawn
queue-call owner and preserve `older submit -> persistent write -> current submit`; do not add a GPU
wait.

The regression control must use the shipping scheduling seam: block the worker, enqueue an old-submit
marker, schedule the production persistent write, enqueue the current-submit marker, then unblock.
An injectable queue-write sink must observe both the required ordering and execution on the worker
thread. The current direct call must fail this control before the ownership change.

## Evidence locations

- `extern/aurora/lib/aurora.cpp`: game-thread FIFO drain
- `extern/aurora/lib/gx/command_processor.cpp`: indexed-array persistent upload
- `extern/aurora/lib/gfx/common.cpp`: direct `Queue::WriteBuffer` and asynchronous worker submission
- `debug_journal/2026-08-26_gpu_illegal_command_stream_incident.md`: causal crash window and exclusions

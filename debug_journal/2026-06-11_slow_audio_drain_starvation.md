# Slow audio / endless post-file-select fade-out — drain starvation (FIXED 3e70de0)

## Symptom (user)
Audio plays very slowly; after the file-select fade-out the game appears to wait for the
audio to finish for a LONG time.

## Measurement chain (headless, SUNBRIGHT_AUTOSTART + PROBE + DUMP_AUDIO)
- /metrics two-point sample during the transition: emu_secs +0.167 s over 20 s wall; DSP WAV
  grew 0.16 s of audio in those 20 s → audio runs at emulated-time rate, and emulated time
  crawled at ~0.008x while video heartbeat frames kept pacing.
- vi-perf phase timer: the 64-frame window at frame ~1920 spent **drain=59982 ms** — the
  heartbeat thread sat ~60 s inside nthrt_block_drain.
- Stall-triggered gdb `thread apply all bt`: the only runnable thread was the scene SETUP
  thread (gSetupThread 0x803FCBE8, entry func_80296dd4 = TMarDirector::setupThreadFunc →
  loadResource) inside func_802b5458 → func_802b35cc → OSYieldThread.
- func_802b35cc (disasm): opens the loading-screen THP, then a TIMED WAIT —
  `while ((OSGetTick()-t0)/ticks_per_sec < threshold) OSYieldThread();`. Caller 802b5458
  switches on area id and picks the per-area duration (0x3e8/0xbb8/0x78 … ms).

## Root cause
block_drain (frame barrier in native VIWaitForRetrace) released only when NO other thread was
Ready. The timed loading wait yield-spins (stays Ready) for its whole duration → heartbeat
starved → wait_vi_field stopped forcing emulated time → CoreTiming (and the DSP audio frames
scheduled on it) crawled at the yield-idle rate → the OSGetTick wait itself stretched ~60x.
Circular: the wait waits on time that only the waiter's victim advances.

Hardware truth: the VI retrace is an INTERRUPT — a yield-spinning lower-priority thread can
delay it at most one field, never starve it. The unbounded drain was the unfaithful part.

## Fix (runtime/native_threads.* + dolphin_hook.* + sms_vi_native.cpp)
- `nthr::block_drain(deadline_us)`: DrainWait also releases at a host-clock deadline, checked
  at every token hand-off (a yield-spinner hands the token back each iteration).
- VIWaitForRetrace passes one field period (16'683 us).
- Yield idle path: a bounded DrainWait pending ⇒ skip idle_run (heartbeat owns time advance;
  idling would hold the token past the deadline and double-advance the clock). idle_run also
  breaks when a bounded drain deadline expires.

## Verified
Headless autostart through file-select into scene load: max drain 321 ms (was 59982), vi-perf
continuous, audio WAV accumulates through the loading window. recomp_test 43/43.

## Dead ends / notes
- /cur during the stall showed srr0=80296dd4 lr=OSExitThread for the setup thread — that is
  its CREATION context (OSThread ctx is stale while a thread runs natively); /cur is not a
  live-PC source for the running thread. gdb backtraces of the host threads are.
- The funcs map names 80296dd4 as registerEventWatcher+0xfc — wrong; it's the setupThreadFunc
  entry (map is sparse there).
- Pre-existing, NOT a regression (present in pre-fix logs too): GPU FIFO drain stall at scene
  entry — Vulkan "waiting for space in vertex/index buffer" spam + [vi] backpressure timeout
  (dist stuck ~0x19a40), VI fields stop, watchdog FREEZE with heartbeat spinning in the
  backpressure loop. This is the next frontier (drawsync/PE-token / GPU drain class).

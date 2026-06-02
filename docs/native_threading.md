# Native OS threading (PC-port model)

## Why
Sunbright is a **PC port**, not "Dolphin with ROM hacks." The GameCube OS multiplexes
software threads onto one CPU via a PPC software scheduler (`SelectThread` /
`__OSReschedule` + context save/load). Our recomp currently runs that scheduler under
Dolphin's interpreter inside `run_jit_sync`, single-stepping **until the call returns to
LR**. A blocking OS call (audio init waiting on the audio thread / a DVD read) reschedules
to the OS **idle loop**, which never returns to LR — so `run_jit_sync` spins to its
500M step budget, bails with inconsistent state, and the game enters `JUTException::run`.

The fix is not to lean on Dolphin's emulated scheduler (a stopgap, `SUNBRIGHT_*_HANDOFF`),
but to make threading **native**: each guest OS thread becomes a host thread, and the OS
thread/sync primitives are HLE'd to native C++ (`std::thread`, `std::mutex`,
`std::condition_variable`). The PPC scheduler is then **never executed** — no busy-spin,
no step budget, no dependence on emulated timing. A blocked thread is simply a host thread
parked on a condvar, so its continuation lives on its own native C stack (no
"resume-mid-function" problem).

## Current execution model (what we're changing)
- `runtime/jit_hook.cpp`: `--wrap` on `JitTrampoline` → `SunbrightBridge::Run(pc)` when the
  block is recompiled. Runs **on Dolphin's EmuThread**.
- `runtime/sunbright_bridge.cpp` `Run()`: makes a **stack-local `CPUState`**, syncs from the
  single global `PowerPC::PowerPCState`, runs the recomp tree, syncs back. One register file
  of record (Dolphin's), ephemeral `CPUState` per entry.
- `runtime/dolphin_hook.cpp`: `call_ppc` (recomp→recomp = nested C call; recomp→non-recomp =
  `run_jit_sync` interpreter loop), `tail_ppc` (siglongjmp back to `Run` via thread-local
  `g_tail_jmp`), `cpu_to_dolphin_state` / `dolphin_state_to_cpu`.
- `runtime/overrides/`: `SUNBRIGHT_OVERRIDE(name, addr)` registers a native replacement;
  consulted by `recomp_lookup` and `Run`. `force_jit` routes an address to Dolphin's JIT.

### Hard constraints found
- Dolphin's interpreter **asserts `Core::IsCPUThread()`** (`tls_is_cpu_thread`, set only in
  `EmuThread` via `DeclareAsCPUThread`) and uses `s_core_mutex`. A host thread that runs the
  interpreter must declare itself the CPU thread, and only one may do so at a time.
- **CoreTiming + interrupt delivery are driven by the CPU loop running.** If the EmuThread
  blocks, DSP/DVD/VI IRQs stop firing. So when no guest thread is runnable, a native
  **idle/driver** must advance CoreTiming until an IRQ marks a thread ready.

## Reference: GC OS API (GMSE01) — to override natively
Thread lifecycle: `OSCreateThread` 0x80348948, `OSExitThread` 0x80348a68,
`OSCancelThread` 0x80348b4c, `OSJoinThread` 0x80348d08, `OSDetachThread` 0x80348e48,
`OSResumeThread` 0x80348ee8, `OSSuspendThread` 0x80349170, `OSYieldThread` 0x8034890c,
`OSSleepThread` 0x803492e0, `OSWakeupThread` 0x803493cc, `OSSetThreadPriority` (see map),
`OSGetThreadPriority` 0x803494d0, `OSGetCurrentThread` 0x80348368,
`OSInitThreadQueue` 0x80348358, `__OSThreadInit` 0x80348230.
Scheduler (to bypass / make inert): `SelectThread` 0x803486dc, `__OSReschedule` 0x803488dc,
`OSEnableScheduler` 0x803483e8, `OSDisableScheduler` 0x803483a8, `__OSPromoteThread`
0x8034868c, `__OSGetEffectivePriority` 0x80348490.
Sync: `OSInitMutex` 0x803466d8, `OSLockMutex` 0x80346710, `OSUnlockMutex` 0x803467ec,
`OSTryLockMutex` 0x80346924, `OSInitCond` 0x803469e0, `OSWaitCond` 0x80346a00,
`OSSignalCond` 0x80346ad4. Message queues: `OSInitMessageQueue`, `OSSendMessage`,
`OSReceiveMessage` (addresses differ by region; resolve from the map at impl time).
Interrupt primitives already overridden in `runtime/overrides/sms_os_intr.cpp`.

OS globals: `RunQueue` 0x803EB198, `RunQueueBits` 0x80409E30, `IdleThread` 0x803EB298,
`DefaultThread` 0x803EB5A8. Current-thread pointer at the standard low-mem slot (0x800000C0
region) — verify at impl time. Guest code reads `OSGetCurrentThread` and thread fields, so
the guest-visible current-thread pointer and `OSThread` fields (priority/state) must stay
coherent even though scheduling is native.

`OSThread` (0x310): `context` @0x0 (OSContext 0x2C8), `state` @0x2C8, `suspend` @0x2CC,
`effective_priority` @0x2D0, `base_priority` @0x2D4, `queue` links @0x2DC.., `stackBase`
@0x304, `stackEnd` @0x308. `OSContext`: gpr @0x0, cr @0x80, lr @0x84, ctr @0x88, xer @0x8C,
fpr @0x90, fpscr @0x190, srr0 @0x198, srr1 @0x19C, gqr @0x1A4, psf @0x1C8.

## Target architecture
1. **Per-thread CPU context.** `CPUState` + `g_tail_jmp` become per host thread (the latter
   already thread-local). Each guest thread runs its recomp tree on its own native stack.
2. **CPU token.** A global lock; exactly one guest thread executes guest/Dolphin code at a
   time (preserves single-core semantics + Dolphin thread-safety). Each runnable thread calls
   `DeclareAsCPUThread` so the interpreter's assert holds; the token serializes them.
3. **GuestThread registry.** host `std::thread`, entry PC + arg, guest stack, `CPUState`,
   state (ready/running/blocked/suspended/dead), priority, per-thread condvar. Mapped to the
   guest `OSThread*` so guest reads stay coherent. Adopt the EmuThread as guest thread 0.
4. **Native primitives (overrides).** Lifecycle + message queues + mutex/cond implemented on
   host threads/condvars. Blocking = release token, condvar-wait, reacquire. The PPC
   scheduler (`SelectThread`/`__OSReschedule`/idle) is never run.
5. **Native idle/driver.** When no guest thread is runnable, advance Dolphin CoreTiming so a
   pending DSP/DVD/VI IRQ fires; its (guest) handler calls `OSWakeupThread` (our HLE) → marks
   a thread ready → scheduler grants it the token. This replaces the GC idle thread.

## Phasing
- **Phase 0 — foundation:** per-thread `CPUState`, CPU token, GuestThread registry, adopt
  thread 0, native scheduler core (pick highest-priority ready, grant token). Compiles, boots
  unchanged (still one thread until something creates a second).
- **Phase 1 — lifecycle + the audio block (first milestone):** override OSCreateThread /
  Resume / Suspend / Sleep / Wakeup / Yield / Exit / Join / InitThreadQueue / GetCurrentThread
  / SetThreadPriority + message queues; native idle/driver. **Goal: audio init completes with
  no spin, audio plays, matches pure-Dolphin.**
- **Phase 2 — sync + correctness:** mutex / cond / cancel / detach; diff-harness pass.
- **Phase 3 — preemption + decoupling:** approximate decrementer/IRQ preemption; remove
  Dolphin-scheduler reliance entirely; relax the token toward real parallelism as Dolphin
  hardware deps are replaced.

## Verification
Headless turbo (`SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO`, `SUNBRIGHT_RUN_SECONDS=N`) with
`SUNBRIGHT_AUTOSTART=1`: audio asset loads should match pure-Dolphin timing (no multi-second
gaps), no `exceeded step budget`, no `FATAL wild guest write` (the trap in
`runtime/memory_bridge.cpp`), no `JUTException`. A/B against `SUNBRIGHT_DISABLE_RECOMP=1`.

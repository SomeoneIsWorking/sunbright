# Native OS threading (PC-port model)

## Decision — host-thread substrate (settled 2026-06-03)
**Guest OS threads become native host threads. We do NOT run the game's PPC scheduler, and we
do NOT use fibers.** The GC OS HLE (thread lifecycle + sync primitives) is reimplemented in
native C++ so guest threads never enter Dolphin's interpreter.

Why host threads and not fibers (a fiber = a cooperative coroutine with its own stack, switched
by hand on one OS thread; both fibers and host threads give the "own stack you can park/resume"
that fixes the stall, so the *scheduler logic is identical either way*):
- The real Dolphin coupling is **guest code running under Dolphin's interpreter at all**, not
  the substrate. Removing that interpreter dependency (native scheduler + MSR/critical-section
  primitives) is the north-star work and is needed regardless of substrate — and once it's done,
  Dolphin's "one CPU thread" wall disappears and host threads are friction-free.
- Subsystems (SDL/present, audio out) **must** be real host threads; they signal guest waiters
  via a native mutex/condvar. Fibers would force a split model (guest=fibers + subsystems=threads,
  bridged); host-threads-everywhere is one uniform model and matches the target shape below.
- Avoids building a `swapcontext` layer we'd retire later.

Fibers would have worked for the interim and are a cheap fallback if host threads hit a real
Dolphin wall — the validated `nthr` scheduler *logic* + per-context switch-hook *mechanism* carry
over either way (only park/resume differs: condvar vs `swapcontext`).

## Target shape
The machine is single-core; **we keep it single-core** (one guest context runs at a time, via a
CPU token). "Native threads" = the older-console-port pattern, not parallel game logic:
- **Process main thread = SDL/subsystem thread:** window, input, present, vblank timing.
- **Game = a second host thread.** Its blocking waits (vblank, audio/DSP, DVD) are satisfied by a
  subsystem host thread **signalling a native condvar** — i.e. native NON-blocking versions of the
  blocking OS primitives. No dependence on Dolphin CoreTiming/interrupt timing for wakes.

## The problem this fixes
The GC OS multiplexes software threads onto one CPU via a PPC scheduler (`SelectThread` /
`__OSReschedule` + OSContext save/load). Our recomp runs that scheduler under Dolphin's
interpreter inside `run_jit_sync`, single-stepping until the call returns to LR. A blocking OS
call reschedules to the OS **idle loop**, which never returns to LR → `run_jit_sync` spins to its
500M-step budget. This is now a **fail-fast `abort()`** (exit 134, with a recomp backtrace) at the
budget exhaustion in `runtime/dolphin_hook.cpp` — the root cause. Continuing with the
half-executed state is what previously produced the downstream `FATAL wild guest write` /
`JUTException::run`. (Both fail-fast traps `setrlimit(RLIMIT_CORE,0)` before aborting so
systemd-coredump doesn't dump the multi-GB process — see [[abort-coredump-hang]].)

**Repro:**
`SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_AUTOSTART=1 SUNBRIGHT_RUN_SECONDS=N ./build/sunbright`
→ aborts at `run_jit_sync(80343fe4→803488c0)`. `SUNBRIGHT_DISABLE_RECOMP=1` (pure Dolphin)
handles the same scene fine, so it's our execution-model mismatch, not the game.

**The stall, concretely (via `SUNBRIGHT_OSWATCH`):** a producer thread (`80402aa8`) floods a
message queue at `0x803fd858` (lock → `OSSendMessage`+wake → unlock, ~5500×) while the receiver
(`804075c0`) sleeps on the queue's recv-wait list (`0x803fd860`). Producer = 16467/17719 sync
events, receiver only 916 — the receiver is **starved**: it's woken but almost never scheduled,
because we single-step one context under `run_jit_sync` instead of switching to the woken thread.

## Key constraints (verified)
- **We do not own the EmuThread loop.** `main_sdl.cpp` → `BootManager::BootCore` spins up
  Dolphin's EmuThread, which runs the CPU loop. Our only seam into guest execution is the
  `--wrap` on `JitTrampoline` → `SunbrightBridge::Run(pc)`, plus `run_jit_sync`/`call_ppc`/
  `tail_ppc`, all nested *inside* Dolphin's CPU loop. So a guest thread's native body runs the
  recomp tree / `run_jit_sync` itself and yields at OS block points.
- **Interception must be universal.** `SUNBRIGHT_OVERRIDE` is only consulted on the recomp path
  (`recomp_lookup`). The audio-init region runs under the **interpreter** (entry is a `call_ppc`
  to an interior addr `0x80343fe4` of `__OSInitAudioSystem`, not a registered recomp entry → once
  in `run_jit_sync` everything below is interpreted). Confirmed: a trace override on the lifecycle
  calls fired **zero** times during boot→audio-init. ⇒ the OS primitives must be intercepted in
  the `run_jit_sync` loop too, via a **dedicated native-OS override set** — NOT the general
  override table (the `sms_os_intr.cpp` interrupt overrides are recomp-path-only deferred-delivery
  and must NOT fire under the interpreter).
- **Super-call foothold differs per function (verified from the recomp disasm, 2026-06-03).**
  Earlier notes lumped `OSCreateThread` with `OSResumeThread` as un-super-callable; that is wrong.
  - `OSResumeThread` (0x80348ee8) DOES reschedule — it calls `SelectThread` (0x803486dc) — so a
    super-call would context-switch and never return. It must be **genuinely native** (no
    super-call). (A behaviour-neutral "trace then super-call" prototype on it regressed boot.)
  - `OSCreateThread` (0x80348948) is **reschedule-free** — it only calls `OSInitContext`
    (0x803440e8) + `OSDisableInterrupts`/`OSRestoreInterrupts`, then returns normally. So it CAN be
    super-called: the native primitive runs the recomp body to faithfully init the guest `OSThread`
    struct (state/priority/links/context/stack-canary + active-thread list), then additionally
    spawns the matching native host thread. Running it atomically is in fact more correct (it
    already wraps its critical section in the interrupt primitives).
- **Per-context PPC register file must be saved/restored at each switch.** `run_jit_sync` works on
  the single global `ppc`; a context that blocks inside it has live state there. This is the
  OSContext save/load the GC scheduler did, done natively — wire it via the step-1 switch hooks
  when a 2nd context first runs (step 4).
- **Dolphin's CPU-thread identity is per-host-thread and reassignable (verified 2026-06-03).**
  `Core::IsCPUThread()` reads `static thread_local bool tls_is_cpu_thread`, set/cleared by
  `DeclareAsCPUThread()`/`UndeclareAsCPUThread()` — no global thread id, no assert against
  re-declaration (`externals/dolphin/.../Core.cpp:129,293`). Since the CPU **token** guarantees
  exactly one guest thread runs at a time, each guest host thread can `DeclareAsCPUThread()` while
  it holds the token and `UndeclareAsCPUThread()` when it yields — so a guest host thread may still
  dip into `run_jit_sync` (the interpreter) for not-yet-native callees without tripping Dolphin's
  `IsCPUThread` asserts. **This is what makes the host-thread substrate viable in the interim**
  (the north-star goal of removing the interpreter dependency entirely is then an incremental
  optimisation, not a prerequisite). Mechanism for 4b: the native blocking primitives bracket
  `Undeclare → nthr::block → Declare`, and a freshly-scheduled guest thread body `Declare`s at
  entry / `Undeclare`s before it exits.

## Reference: GC OS API (GMSE01) — to reimplement natively
Lifecycle: `OSCreateThread` 0x80348948, `OSExitThread` 0x80348a68, `OSCancelThread` 0x80348b4c,
`OSJoinThread` 0x80348d08, `OSDetachThread` 0x80348e48, `OSResumeThread` 0x80348ee8,
`OSSuspendThread` 0x80349170, `OSYieldThread` 0x8034890c, `OSSleepThread` 0x803492e0,
`OSWakeupThread` 0x803493cc, `OSGetThreadPriority` 0x803494d0, `OSGetCurrentThread` 0x80348368,
`OSInitThreadQueue` 0x80348358, `__OSThreadInit` 0x80348230.
Scheduler (make inert / never run): `SelectThread` 0x803486dc, `__OSReschedule` 0x803488dc,
`OSEnableScheduler` 0x803483e8, `OSDisableScheduler` 0x803483a8, `__OSPromoteThread` 0x8034868c,
`__OSGetEffectivePriority` 0x80348490.
Sync: `OSInitMutex` 0x803466d8, `OSLockMutex` 0x80346710, `OSUnlockMutex` 0x803467ec,
`OSTryLockMutex` 0x80346924, `OSInitCond` 0x803469e0, `OSWaitCond` 0x80346a00,
`OSSignalCond` 0x80346ad4.
Message queues: `OSInitMessageQueue` ≈ 0x80346130, `OSSendMessage` 0x80346190,
`OSReceiveMessage` 0x80346258 (send enqueues/wakes recv-waiters/sleeps on full; receive
dequeues/wakes send-waiters/sleeps on empty).
Interrupt primitives already overridden in `runtime/overrides/sms_os_intr.cpp` (recomp-path-only).

OS globals: `RunQueue` 0x803EB198, `RunQueueBits` 0x80409E30, `IdleThread` 0x803EB298,
`DefaultThread` 0x803EB5A8. Current-thread ptr at low-mem `0x800000E4`
(`OSGetCurrentThread` = `lwz r3, 0xE4(0x80000000)`). Guest code reads `OSGetCurrentThread` and
thread fields, so the current-thread ptr and `OSThread` fields must stay coherent though
scheduling is native.

Struct layouts (confirmed from the recomp):
- `OSThread` (0x310): `context` @0x0 (OSContext), `state` @0x2C8 (4 = sleeping), `suspend` @0x2CC,
  `effective_priority` @0x2D0, `base_priority` @0x2D4, queue links @0x2DC.., `stackBase` @0x304,
  `stackEnd` @0x308.
- `OSContext` (0x2C8): gpr @0x0, cr @0x80, lr @0x84, ctr @0x88, xer @0x8C, fpr @0x90, fpscr @0x190,
  srr0 @0x198, srr1 @0x19C, gqr @0x1A4, psf @0x1C8.
- `OSMessageQueue` (32 B): `+0` queueSend (`OSThreadQueue` {head@+0,tail@+4}, blocked on full),
  `+8` queueReceive (blocked on empty), `+16` msgArray, `+20` capacity, `+24` firstIndex,
  `+28` usedCount.

SMS boot thread topology — captured live by native `OSCreateThread` up to the audio-init stall
(`[native_os] OSCreateThread #N`), in creation order:
| # | entry | prio | stack | note |
|---|---|---|---|---|
| 1–3 | 802c54b8 | 8  | 16 KB | 3-thread worker pool (×3, different param) |
| 4 | 802a9184 | 15 | 4 KB  | |
| 5 | 802a7878 | 17 | 64 KB | big stack, made during audio init (audio thread) |
| 6 | 80311170 | 2  | 4 KB  | high-priority (OSThread 804075c0 — the OSWATCH receiver) |
| 7 | 803171ec | 3  | 4 KB  | |
| 8 | 802b3264 | 14 | 4 KB  | |
| 9 | 802a7080 | 17 | 64 KB | reuses OSThread 803fcbe8 (#5's) — audio thread re-created |

(#6's `OSThread*` 804075c0 is the message-queue receiver the stall analysis named — see "The
stall, concretely" above.)

## Target architecture
1. **Per-thread CPU context.** `CPUState` + `g_tail_jmp` per host thread (the latter already
   `thread_local`). Each guest thread runs its recomp tree on its own native stack.
2. **CPU token.** A global lock; exactly one guest thread runs guest/Dolphin code at a time
   (single-core semantics + Dolphin thread-safety). While it holds the token a thread calls
   `DeclareAsCPUThread` (needed only for the residual interpreter calls; the endgame removes it).
3. **GuestThread registry.** host `std::thread`, entry PC + arg, guest stack, `CPUState`, state,
   priority, per-thread condvar; mapped to the guest `OSThread*`. Adopt the EmuThread as thread 0.
4. **Native primitives (overrides).** Lifecycle + message queues + mutex/cond on host
   threads/condvars. Blocking = release token, condvar-wait, reacquire. The PPC scheduler
   (`SelectThread`/`__OSReschedule`/idle) is never run.
5. **Native idle/driver.** When no guest thread is runnable, advance Dolphin CoreTiming so a
   pending DSP/DVD/VI IRQ fires; its guest handler calls `OSWakeupThread` (our HLE) → marks a
   thread ready → token granted. Replaces the GC idle thread.

Primitive mapping onto `nthr` + guest structs (blocking = native park; waking actually schedules):
- `OSSleepThread(q)`: enqueue self on guest queue `q`, set `OSThread.state`, `nthr::block(Blocked)`.
- `OSWakeupThread(q)`: for each guest thread on `q`, `nthr::make_ready(map[it])`.
- `OSSendMessage`/`OSReceiveMessage`: native enqueue/dequeue on the guest `OSMessageQueue`, then
  wake the opposite queue / block on own queue via the two above.
Keep guest-visible state coherent throughout (write `OSThread.state`, the `0x800000E4` ptr, queue
links) so game code inspecting them still sees the truth.

## Built + validated in isolation
`runtime/native_threads.{h,cpp}` (`SUNBRIGHT_NTHR_SELFTEST=1`, 3/3 PASS, stable over 8 runs):
**host-thread substrate** (one `std::thread` per guest thread, serialised by a single CPU token
= `g_running`; block = park on the thread's own condvar, no spin). Scheduler core —
`spawn`/`block`/`make_ready`/`run_and_wait`, highest-priority-Ready (FIFO among ties); the SMS
producer/consumer starvation pattern (lower-prio consumer not starved, and `concurrent=no` now
genuinely proves token mutual exclusion across real OS threads); and per-context switch hooks
(`set_switch_hooks(save, restore)` invoked around each token hand-off, agnostic to *what* is
saved). NOT yet wired into the game.

Converted from the validated fiber prototype on 2026-06-03 (the dropped substrate): park/resume
is now a host-thread condvar instead of `swapcontext`; the scheduler logic and switch-hook
mechanism carried over unchanged. **`g_tail_jmp` no longer needs the hook** — it is `thread_local`
and each guest thread is now a real host thread, so it is per-guest-thread for free. The switch
hook now carries only the **global PPC register file** (the single `ppc` that `run_jit_sync`
drives), which all guest threads share and must be swapped on each hand-off. The self-test's
`per_thread_ctx` case validates exactly this: a non-`thread_local` global slot, saved/restored
through the hooks, survives a yield to another thread.

## Integration checklist (each step independently verifiable; verify against a headless run)
1. ✅ **Host-thread substrate + per-context switch hooks** (done 2026-06-03 — see above).
   `nthr` is now host threads + CPU token + condvar park (fiber/`swapcontext` prototype retired);
   validated in isolation (`SUNBRIGHT_NTHR_SELFTEST`, 3/3, stable). Game path untouched →
   boot/render unchanged. Remaining for step 3: adopt the EmuThread as guest thread 0 and wire the
   token into the live game.
2. ✅ **Scoped native-OS interception (the gating sub-goal)** — mechanism done 2026-06-03.
   `runtime/native_os.{h,cpp}` is a dedicated native-OS override set (separate from the general
   `SUNBRIGHT_OVERRIDE` table and from the recomp-path-only `sms_os_intr.cpp` interrupt overrides).
   `call_ppc` consults it on the recomp path AND inside the `run_jit_sync` interpreter loop (the
   interpreter-path intercept converts ppc↔CPUState around the native fn and resumes the caller at
   its LR). Proven with the first genuinely-native primitive, **OSGetCurrentThread (0x80348368)**
   — behaviour-identical (`r3 = *(0x800000E4)`), so boot reaches the identical stall point; the
   `[native_os] first interpreter-path intercept at 80348368` log confirms the set fires under the
   interpreter (where the general override table never did). The load-bearing scheduling primitives
   (create/resume/sleep/wakeup/message queues) are added on this proven seam in steps 4–5.
3. ✅ **Wire the host-thread substrate into the game** (done 2026-06-03). `nthr::adopt_current`
   registers Dolphin's EmuThread as guest thread 0 holding the CPU token (no new host thread);
   the switch hooks (`nthr_ctx_save`/`restore` in `dolphin_hook.cpp`) swap a per-thread `CPUState`
   slot with the global `PowerPCState` on each hand-off. Adopted on first `SunbrightBridge::Run`
   via `std::call_once` (`sunbright_adopt_cpu_thread`). The EmuThread is already Dolphin's CPU
   thread, so no extra `DeclareAsCPUThread` for thread 0 (needed when a *different* host thread
   takes the token, step 4). Verified: boot reaches the **identical** stall point
   (`run_jit_sync 80343fe4→803488c0`, same step budget) — inert with only thread 0, no regression.
4. **Native `OSCreateThread`/`OSResumeThread`.**
   - 4a ✅ **`OSCreateThread` interception + faithful init + native registry** (done 2026-06-03).
     The native primitive super-calls the reschedule-free recomp body (`func_80348948`) for faithful
     guest `OSThread` struct init, then records the thread (`OSThread*`, entry, param, stack, size,
     priority) in a native registry and logs it. Verified: the boot threads are captured matching
     the known topology and boot reaches the identical stall (no regression). The real `nthr` host
     thread + body is spawned in 4b (a spawned thread can only actually run once thread 0 yields at
     a native block point, so spawning is landed with resume+blocking where it's end-to-end
     testable — avoids committing an unexercised thread body).
   - 4b **`OSResumeThread`** (genuinely native — it reschedules via `SelectThread`, no super-call):
     decrement the guest suspend count, and when runnable `nthr::make_ready` the mapped thread.
     Best landed WITH step 5, since a resumed thread only actually runs once thread 0 yields at a
     native block point. The body runs recomp from the entry PC on its own host stack; PPC register
     file saved/restored per context via the step-1 hooks; keep `0x800000E4` + state/priority
     coherent. A guest thread that runs on a non-EmuThread host thread needs `DeclareAsCPUThread`
     while it holds the token.
5. **Native `OSSleepThread`/`OSWakeupThread` → `OSSendMessage`/`OSReceiveMessage` → mutex/cond.**
   Blocking = release token, condvar-wait, reacquire; PPC scheduler never run. **Verify the
   audio-init stall clears** (no step-budget abort, no `JUTException`, audio assets load at
   pure-Dolphin speed).
6. **Subsystem wakes + preemption-if-needed.** SDL/present thread signals vblank waiters via the
   native condvar; audio-out thread signals audio waiters. Add a preemption nudge only if a
   busy-wait guest thread is found not to yield.

## Verification
Headless turbo (`SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1`, `SUNBRIGHT_RUN_SECONDS=N`,
`SUNBRIGHT_AUTOSTART=1`): audio asset loads should match pure-Dolphin timing (no multi-second
gaps), no `FATAL ... exceeded step budget` abort, no `FATAL wild guest write`, no `JUTException`.
A/B against `SUNBRIGHT_DISABLE_RECOMP=1`.

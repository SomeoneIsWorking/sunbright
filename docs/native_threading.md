# Native OS threading (PC-port model)

## Why
Sunbright is a **PC port**, not "Dolphin with ROM hacks." The GameCube OS multiplexes
software threads onto one CPU via a PPC software scheduler (`SelectThread` /
`__OSReschedule` + context save/load). Our recomp currently runs that scheduler under
Dolphin's interpreter inside `run_jit_sync`, single-stepping **until the call returns to
LR**. A blocking OS call (audio init waiting on the audio thread / a DVD read) reschedules
to the OS **idle loop**, which never returns to LR — so `run_jit_sync` spins to its
500M step budget. This is now a **fail-fast `abort()`** (with a recomp backtrace) at the
budget exhaustion in `runtime/dolphin_hook.cpp`: it is the root cause, and continuing with
the half-executed, inconsistent state is what previously produced the downstream
`FATAL wild guest write` / `JUTException::run`. (Both fail-fast traps suppress the core dump
via `setrlimit(RLIMIT_CORE,0)` before aborting — the default `core_pattern` pipes to
systemd-coredump, which would dump the multi-GB process and wedge for minutes; the printed
backtrace is the artifact we want. Exit 134.)

The fix is not to lean on Dolphin's emulated scheduler (a stopgap, `SUNBRIGHT_*_HANDOFF`),
but to make threading **native**: each guest OS thread becomes a host thread, and the OS
thread/sync primitives are HLE'd to native C++ (`std::thread`, `std::mutex`,
`std::condition_variable`). The PPC scheduler is then **never executed** — no busy-spin,
no step budget, no dependence on emulated timing. A blocked thread is simply a host thread
parked on a condvar, so its continuation lives on its own native C stack (no
"resume-mid-function" problem).

## Threading model (clarified 2026-06-03)
The goal is **not parallelism**. The emulated machine is single-core, and we keep it that
way: exactly one guest context runs at a time (the CPU token — already built). "Native
threads" here means the *older-console-port* pattern, not running game logic on many cores:

- The **process main thread = SDL/subsystem thread**: window, input, frame presentation,
  vblank/present timing — the host side.
- The **game runs as the second thread** (today: Dolphin's EmuThread running recomp).
- The game's **blocking waits are satisfied by a subsystem host thread signalling a native
  mutex/condvar.** The canonical example: a "wait for vblank" blocks the game thread on a
  condvar that the present/vblank thread signals once per frame. The audio/DSP and DVD waits
  follow the same shape — a subsystem host thread does the work and signals the waiter.

⇒ This **supersedes** the earlier "hard part" (a native idle/driver advancing Dolphin's
CoreTiming so an emulated DSP/DVD/VI interrupt fires to wake a thread). Instead the subsystem
host thread signals the wake **directly** via native sync — fewer moving parts, and it
removes a dependence on Dolphin's interrupt/CoreTiming plumbing. The CPU-token + cooperative
scheduler substrate still applies for the game's own (cooperative, non-parallel) threads;
what changes is *where wakes come from*.

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

## Blocking-primitive inventory + the audio-init stall (2026-06-03, via SUNBRIGHT_OSWATCH)
`SUNBRIGHT_OSWATCH` (a pure-observation trace in `call_ppc` + the `run_jit_sync` loop —
`runtime/dolphin_hook.cpp`) logged every OS sync call (object in r3, current `OSThread*`
read from low-mem `0x800000E4`) through the audio-init stall. Findings:

**The blocking/sync primitives SMS uses (GMSE01 addresses):**
- `OSSleepThread` 0x803492e0 / `OSWakeupThread` 0x803493cc — sleep on / wake a thread queue.
- `OSSendMessage` **0x80346190** / `OSReceiveMessage` **0x80346258** — message queue (verified
  by reading the recomp: send enqueues, wakes recv-waiters, sleeps on full; receive dequeues,
  wakes send-waiters, sleeps on empty). `OSInitMessageQueue` ≈ 0x80346130.
- `OSLockMutex` 0x80346710 / `OSUnlockMutex` 0x803467ec — heavily used, ~matched pairs.
- `OSWaitCond` 0x80346a00 / `OSSignalCond` 0x80346ad4 — rare.
- `OSJoinThread` 0x80348d08 — once.

**Struct layouts (confirmed from the recomp):**
- `OSThread`: `.state` @0x2C8 (4=sleeping), thread-queue link follows; current-thread ptr at
  low-mem `0x800000E4` (`OSGetCurrentThread` = `lwz r3, 0xE4(0x80000000)`).
- `OSMessageQueue` (32 B): `+0` queueSend (OSThreadQueue head/tail, threads blocked on full),
  `+8` queueReceive (threads blocked on empty), `+16` msgArray, `+20` capacity, `+24`
  firstIndex, `+28` usedCount. `OSThreadQueue` = {head@+0, tail@+4}.

**The stall:** a producer thread (`80402aa8`) floods a message queue at `0x803fd858`
(lock → `OSSendMessage`+wake → unlock, ~5500×) while the receiver (`804075c0`) sleeps on the
queue's recv-wait list (`0x803fd860 = 0x803fd858+8`). The producer accounted for 16467 of
17719 sync events; the receiver only 916 — i.e. **the receiver is starved**. It's woken but
almost never scheduled, because we run the PPC scheduler synchronously under `run_jit_sync`
(single-step one context) instead of actually switching to the woken thread. Pure-Dolphin
schedules it fine; this is our execution-model mismatch — exactly what native scheduling fixes.

### Native implementation plan for these primitives
Map each onto `nthr` (the host-thread scheduler) + the guest structures, so the actual
blocking is a native park and waking actually schedules the woken thread:
- Maintain a `guest OSThread* ↔ nthr::GuestThread*` map; keep guest-visible state coherent
  (write `OSThread.state`, the current-thread ptr at `0x800000E4`, queue links) so game code
  that inspects them still sees the truth.
- `OSSleepThread(q)`: enqueue self on guest queue `q`, set state, `nthr::block(Blocked)`.
- `OSWakeupThread(q)`: for each guest thread on `q`, `nthr::make_ready(map[it])`.
- `OSSendMessage`/`OSReceiveMessage`: native enqueue/dequeue on the guest `OSMessageQueue`,
  then wake the opposite queue / block on own queue via the two above.
- `OSLockMutex`/`OSUnlockMutex`, `OSWaitCond`/`OSSignalCond`: same shape (own thread queues).
- `OSCreateThread`/`OSResumeThread`: register a `nthr` guest thread whose body runs recomp
  from the entry PC; `OSResumeThread` = `make_ready`. Adopt the boot/EmuThread as thread 0.
- **Open integration risk:** a spawned host thread that runs a non-recomp call goes through
  Dolphin's interpreter, which asserts `Core::IsCPUThread()` and uses `s_core_mutex`. The
  token serializes execution (one at a time), so the token holder must present as the CPU
  thread (`DeclareAsCPUThread` / hold `s_core_mutex`). De-risk this in isolation before wiring
  it to the game. Cooperative switching also assumes threads yield via OS primitives; if a
  thread busy-waits without blocking, add a preemption nudge (see preemption note).

## Execution-context decision: fibers on the EmuThread vs. host-thread-per-guest-thread
The `nthr` scheduler *logic* (priority pick, Ready/Blocked, the producer/consumer hand-off)
is validated and **independent of how a guest context is parked/resumed**. But *where* guest
code physically runs is a real fork, forced by a hard Dolphin constraint:

> Dolphin's interpreter asserts `Core::IsCPUThread()` (a thread-local set only on the
> EmuThread) and guards core state with `s_core_mutex`. Non-recomp guest calls go through
> that interpreter. So whatever runs guest code must satisfy Dolphin's "I am the CPU thread"
> identity.

- **Host-thread-per-guest-thread + token** (what the substrate currently uses): each guest
  thread is a real OS thread; the token serializes them. *Risk:* every such thread must
  present as Dolphin's CPU thread (`DeclareAsCPUThread`, and likely hold `s_core_mutex`), and
  Dolphin may have other single-CPU-thread invariants (CoreTiming/GPU-Fifo affinity) that
  only surface when driven from a non-EmuThread. Medium-high, discoverable only by trying.
- **Fibers on the EmuThread** (lean toward this): guest threads are fibers (e.g.
  `swapcontext`) multiplexed on the **one** EmuThread, switched cooperatively at block points.
  Dolphin always sees its single CPU thread → the whole `IsCPUThread`/`s_core_mutex` risk
  class **disappears**. Each fiber keeps its own stack, so a blocked guest's recomp
  continuation is preserved (same property we need). Subsystems stay *real* host threads that
  signal a fiber Ready via a condvar across the EmuThread boundary.

**Recommendation:** fibers-on-the-EmuThread for guest execution; real host threads only for
subsystems (SDL/present/vblank, audio out) that signal wakes. The validated `nthr` scheduler
logic + self-tests carry over unchanged; only the park/resume layer swaps condvar↔swapcontext.
**To reconcile when implementing:** the existing tail-branch handoff uses `siglongjmp`
(`g_tail_jmp`) — verify `siglongjmp` interacts correctly with fiber contexts (longjmp must
stay within a fiber's own stack), or route the tail handoff through the fiber switch instead.
This is the next decision to settle before wiring native threading into the game.

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

## Finding (2026-06-02): interception must be universal
Overrides (`SUNBRIGHT_OVERRIDE`) are only consulted on the **recomp path** (`recomp_lookup`
in `call_ppc` / `Run`). When code runs under the **interpreter** inside `run_jit_sync`, the
interpreter executes raw PPC and never consults `override_lookup` — so the override is
bypassed. Confirmed live: with a trace override on `OSCreateThread`+lifecycle, **zero** fired
during boot→audio-init, because that whole region runs under the interpreter. The entry into
the interpreter here is a `call_ppc` to an **interior** address of `__OSInitAudioSystem`
(`0x80343fe4`, mid-function — not a registered recomp entry, so `recomp_lookup` misses), and
once in `run_jit_sync` everything below (including `OSCreateThread` and the scheduler) is
interpreted. The blocking OS calls we must replace happen *under the interpreter*.

⇒ **Prerequisite for native threading:** the OS primitives must be intercepted regardless of
execution backend. Recomp-native option (no new Dolphin coupling): make the `run_jit_sync`
loop consult overrides — before stepping, if `ppc.pc` is an override, run it instead of
single-stepping.

### Step result (2026-06-02): interception works, but super-call is the wrong shape
Prototyped `run_jit_sync` consulting `override_lookup` per step, with `sms_os_threads.cpp`
overriding the lifecycle calls as a *trace that super-calls the recomp body*. Results:
- **Interception fires** under the interpreter — the trace logged, mechanism validated.
- **Captured SMS thread topology** (created during boot, via `OSCreateThread`):
  | thread | entry | prio | stack | caller |
  |---|---|---|---|---|
  | 8042fe60 | 802c54b8 | 8  | 16 KB | 802c5380 | ← 3-thread worker pool
  | 80434300 | 802c54b8 | 8  | 16 KB | 802c5380 |   (same entry/caller, diff param)
  | 80438700 | 802c54b8 | 8  | 16 KB | 802c5380 |
  | 80575ec8 | 802a9184 | 15 | 4 KB  | 802a9410 |
  | 803fcbe8 | 802a7878 | 17 | 64 KB | 802a7854 | ← big stack, made during audio init (likely audio thread)
- **But it regressed boot** (loaded only `nintendo.szs`, then idled in the scheduler).
  Root cause (confirmed — excluding the interrupt primitives did NOT fix it): super-calling
  the recompiled `OSResumeThread`/`OSCreateThread` bodies runs `__OSReschedule` → a context
  switch (`rfi` to another thread) that **never returns to its caller** — the same call-model
  break this whole effort exists to fix. ⇒ **The OS thread/scheduler functions cannot be
  traced via super-call; they must be replaced with genuinely native logic** (no super-call),
  in any context. Both experimental pieces were reverted.

Revised plan: there is no behaviour-neutral "trace then super-call" foothold for these
functions. The first real unit is a **native implementation of one primitive + scoped
interception, together** — interception consulting a *dedicated native-OS override set*
(NOT the general override table; the interrupt-primitive overrides in `sms_os_intr.cpp` are
recomp-path-specific deferred-delivery and must NOT fire under the interpreter).

## Phasing
- **Phase 0a — scoped interception + first native primitive (together):** `run_jit_sync`
  consults a *dedicated native-OS override set* (not the general override table) so native
  OS primitives fire under the interpreter; deliver alongside the first genuinely native
  primitive (no super-call). Interception alone is not a standalone step — there is no
  behaviour-neutral super-call foothold (see Step result above).
- **Phase 0 — foundation:** per-thread `CPUState`, CPU token, GuestThread registry, adopt
  thread 0, native scheduler core (pick highest-priority ready, grant token). Compiles, boots
  unchanged (still one thread until something creates a second).
  - ✅ **Done (2026-06-02): cooperative scheduler core**, `runtime/native_threads.{h,cpp}`:
    `spawn`/`block`/`make_ready`/`run_and_wait`, highest-priority-Ready (FIFO among ties),
    parking with no spin. Proven by `nthr::self_test()` (`SUNBRIGHT_NTHR_SELFTEST=1`).
  - ✅ **Done (2026-06-03): converted to the fiber model** (the decided execution context).
    Guest threads are now `ucontext` fibers multiplexed on ONE thread (`swapcontext`),
    single-threaded cooperative ⇒ **no locks**. Validated by the same API tests: round-robin,
    the SMS producer/consumer starvation pattern (lower-prio consumer not starved), and a
    fiber + `siglongjmp`-with-per-fiber-jmpbuf test — 5/5 PASS. Keeps everything on one
    thread, so Dolphin's CPU-thread identity is intact when this is wired in. Not yet wired.
  - Next: adopt the boot thread as guest thread 0 (link it to the guest `OSThread*` at the
    low-mem current-thread slot) and run a real recompiled function body on a spawned guest
    thread under the token — then layer the OS-primitive overrides (Phase 0a/1).
- **Phase 1 — lifecycle + the audio block (first milestone):** override OSCreateThread /
  Resume / Suspend / Sleep / Wakeup / Yield / Exit / Join / InitThreadQueue / GetCurrentThread
  / SetThreadPriority + message queues; native idle/driver. **Goal: audio init completes with
  no spin, audio plays, matches pure-Dolphin.**
- **Phase 2 — sync + correctness:** mutex / cond / cancel / detach; diff-harness pass.
- **Phase 3 — preemption + decoupling:** approximate decrementer/IRQ preemption; remove
  Dolphin-scheduler reliance entirely; relax the token toward real parallelism as Dolphin
  hardware deps are replaced.

## Next session — ordered integration checklist (each step independently verifiable)
The fiber scheduler (`nthr`) is built + validated in isolation. Wiring it into the game is
the next chunk; do it in this order, verifying each against a headless run before the next:

1. ✅ **Per-fiber tail-jmp (done 2026-06-03).** `g_tail_jmp` in `dolphin_hook.cpp` is
   `thread_local`; all fibers share one host thread's TLS, so a resumed fiber would read
   another fiber's stale jmpbuf. Built the generic mechanism in `nthr`: each fiber has one
   opaque `user` slot + a registered `set_switch_hooks(save, restore)` pair invoked around
   every `swapcontext` (`save(self)` before a fiber yields in `block()`, `restore(t)` before
   a fiber is granted the CPU in `run_and_wait()`). `nthr` stays agnostic to *what* is saved;
   the runtime will register hooks that move `g_tail_jmp` into/out of the slot when fiber 0 is
   adopted (step 2). Proven by the `per_fiber_tls` self-test (`SUNBRIGHT_NTHR_SELFTEST=1`,
   5/5 PASS): two fibers write distinct markers into a shared `thread_local`, yield, and each
   observes its own marker on resume (FAIL without the hooks). Game path doesn't touch `nthr`
   yet → boot/render unchanged (intro renders; only the known `run_jit_sync(80343fe4)`
   audio-init stall remains — the thing steps 4–5 fix).
2. **Adopt the boot context as fiber 0.** The EmuThread's recomp execution becomes nthr fiber
   0 holding the "CPU". Restructure so `run_and_wait` (or an equivalent) drives execution.
   This is the invasive step — verify boot/render unchanged before adding a 2nd fiber.
3. **Scoped interception.** `run_jit_sync` (and `call_ppc`) consult a *dedicated native-OS set*
   (NOT the general override table — keep the interrupt overrides recomp-path-only) so the
   native primitives fire under the interpreter too. Verify with `SUNBRIGHT_OSWATCH`.
4. **Native `OSCreateThread`/`OSResumeThread`.** Register a fiber whose body runs recomp from
   the entry PC; map guest `OSThread*` ↔ `nthr::GuestThread*`; keep `0x800000E4` + state
   coherent. Verify the 5 boot threads spawn as fibers.
5. **Native `OSSleepThread`/`OSWakeupThread`, then `OSSendMessage`/`OSReceiveMessage`,** then
   mutex/cond — each on the guest structs (offsets above) + `nthr::block`/`make_ready`.
   Verify the audio-init stall clears (no `FATAL ... exceeded step budget` abort, no `JUTException`, audio
   assets load at pure-Dolphin speed).
6. **Subsystem wakes + preemption-if-needed.** SDL/present thread signals vblank waiters; add
   a preemption nudge only if a busy-wait thread is found not to yield.

## Verification
Headless turbo (`SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO`, `SUNBRIGHT_RUN_SECONDS=N`) with
`SUNBRIGHT_AUTOSTART=1`: audio asset loads should match pure-Dolphin timing (no multi-second
gaps), no `FATAL ... exceeded step budget` abort, no `FATAL wild guest write` (the trap in
`runtime/memory_bridge.cpp`), no `JUTException`. A/B against `SUNBRIGHT_DISABLE_RECOMP=1`.

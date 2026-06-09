# Native OS threading (PC-port model)

> ## 🟢 ACTIVE BUILD — native scheduler is LIVE (2026-06-05, increments 1–4 landed)
> Per the user directive "do it incrementally even though it will break", the nthr scheduler is now
> ENABLED and owns GC threading. Landed + committed:
> 1. **Lazy incremental adoption** (`sunbright_adopt_all_gc_threads`, af8d57d) — re-scans the active
>    queue on each native-OS call, registers every GC thread (worker pool + audio) into nthr as the
>    OS creates them. Inert alone.
> 2. **Native primitives + scheduler neutralized** (c4d9e39) — OSCreateThread→spawn parked nthr,
>    OSResumeThread→make_ready, OSSleepThread→nthr park (GC fallback removed), OSWakeupThread→
>    make_ready, **__OSReschedule→no-op** (nthr switches at block points, the GC must not also switch
>    the one global ppc). Verified: all boot threads spawn, run their bodies on their own host stacks,
>    switch cooperatively, and park.
> 3. **Native idle/IRQ driver** (`nthr_idle_driver`, 5efbd35) — when all threads block, advance
>    CoreTiming (Idle+Advance) + CheckExceptions to deliver the pending device IRQ; its ISR calls
>    native OSWakeupThread→make_ready. Loops until a thread wakes; deadlock fail-fasts.
> 4. **Clean recoverable MSR per IRQ** (a3b3887) — take each interrupt on a fixed IDLE_MSR
>    (EE|ME|IR|DR|RI), not the degrading parked MSR, else RI=0 → "Non-recoverable Exception 4" → TRK
>    halt. Fixed.
>
> **The post-THP recompiled-scheduler crash is GONE.** Boot now runs the native scheduler through
> early thread + audio init into asset loading (nintendo.szs) and early rendering (thread0 reaches
> `JDrama::TDisplay::endRendering` 802f80d0).
>
> **FILE-LOAD PIPELINE STALL — FIXED (2026-06-09) by a native DVD read service.**
> ROOT CAUSE: under cooperative native scheduling the GC DVD command FSM stalls after the FIRST
> transfer. A thread calls `DVDRead`→`DVDReadAbsAsyncPrio` (8034da6c), which queues a DVDCommandBlock
> and relies on the DI hardware interrupt + the OS command-queue "dispatch next on completion" to
> advance. JIT-only threads (e.g. the audio thread, entry 802a7878 — NOT in functions.h) run wholly
> under Dolphin's interpreter, where the general SUNBRIGHT_OVERRIDE table is never consulted, so they
> hit the raw GC FSM; the dispatch-next step never fires (no preemption), so the 2nd read's DVDLowRead
> is never kicked. Confirmed: only `data/nintendo.szs` transferred under native vs pure-Dolphin reading
> `sequence.arc`/`*.aw`/`mario.szs`/… next; the audio thread livelocked in DVDRead's wait loop (8034be2c
> polling block->state@0xc), and thread0 reached `mountStageArchive`→`mountFixed(null)` → wild read.
> FIX (own the path, not the hardware): `runtime/overrides/native_dvd.cpp` registers a native
> `DVDReadAbsAsyncPrio` on the **native_os seam** (fires on BOTH the recomp call path AND the
> interpreter, so JIT-only threads are covered). It reads `length` bytes at the absolute disc offset
> straight from our own read-only `DiscIO::Volume` into guest RAM (`CopyToEmu`, raw bytes like the DVD
> DMA), fills the command block (state=END, transferred=length), and invokes the completion callback.
> No DI registers, no interrupt, no queue dispatch. VERIFIED: audio thread now streams its archive
> reads; thread0's stage archive mounts; boot runs deep into audio init (deterministic, 2 runs).
> Block layout (from the 8034da6c prologue): 0x08 cmd, 0x0c state, 0x10 disc-offset, 0x14 len, 0x18
> dest, 0x20 transferred, 0x28 prio. Args r3=block,r4=addr,r5=len,r6=offset,r7=prio,r8=cb.
>
> **Audio DSP init null FX-line buffer — FIXED (2026-06-09) by priority preemption on OSResumeThread.**
> ROOT CAUSE: `JAIBasic::initDriver` (80301a28) calls `JASystem::AudioThread::start` (803113c4), which
> `OSCreateThread`+`OSResumeThread`s the higher-priority audio driver thread and RETURNS WITHOUT
> WAITING — relying on the GC scheduler PREEMPTING to it at the resume (`__OSReschedule` inside
> `OSResumeThread`). That driver thread runs `Driver::init`→`DSPInterface::initBuffer` (80314f50) which
> ALLOCATES the DSP FX-line array (global `[r13-0x5c04]`). Our scheduler was cooperative (no preempt,
> `__OSReschedule` no-op'd), so the main thread raced ahead to `JAIData::initData`→`setFXLine` and wrote
> through the not-yet-allocated FX array (null `getFXHandle(0)`) → wild 16-bit write to ea=0. FIX:
> `os_resume_thread` now yields (`nthrt_yield_current`, stay Ready) when the resumed thread is STRICTLY
> higher priority than the caller — replicating GC reschedule-on-resume without the GC scheduler
> (nthr makes the decision; `pick_next` already honors priority). VERIFIED: `fxbase` is now allocated
> (805fe7e0) when initData runs; the setFXLine null crash is gone. Diag: SUNBRIGHT_DBG_AUDIO traces
> Driver::init / initBuffer / initDriver / initData with the FX-base value + current thread.
>
> **Audio-init run_jit_sync engine spin — FIXED (2026-06-09) by a native OSYieldThread.** ROOT CAUSE:
> the audio DSP-init `802c6830` (JIT-only, reached via a computed `bclrl` from `__OSInitAudioSystem`,
> run under run_jit_sync) calls **`OSYieldThread`** (8034890c) after kicking the DSP. OSYieldThread was
> NOT on the native_os seam, so it ran the GC `SelectThread` — which under native threading finds NO
> runnable GC thread (nthr owns them; GC run-queue empty), sets `OS_CURRENT_THREAD=0`, and spins the GC
> idle loop (`SelectThread+0x138` = 80348814) on `[r13-0x59c0]` forever → run_jit_sync step-budget abort.
> Guest chain confirmed it (`80005628→…→__OSInitAudioSystem→802c6830(+0xdc)→OSYieldThread→SelectThread`).
> FIX (satisfies all three user directives — port to native, no run_jit_sync engine spin, leave gameplay
> in JIT): `os_yield_thread` on the native_os seam → `nthrt_yield_current()` (yield the token but stay
> Ready; `block(State::Ready)` re-stamps ready_seq = round-robin, matching OSYieldThread). Engine code
> stays JIT-only; no recompile. VERIFIED: the run_jit_sync spin is gone, audio init completes, boot runs
> the full headless duration with no FATAL.
>
> **NEW frontier (2026-06-09) — null sub-object in the TApplication boot sequencer (after audio init).**
> With audio init completing, boot fails with `Jit64: ISI exception at 0x00000000` and the GC exception
> reporter runs (thread0 spins in `JUTException::readPad`/`waitTime`/`__div2i`, speed ~0.06×; the
> "TMarDirector teardown" read earlier was a sparse-symbol mislabel of the reporter's own delay loop).
> REAL fault from the Dolphin MMU log: `PC=0x802a6160` reads `[0x0]` then `[0x64]` → ISI at 0. Disasm:
> the boot sequencer **function at 802a5f50** (`this` in r3→r31; the reference lumps it under
> `mountStageArchive`) does a C++ virtual call `[r31+4]->vtable->method@0x64()` at 802a615c–616c where
> `[this+4]` is NULL — a sub-object (director/scene) the state machine expects at this state. It is
> NEVER stored in this function (no `stw _,4(r31)`), so it is created elsewhere and that creation was
> skipped/failed under native scheduling. This is real forward progress: the earlier crash was at
> 802a6338 (mountFixed null) EARLIER in the same sequencer; it now advances past audio init to a later
> state.
> MECHANISM (refined): 802a5f50 is a **jump-table state machine** (`bctr` @802a63e0, indexed on state
> `[r13-0x6030]`). One case CREATES `[this+4]`: `new 0x48`-byte object (@802a63f8) → init → `stw r25,
> 4(r31)` (@802a6410; also 6464/64ec/6554/65bc/6630/667c — several states set it). Other cases CALL
> `[this+4]->vtable->method@0x64()` (@802a60ec, guarded on state bit0; and @802a615c = the fault). So a
> CALL-state ran before the CREATE-state populated `[this+4]` → null vtable → ISI. `[this+4]` is the
> per-scene director/loader the sequencer drives.
> A/B CONFIRMED (REPL `/poll`, native vs `SUNBRIGHT_DISABLE_RECOMP=1`): `this` = global **803e9700**
> (the app/manager; was r26 in the first crash too). Pure-Dolphin: `[803e9704]` (this+4) = **80902a40**
> (director created), state `[8040e190]` = **3**. Native at crash: `[803e9704]` = **0**, state = **0**.
> So under native the boot sequencer's state never advances to the create-state, so the director is
> never `new`'d, yet a later call-state still runs → null. Next: trace how Dolphin advances state
> 0→3 (REPL `/trace?a=8040e190` during early boot) and find the per-iteration condition the native
> path fails to satisfy (likely an async load-complete the sequencer polls). Tools: REPL `/poll`,
> `/trace`, `/r`, `/fn`, `/stack` (see CLAUDE.md). NB: run ONE instance at a time + `kill -9` the exact
> pid between runs (`pgrep -x sunbright`, NOT `-f` which self-matches the shell); stacked headless
> Vulkan instances exhaust GPU memory (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) — use `SUNBRIGHT_BACKEND=Software`
> for state-inspection A/B (no GPU).
>
> PINNED TO ONE BYTE (2026-06-09, via Software-backend A/B + REPL `/poll`). The director ([this+4]) is a
> **`TMarDirector`** (vtable 803df0c8). It is created by a SEPARATE state machine — `func 802a6398`
> (the reference lumps it under mountStageArchive; `this`=803e9700, called from 80005624) — which loops
> reading a STATE BYTE at **`[this+8]` = `[803e9708]`** (`lbz r0,8(r31)` @802a67c8), dispatches a
> 10-entry jump table @803df424 for states 0–9 (7=exit), and on its COMMON TAIL @802a6650 calls
> `802a5f50` which USES `[this+4]`. So every iteration drives the director after the switch.
> A/B at the same boot point: pure-Dolphin `[803e9708]`=**0x05** (valid; director `[803e9704]`=80902a40,
> app fields `[+c]=[+10]=0x0f00` populated); native `[803e9708]`=**0x18** (=24, OUT of range → default
> path, never the create-state; director=0, app fields 0 = UNINITIALIZED). So native's director-creator
> state byte holds garbage 0x18 and the app object never got initialized — yet 802a5f50 still runs and
> derefs the null director. NEXT: find who writes `[803e9708]` (the app ctor's initial value + the
> per-state advance) and why native lands on 0x18 instead of stepping 0→…→5 — i.e. which create-state's
> work (or its gating condition) native skipped. Search generated/ for stores to app+8 / the 803e9700
> ctor.
>
> TRACED FURTHER (2026-06-09) — this is the **GC boot-logo state machine**, and 802a5f50 RETURNS the
> next state. Jump table @803df424: case2=802a63e4 `SMSSetupGCLogoRenderingInfo` (calls `VIGetTvFormat`),
> case3=802a63f0 = the `new TMarDirector`+store `[this+4]`. The state byte `[this+8]` is written by
> EXACTLY ONE instruction: `stb r30,8(r31)` @802a6794, where `r30 = (802a5f50 return) & 0xff` (mask
> @802a6654). So `802a5f50` (the per-iteration director/logo driver, called @802a6650) computes & returns
> the NEXT logo state. A/B: at logo-state 2, Dolphin's 802a5f50 returns a value stepping toward 3→5
> (creates the director); native's returns **0x18** → out-of-range → default → never creates the director,
> then a later iteration calls 802a5f50 at state 0x18 and derefs the null `[this+4]` → ISI. NOT autostart
> (reproduces without it). NEXT: RE `802a5f50`'s return — find which sub-call/condition yields 0x18 at
> logo-state 2 under native (its loop body starts @802a5f98 with a `[this+0x1c]`=TDisplay vtable call@8).
> 0x18 is likely a "skip/abort logo" code returned when an early-frame/VI/load precondition isn't met —
> the same native-scheduling ordering class as every prior wall this session.
>
> GATE LOCATED, but static RE stalls (2026-06-09). In 802a5f50 at logo-state 2 (`lbz [this+8]`==2 check
> @802a60a0): it calls **`8034c374(r30)`** @802a60b4 — if NON-ZERO → `8034c374`... then `li r29,3`
> @802a60cc (advance to state 3 = `new TMarDirector`); if zero → branches away, no create. So
> `8034c374`'s return is the condition native fails (the symbol is a sparse-table mislabel — it
> disassembles as a 3-instr stub `stw r4,[r13-0x5918]; li r3,1; blr`, i.e. one arm of a switch/lookup,
> so the REAL callee/return needs runtime confirmation). CONTRADICTION blocking pure static analysis:
> 802a5f50 returns `r29`, whose only writes are `{0,1,3,4,7}` or `r29=r3` from the `[this+4]`-method
> call @802a6170 — so `0x18` could only come from that null-`[this+4]` path, which would CRASH, yet
> native returns 0x18 without crashing there. 802a5f50 is recursive (the crash stack had several of its
> frames) and juggles TWO state vars ([this+8]=803e9708 AND [r13-0x6030]=8040e190), so the post-crash
> snapshot ≠ execution-time values. RESOLUTION NEEDED: runtime tracing of 802a5f50's per-call inputs/
> return (proposed: a REPL-readable ring buffer written by a thin observer on 802a5f50/802a6398 — keeps
> data in the REPL, not env-gated stderr). That will show directly what `8034c374` returns and why
> native picks the non-create / 0x18 branch.
>
> **REFRAME (2026-06-09) — the register file is CORRUPT; chase r31, not the state-machine logic.**
> New tooling: a Dolphin-side invalid-access trap (commit 874e90b) makes Dolphin's "Invalid
> read/write" MMU PanicAlert fatal at the ORIGINATING access (it used to swallow it and return
> garbage, so the game limped on and died later — that downstream death is the `802a6160` ISI the
> entries above chased). Running headless, the trap now aborts at the TRUE originator:
> `Invalid read from 0x28040060, PC=0x802a6338` (in `mountStageArchive`, same function as 802a5f50 /
> 802a6160). Disasm of the fault:
> ```
> 802a6334: lwz r3, 0x1c(r31)    ; r3 = [r31+0x1c]
> 802a6338: lwz r6, 0x60(r3)     ; FAULTS: ea = 0x28040060
> ```
> Execution-time regs at the fault (Dolphin PPCState, reliable — this is the live faulting state, NOT
> a post-crash REPL snapshot): **r31 = 0x802a6324** — a `.text` CODE address, never a valid `this`.
> `[0x802a6324+0x1c] = [0x802a6340]` = the instruction word `0x28040000` (the `cmpli` at 802a6340),
> so r3 = 0x28040000 and `[r3+0x60]` = the wild read. So **r31 (a non-volatile GPR holding `this`) is
> clobbered to a code address.** Note 0x802a6324 is exactly the return addr of the **virtual call**
> @802a6320 (`lwz r12,0(r3); lwz r12,0xc(r12); mtlr r12; bclrl`) — i.e. r31 survives that call
> corrupted. This means the `[803e9708]=0x18` garbage state the entries above analyzed is almost
> certainly a CONSEQUENCE of register/`this` corruption, not the root — static analysis of the state
> machine assumed a good register file that isn't there. Since `SUNBRIGHT_DISABLE_RECOMP=1` boots, the
> clobber is in our recomp/native-threading hybrid (a non-volatile-register-preservation bug across a
> call boundary), not the game. NEXT: trace r31 across the calls inside mountStageArchive (the virtual
> call @802a6320 and the recursive 802a5f50 calls @802a6650) to find which RETURN first leaves r31 ≠
> its caller value — that callee's recomp↔interp boundary drops the non-volatile. Confirm whether the
> clobbering callee is recomp or JIT, and whether the hybrid copies guest non-volatiles correctly on
> return.
>
> **(superseded sub-note) DETERMINISTIC null archive (heap-not-ready / thread ORDERING, not a race).**
> A thread crashes with a null `this` in `JKRMemArchive::mountFixed` ⇒ `new JKRMemArchive` returned
> NULL (heap alloc failed). The fault regs (r26=803e9700, r5=0xffff) point at the **audio thread
> 803fcbe8** (entry 802a7878) running `SMSMountAramArchive` (802a797c) — i.e. the audio thread mounts
> its ARAM archive before its heap/prereqs are ready. It's DETERMINISTIC (same point every run), so
> it's an ORDERING bug from cooperative scheduling, not corruption: the audio thread is made Ready by
> OSResumeThread and, with `__OSReschedule` no-op'd (no preemption) + whatever yield thread0 takes,
> runs before the init that sets up the audio archive heap. **Ruled out:** the JKRThread decomp
> workers (same null with SUNBRIGHT_JKR_SYNC=1); the context save (gpr/fpr/lr/ctr/xer/cr/gqr/srr all
> saved; **MSR now saved per-thread too**, fixed b3c2378 — a real critical-section bug, but not this
> crash). A separate DBG_CONSOLE run hit `double free` — there IS latent heap corruption too.
> **Next, in order:** (1) confirm WHICH thread faults + its guest call chain (the recomp doesn't track
> guest pc mid-tree — add a guest-stack walk to the wild-read trap, or watch OSResumeThread→first-run
> ordering); (2) check whether the audio thread needs a wait/sequencing that the GC scheduler provided
> via preemption and `__OSReschedule` no-op removed — may need to honor a reschedule point or a
> message/cond wait; (3) verify the audio heap/ARAM init completed before the audio thread runs.
> Diagnostics: SUNBRIGHT_DBG_IDLE (per-IRQ context), SUNBRIGHT_DBG_SWITCH (switches), SUNBRIGHT_DBG_ADOPT.

> ## 🟢 REOPENED — full native threading is THE direction (2026-06-05, user-directed)
> The 2026-06-03 "superseded" banner below is itself superseded. The hybrid that replaced this —
> **recompile the GC scheduler** (model `rfi`/`mtmsr` in the recompiler, commits `fb76ced`/`118263f`,
> and the `OSLoadContext`→Dolphin handoff) — is now the SOURCE of boot crashes: recompiling the
> exception/interrupt/context machinery (real-mode `rfi`, BAT/MMU, full Gekko context save/restore)
> can't be done faithfully, and a non-recomp thread-proc that **blocks** spins under
> `interp_run_until` to the 500M-step budget (`run_jit_sync(802c6830→…)`). Pure Dolphin handles all
> of it; it's our hybrid interaction. See memory `blocking-call-interp-spin`.
>
> **User directive (2026-06-05):** own threading natively; Dolphin is for **rendering + oracle only**,
> NOT for OS/threading. So we UN-SHELVE the `nthr` host-thread scheduler below and FINISH it. The
> substrate (`runtime/native_threads.{h,cpp}`, std::thread + CPU token + condvar — macOS-safe, no
> `ucontext`) and the native-OS primitive set (`runtime/native_os.cpp`) are already built and were
> only DISABLED, not removed.
>
> ### What's left to finish (the two documented blockers + the conflict removal)
> 1. **Lazy takeover-time adoption of ALL existing GC threads.** Confirmed 2026-06-05: at the first
>    recomp entry (where `sunbright_adopt_cpu_thread` runs) the GC active-thread queue is EMPTY and
>    `0x800000E4`=0 — takeover precedes `__OSThreadInit`. So adoption can't be a one-time snapshot
>    there; it must fire LAZILY at the first native-OS scheduling primitive, when threads exist.
>    Enumerate via the **GC `__OSActiveThreadQueue`** (extracted from `OSCreateThread`'s
>    `__OSLinkActiveThread`): head @ `0x800000DC`, tail @ `0x800000E0`; `OSThread.linkActive.next`
>    @ `+0x2FC`, `.prev` @ `+0x300`. Walk head→next. Map each `OSThread*`→`nthr` thread; the current
>    (`0x800000E4`, state=2 RUNNING) is the token holder, others get a host body that restores their
>    `OSContext` and resumes at `srr0` (`+0x198`) when scheduled. (Diagnostic:
>    `SUNBRIGHT_DBG_ADOPT` logs the set — verified it shows DefaultThread `80402aa8` at first-populate.)
> 2. **Native idle/hardware driver** (`nthr` idle handler, currently `nthr_idle_fatal`): when no
>    `nthr` thread is Ready, advance CoreTiming + deliver the pending DSP/DVD/VI IRQ so its handler's
>    native `OSWakeupThread` makes an `nthr` thread Ready. Then `os_sleep_thread` ALWAYS native-parks
>    (drop the `ready_count()==0` GC-scheduler fallback — it's the "two schedulers" conflict).
> 3. **Remove the conflicting recompiled-scheduler path**: un-model `rfi`/`mtmsr` (or otherwise stop
>    recompiling the exception/interrupt/context primitives) once native threading owns the path, so
>    the two schedulers don't fight.
>
> The 2026-06-03 investigation below (esp. "Attempt 3 — ROOT CAUSE") is the map for blocker 1.

> ## ⛔ (older) SUPERSEDED note (2026-06-03)
> This document describes the **native host-thread scheduler** approach to the audio-init stall.
> It was shelved 2026-06-03 in favor of the recompile-the-scheduler hybrid — see the REOPENED banner
> above for why that's now reversed.

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
   - 4b 🔄 **`OSResumeThread` + spawn + run + sleep/wakeup** (core working 2026-06-03; integration
     unfinished — see "Current frontier"). Implemented & verified to run: native `OSResumeThread`
     (decrement suspend; `nthr::make_ready` when runnable), `OSCreateThread` spawns the host thread
     SUSPENDED, the body runs recomp (or the budget-less interpreter loop for a JIT-only entry) on
     its own host stack, native `OSSleepThread`/`OSWakeupThread` park/wake via `nthr` on the guest
     wait-queues. Verified headless: all 5 boot threads spawn, start with the correct context, and
     park/switch cooperatively (workers sleep on their work queues; the audio thread runs).
     **Two bugs found & fixed along the way:**
     - The thread body MUST load its full initial register file from the guest `OSContext`
       (`OSThread+0`), not just sp/param/pc — otherwise `r2`/`r13` (the small-data bases) are zero
       and every SDA access is a wild `0xFFFFxxxx` pointer (the `strcpy` wild-write crash).
     - A JIT-only thread entry can't run under a *budgeted* `run_jit_sync` (the budget is for short
       synchronous callees); it runs under a **budget-less** `interp_run_until` and blocks by native
       parking, not by returning. (`interp_run_until` is the extracted shared interpreter loop.)
     - `DeclareAsCPUThread`/`Undeclare` bracket the body and every native block, so exactly one host
       thread is Dolphin's CPU thread at a time; `0x800000E4` is kept coherent via the restore hook
       + `nthrt_bind_current` (thread 0's real OSThread* isn't known until after adoption).
5. ✅ **`OSSleepThread`/`OSWakeupThread` are native (step 4b above);** `OSSendMessage`/
   `OSReceiveMessage`/mutex/cond stay recomp/interpreted and reach them through the interception
   seam (verified from the disasm: the message/cond primitives don't inline queueing — they *call*
   OSSleepThread/OSWakeupThread). So nativizing just sleep/wakeup covers the whole sync layer.
6. **Native idle/hardware driver (the current blocker — next step).** Replace the
   `ready_count()==0` fallback (see below) so a wakeup always flows through `nthr`.

## Current frontier (2026-06-03) — the idle/hardware driver
The native-threading core works, but the **`ready_count()==0` fallback does NOT compose with native
`OSWakeupThread`** and is the blocker. When a thread calls `OSSleepThread` and no other `nthr`
thread is Ready (early single-threaded boot, or all-threads-blocked), `os_sleep_thread` currently
super-calls the recomp `OSSleepThread`, whose `SelectThread` runs the GC idle thread under the
interpreter to await the waking IRQ. But the waking IRQ's handler calls native `OSWakeupThread` →
`nthr::make_ready`, which the **GC idle loop's run-queue never sees** → the wakeup is lost. Observed:
the audio thread (JIT-only `802a7878`) busy-waits in `DVDChangeDir` (`8034be30`) / re-enters
`OSSleepThread` forever, because the DVD-completion wakeup is routed through `nthr` but awaited by
the GC idle loop.

**Fix (next):** a real idle driver in the `nthr` idle handler (already wired via `set_idle_handler`,
currently a fail-fast). When no `nthr` thread is Ready, advance CoreTiming + deliver pending
interrupts so the IRQ handler's native `OSWakeupThread` makes an `nthr` thread Ready, then re-pick —
no GC idle loop, no fallback. **`os_sleep_thread` then always native-parks (drop the
`ready_count()==0` branch).** Required by [[done-right-over-working]] — the fallback is a
known-broken stopgap, not a kept dual path.

Concrete design (Dolphin API scouted 2026-06-03):
- Save global `ppc`; set `ppc.pc = ppc.npc =` a safe idle spin and `MSR[EE]=1`. Idle-spin PC = the
  GC IdleThread's context srr0 `mem_r32(0x803EB298 + 0x198)` if it's a valid `0x80xxxxxx` (it spins
  with interrupts on — exactly the idle context); else scan RAM/DOL once for a `b .` (`0x48000000`).
- Loop: `coretiming.Idle()` (zeroes `ppc.downcount` so the next step's `Advance()` fires due events)
  then `interp.SingleStep()` (Advance raises the pending IRQ; the exception check vectors to
  `0x80000500`; the ISR runs and calls native `OSWakeupThread` → `nthr::make_ready`). Stop the moment
  `nthr::ready_count() > 0`; a step budget with no progress = genuine deadlock (fail-fast).
- The idle handler runs on the parking host thread with `nthr`'s lock released (see
  `grant_token_locked`), so `make_ready`/`ready_count` (which take the lock) are order-safe. It only
  mutates global `ppc` (discarded when the woken thread's ctx is restored) + guest RAM/device state
  (the real work of delivering the interrupt) — intended.

### Attempt 1 — bare `b .` spin (RULED OUT, 2026-06-03)
Implemented the idle handler above (`b .` spin found by scanning RAM; `CoreTiming::Idle()` +
`SingleStep` with the native-OS intercept; drop the fallback). **It does not deliver device
interrupts.** Findings from the headless run:
- Removing the `ready_count()==0` fallback **breaks early boot**: the very first `OSSleepThread`
  (`cur=8042ba20 q=803e0220`, `ready_others=0`) is a single-runnable-context hardware wait the
  fallback was quietly handling. So the fallback is load-bearing for early boot, not just the audio case.
- The `b .` spin carries the *blocked thread's* MSR/context (`msr=0x1032`, `EE=0`; I force `EE=1`).
  Stepping it 20M–200M times delivered **no external-interrupt vector at all** (`0x500` never
  entered); the manual `Idle()`+`Advance()` variant fired the **decrementer (`0x900`) exactly once**
  then nothing. So a bare spin does NOT reproduce what the GC idle/reschedule does — the device
  IRQ that completes the wait never becomes pending/delivered.
- Why the *fallback* works where the bare spin doesn't is the key open question: the fallback runs
  the **real GC idle thread** (its own context/stack) via `SelectThread` under the interpreter, which
  evidently drives the device-completion path (PI unmask / servicing) that a context-less `b .` does
  not. Likely the idle context must be the genuine IdleThread context (load its full register file),
  or the wait completes by the thread re-polling a flag the ISR sets rather than via `OSWakeupThread`.
- **Next attempt:** run the **full GC IdleThread context** (load all 32 GPRs from `0x803EB298+0`, not
  a bare `b .`) as the idle spin and re-check whether `0x500` IRQs deliver; if they still don't,
  trace the *fallback's* wakeup of `8042ba20` (set `SUNBRIGHT_OSWATCH`, watch `q=803e0220`) to learn
  whether it's an `OSWakeupThread` or a polled-flag completion. Code was reverted to the committed
  WIP (fallback retained) so `main` stays in the best working state meanwhile.

### Attempt 2 — traced the working fallback (2026-06-03): the deeper conflict
Instrumented the committed (working-fallback) boot to learn how waits actually resolve:
- **`OSWakeupThread` is NEVER called during boot** (native `os_wakeup_thread` logged zero hits).
  Yet the audio thread (`803fcbe8`) sleeps on `q=8040e870` and the fallback returns ("woken")
  immediately, in a tight loop. So the wakeup is **not** an `OSWakeupThread` — the GC `SelectThread`
  inside the fallback just re-selects the same sleeping thread. The audio wait is for a **DSP/AI
  hardware event** whose ISR *would* call `OSWakeupThread(8040e870)`, but that IRQ isn't firing.
- The first sleeper `8042ba20` (`q=803e0220`) blocks, and **the 5 worker threads are created while
  it is still inside its fallback `OSSleepThread`** — i.e. the fallback's `SelectThread` switched to
  another guest context (under the interpreter) which ran boot + `OSCreateThread`. So the fallback is
  **running the GC scheduler**, switching guest contexts, *concurrently with* `nthr` tracking the
  same threads — **two schedulers over the same guest structures.** This is the real conflict:
  native threading and the GC scheduler cannot coexist; the fallback must be fully removed, not kept.

**Reframed problem & plan.** The blocker is not "compose the fallback" — it's that finishing native
threading requires *both*: (a) a real idle/hardware driver that delivers device IRQs so an ISR's
`OSWakeupThread` wakes an `nthr` thread (the audio `8040e870` wait needs the DSP/AI IRQ), **and**
(b) removing the GC-scheduler fallback entirely (it conflicts). Attempt 1 showed a bare `b .` spin
delivers no IRQs — but it was tested on `8042ba20`'s early wait, which the trace shows is **woken by
a context switch, not an IRQ**, so that was the wrong test case. Open questions for the next session,
in order: (1) map the early-boot thread identities — is `8042ba20` `nthr` thread 0, and who is meant
to run when it sleeps? (the model assumes one boot thread, but boot uses several); (2) confirm the
audio `8040e870` wait's waker is the DSP/AI ISR (set a watch on `OSWakeupThread`/the AI/DSP vectors);
(3) get the idle driver to deliver that specific DSP/AI IRQ. This likely needs the **full IdleThread
context** (not a bare spin) and possibly servicing the AI/DSP the way the GC idle path does.

### Attempt 3 — ROOT CAUSE: threads created before interception (2026-06-03)
`SUNBRIGHT_OSWATCH` trace of early boot pins the real problem:
- The **main boot thread is `80402aa8`** (= `nthr` thread 0 / the adopted EmuThread — it owns almost
  every early OS call: the malloc-lock `OSLockMutex`/`Unlock` on `80427838`, etc.).
- **`8042ba20` is a *different* guest thread** (sleeps on message queue `803e0220` via
  `OSReceiveMessage`, `lr=803462b0`) — and it was **never seen by our `OSCreateThread` intercept**.
  So it (and `80402aa8`) were created during **early `OSInit`, before any recomp ran**, i.e. before
  `native_os` interception is active (the intercept only fires inside `call_ppc`/`interp_run_until`).

**Root cause.** `nthr` adopts "thread 0 = the EmuThread" as *one* thread, but the EmuThread is really
the **GC scheduler multiplexing several pre-existing guest threads** (`80402aa8`, `8042ba20`, …)
created before interception. So `nthr` can't see those context switches; when `8042ba20` sleeps and
the GC scheduler switches to `80402aa8`, `nthr` still thinks its single thread 0 is running. The
native primitives + fallback therefore operate on a thread set that doesn't match reality — hence the
"two schedulers" tangle and the audio thread's `8040e870` wait never being driven.

**What this means for the design.** Adopting only the EmuThread is insufficient. To finish, native
threading must **enumerate and adopt the GC OS's already-existing threads at takeover** (walk the
active-thread list at `0x800000DC` / the run queue `0x803EB198`, create an `nthr` GuestThread for
each, map current `0x800000E4`), and intercept `OSCreateThread` for all *future* ones — i.e. take over
the scheduler at a single well-defined point with the full thread set, rather than incrementally
from the EmuThread. Until that redesign, the committed WIP (native primitives + GC-scheduler fallback)
is the best working state; the fallback can't be removed without the full adoption.

### Older notes
SDL/present thread signals vblank waiters via the native condvar; audio-out thread signals audio
waiters. A preemption nudge may be needed if a busy-wait guest thread is found not to yield.

## Verification
Headless turbo (`SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1`, `SUNBRIGHT_RUN_SECONDS=N`,
`SUNBRIGHT_AUTOSTART=1`): audio asset loads should match pure-Dolphin timing (no multi-second
gaps), no `FATAL ... exceeded step budget` abort, no `FATAL wild guest write`, no `JUTException`.
A/B against `SUNBRIGHT_DISABLE_RECOMP=1`.

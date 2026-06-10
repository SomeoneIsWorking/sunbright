# Native OS threading (PC-port model)

> ## 🟢 2026-06-10 — external-interrupt delivery is NOW FULLY PC-NATIVE
> The GC guest interrupt path (0x500 vector → ExternalInterruptHandler → __OSDispatchInterrupt →
> handler → OSLoadContext rfi) is RETIRED from live execution. `native_dispatch_one/pending`
> (dolphin_hook.cpp) is a behavior port of OSInterrupt.c: it reads Dolphin's PI cause/mask
> host-side, builds the OS cause word (DSP/AI/EXI sub-cause MMIO reads), masks with the OS globals
> (0x800000C4/C8), walks InterruptPrioTable, and calls the registered guest handler from the table
> at *0x8040E7B0 directly via call_ppc (r3=interrupt, r4=context). Delivery happens ONLY at owned
> points: the idle driver, poll_yield, and the interp loop (pending+EE check before SingleStep).
>
> WHY (the OSError-15 corruption class, fully root-caused): stepping the non-reentrant guest
> dispatch under the hybrid corrupted it three independent ways —
> 1. `nthr::block()` used `self = g_running` (the token holder) instead of the CALLING host
>    thread; a non-holder blocking parked the wrong GuestThread and later woke TWO runners.
> 2. The per-thread context switch did not carry SRR0/SRR1/npc/lwarx-reservation; a thread parked
>    mid-exception-window resumed with a foreign srr0 → its rfi jumped into the middle of
>    unrelated functions (epilogue loads a never-written LR slot → blr to 0).
> 3. Forced delivery at EE=0 points nested a second dispatch inside the first (interrupt index
>    scrambled to 0 → MEMIntrruptHandler → spurious OSError 15 → JUTException crash screen →
>    MarErrException readPad spin = the "infinite spin" symptom).
> All three fixed (commit "Native interrupt dispatch", 2026-06-10). Diagnostics kept: per-step PC
> ring (collapsed-run, thread-tagged), poisoned-entry traps, MEM-dispatch trap, interpreter token
> guard, and `sunbright_park()` (CPU-idle park, REPL stays up, watchdog spares it — nothing
> busy-spins on a failure anymore).
>
> ## 🟢 2026-06-10 (later) — GX FIFO CPU↔GPU pacing ported natively
> The "FIFO is overflowed by GatherPipe" crash + the all-blocked deadlocks after it were ONE
> chain — the GC frame-pacing contract was unported. Five pieces (commit "GX FIFO pacing"):
> 1. OSSuspendThread (0x80349170) ported — __GXOverflowHandler's hi-watermark self-suspend of
>    the pushing thread now parks the nthr thread (was a recomp no-op → overflow by a full lap).
> 2. Recomp call boundaries deliver pending IRQs natively (charge_guest_time → dispatch seeded
>    from the live recomp ctx) — a GX push loop never reaches another delivery point.
> 3. The native VI retrace transaction also runs from the IDLE driver (once per presented field)
>    — when every guest thread is blocked the retrace must still tick (it is an interrupt).
> 4. Dolphin's GPU loop only wakes on bursts/CTRL writes: our bridge kicks Fifo::RunGpu on
>    FIFO_BP_LO/HI writes and the idle driver kicks it each step (real CP never sleeps).
> 5. VIWaitForRetrace gained GPU BACKPRESSURE (wait until FIFO < cap/8 before the next frame),
>    pumping CoreTiming + native dispatch + yielding the nthr token each spin. Without it the
>    game ran 18+ frames ahead during boot shader-compile hitches; Dolphin's PE coalesces
>    draw-sync token interrupts (keeps only the latest) → TDrawSyncManager lost tokens → the
>    breakpoint stopped advancing → watermark deadlock. Key decomp source:
>    reference/sms/src/System/DrawSyncManager.cpp (token→breakpoint protocol).
> Verified: long runs with draw-sync tokens + CP interrupts alternating continuously, VI fields
> presented throughout, no overflow/deadlock. REPL gained /r16 (16-bit MMIO reads) and /gx
> (CP/Fifo internals: rp/wp/bp/watermarks/interrupt_waiting).
> 🟢 RESOLVED same day — the "0.01×" was a MEASUREMENT ARTIFACT plus a missing emu-clock tick:
> `sunbright_wait_vi_field` (modernized: EE-masked Advance + native dispatch) is wired into the
> heartbeat, forcing exactly one VI field of emulated time per host frame → ~60 presented
> fields/s (verified by the [vi-field] telemetry and the watchdog field counter). Dolphin's
> GetSpeed/FPS counters read ~0 on this path — their update hooks sit on the throttle path we
> bypass; TRUST the field counter + frame dumps, not those metrics. Backpressure threshold must
> stay fifo_cap/8 (watermark-relative re-entered the token-coalescing deadlock — measured).
> **BOOT REACHES THE SMS TITLE SCREEN HEADLESS** (Dolby logo → title, framedump evidence,
> run 88, 2026-06-10). Next: menus/file-select under autostart, in-game, audio output, and a
> real perf pass with honest metrics.

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
> CONFIRMED OUR HYBRID, NOT THE GAME (2026-06-09 A/B). `SUNBRIGHT_DISABLE_RECOMP=1`
> `SUNBRIGHT_BACKEND=Software` headless boots clean PAST mountStageArchive — it loads the stage/option
> assets (`w1stLoad`, `yoshi.szs`, `scenecmn.bin`, `params.szs`, `option.szs`, `PerformLists.bin`) and
> never trips the invalid read. So the r31 clobber is introduced by our recomp/native-threading hybrid.
> The fault PC 0x802a6338 lives in **`func_802a5f50`** (recompiled — spans 802a5f50→802a637c, no recomp
> entry between it and func_802a6380), YET the faulting load went through Dolphin's MMU (our recomp
> wild-read trap did NOT fire), so func_802a5f50 is executing under Dolphin (interp/JIT) at fault time,
> not as recomp — i.e. an ancestor entered the interpreter via call_ppc and the whole subtree runs
> there. NEXT (mechanism hunt): runtime-trace r31 through func_802a5f50 (the trace ring, 5876e71) to
> find the first call whose return leaves r31 != caller value; and establish how 802a5f50 is reached
> (which call_ppc ancestor handed it to the interpreter) since that boundary is where a guest
> non-volatile would be dropped.
>
> ROOT CAUSE PINNED (2026-06-09) — `func_802f80d0` (`JDrama::TDisplay::endRendering`) recomp drops
> non-volatile r31. New diagnostic `SUNBRIGHT_DBG_TRAMP=LO-HI` (jit_hook.cpp) logs every Dolphin-JIT
> block dispatch in a range with the engine (recomp vs JIT) + live regs (block-linking is off, so
> every basic-block boundary hits the trampoline). It showed the boot-logo loop runs under Dolphin JIT
> with r31=803e9700 (the valid TApplication `this`) correct through the WHOLE loop until the virtual
> call `bclrl @802a6320` (`method = [[r31+0x1c]vtable+0xc]`). That method dispatches to
> **`802f80d0 -> recomp`** (entered with r31=803e9700 correct); the very next dispatch `802a6324` has
> **r31=802a6324** — i.e. `endRendering`'s recomp returned with r31 = the call's own return address.
> CONFIRMED by bisection: `SUNBRIGHT_FORCE_JIT=802f80d0-802f816c` (route endRendering to Dolphin JIT,
> DIAGNOSTIC ONLY) → r31 stays 803e9700 across the call and the `0x28040060` wild read DISAPPEARS. So
> the defect is in our recomp of endRendering or a function in its call tree — NOT the game.
> endRendering's own recomp prologue/epilogue is CORRECT (saves r31 to [r1+0x24], restores from the
> same slot; saves LR to [r1+4]→[r1+0x2c]). The corruption value 0x802a6324 == the saved-LR linkage
> slot [entry_sp+4], and reading r31 from [r1+0x24] would hit that slot iff cpu.gpr[1] (SP) at the
> epilogue is +8 too high — i.e. an **inner `call_ppc` in endRendering returns SP off by +8** (an
> unbalanced-stack mistranslation in one of its callees: 0x802fc9a4, 0x8035d8f0, and the bl targets
> @802f8110/@802f8144). NEXT: force_jit-bisect endRendering's four inner calls (one at a time) to find
> which returns a corrupted SP, then fix that callee's recomp frame handling + add a recompiler test.
> Getting past this clobber reveals the NEXT crash: `Invalid read ea=0x00000000 PC=0x802a6160` (the
> null `[this+4]` deref the entries above chased) — a separate downstream issue.
>
> NARROWED + MECHANISM (2026-06-09). force_jit-bisecting endRendering's four inner calls (0x802fc9a4,
> 0x802ca1e0, 0x802f917c, 0x8035d8f0) one at a time, watching r31 at the 802a6324 dispatch: ONLY
> `SUNBRIGHT_FORCE_JIT=802fc9a4-...` restores r31=803e9700 and kills the wild read. So the corruptor is
> **`func_802fc9a4` = `JDrama::TVideo::waitForRetrace(u16)`** (a retrace WAIT loop) or its recomp tree.
> Built a reusable SP-imbalance detector `SUNBRIGHT_DBG_SPCHK` (dolphin_hook.cpp call_ppc): the PPC ABI
> guarantees r1 is identical across any call, so a recomp call returning with a changed SP (excluding
> context-switch handoffs) is an unbalanced-stack bug. IT DID NOT FIRE — so this is NOT a plain
> unbalanced-stack instruction mistranslation. waitForRetrace does not return normally through
> call_ppc: as a retrace wait it does a WAIT/CONTEXT-SWITCH handoff (OSSleepThread/siglongjmp) that
> ABANDONS endRendering's recomp C frame; when the thread is later woken and control returns toward the
> logo loop, it resumes under Dolphin JIT with SP/r31 off by 8 (epilogue then reads r31 from the
> saved-LR slot = 0x802a6324). force_jit fixes it because Dolphin/the native scheduler runs the wait +
> resume consistently. This is a recomp×native-threading boundary issue, NOT a pinnable single-opcode
> mistranslation → per the debug-path rule it falls to OWN-IT-NATIVELY. PROPER FIX: a PC-native
> override of `waitForRetrace` (func_802fc9a4) — RE what it waits on (VI retrace count via the VI ISR /
> OSSleepThread on the retrace queue) and port it, like the TTrack-tick native port. That keeps the
> wait off the recomp C stack so no handoff abandons a live recomp frame.
>
> CORRECTED MECHANISM (2026-06-09) — it is a CALL-MODEL tail-handoff hazard, not waitForRetrace logic.
> Added `SUNBRIGHT_DBG_SPCHK` interp-path coverage (checks cpu.gpr[1] vs sp_floor after
> interp_run_until) and `SUNBRIGHT_DBG_TAIL` (logs tail_ppc to non-recomp targets). For the
> endRendering window:
>   - `SUNBRIGHT_DBG_SPCHK` fires on NEITHER the recomp nor the interp call path → no call returns with
>     a changed SP. The earlier "inner call returns SP +8" theory is FALSIFIED.
>   - `SUNBRIGHT_DBG_TAIL` shows MANY `tail_ppc -> 8033xxxx` (non-recomp) handoffs during the window
>     (8033bc1c/8033addc/80337d18, lr=80338fb8…). Each siglongjmp's back to `Run`, unwinding the recomp
>     C frames between it and Run.
>   - `SUNBRIGHT_DBG_TRAMP=802f80d0-802f8170` shows endRendering dispatched to recomp exactly ONCE and
>     its epilogue NEVER runs under JIT; `SUNBRIGHT_DBG_CTX` shows ZERO OSLoadContext switches.
> So: endRendering(recomp) calls waitForRetrace via `bl` (call-and-continue, expecting its C
> continuation/epilogue to run). Deep in waitForRetrace's tree a `tail_ppc` to a non-recomp 8033xxxx
> leaf siglongjmp's back to Run, UNWINDING endRendering's (and other `bl`-callers') C frames. Their C
> epilogues never run; execution resumes under Dolphin JIT from the committed shared register file.
> Because the unwinding bypasses endRendering's epilogue, the non-volatile r31 is never restored to the
> value the still-active logo-loop frame holds → it surfaces as 0x802a6324 at the logo loop's resume.
> tail_ppc's own comment claims unwinding a non-tail `bl` caller is "correct… resumes under the JIT from
> the shared state" — THIS CASE FALSIFIES THAT when a `bl` caller still needs a non-volatile.
> force_jit(waitForRetrace) works only because it keeps the entire handoff-heavy subtree OFF the recomp
> C stack, so no live `bl`-caller frame is unwound.
> FIX OPTIONS (next session — do NOT ship a half-understood reimplementation):
>   (a) Call-model fix (the real root, architectural): a tail/context handoff must not silently unwind
>       `bl`-caller recomp frames that still need their non-volatiles — those frames must run their C
>       epilogues (restore guest non-volatiles) before control leaves, OR the handoff must guarantee
>       the guest stack fully encodes every unwound frame's restore so the JIT resume is register-exact.
>   (b) Targeted own-it-natively: native override of waitForRetrace (func_802fc9a4) so its wait +
>       8033xxxx handoffs never sit under a recomp `bl` caller — needs faithful RE of its spin + the
>       post-wait field double-buffering ([this+0x3c..0x5c]→[this+0..0x1c], [this+0x78], [this+0x84]).
> force_jit(802fc9a4) is the confirmed ISOLATION (a DIAGNOSTIC, NOT committed as a fix).
>
> TRIED + REVERTED (2026-06-09) — blanket "run returning tail targets synchronously" call-model fix.
> Changed tail_ppc's non-recomp path to `interp_run_until(cpu.lr, budget, sp_floor)` + return (like
> call_ppc) instead of siglongjmp, so a `bl`-caller frame survives. RESULT: it DID clear the logo r31
> clobber (Invalid read 0x28040060 gone), but boot then HUNG — watchdog FREEZE, recomp spinning at
> `pc=803433f8` in `func_803433b4 → func_803494f0 → SystemTimers::GetFakeTimeBase` (an OS mftb
> time-wait), with `tail +0 interp_steps +0 poll_yield +0` (CoreTiming not advancing). So the
> siglongjmp handoff is LOAD-BEARING: it is how OS time-wait/delay loops get run under Dolphin's CPU
> loop where CoreTiming (and thus mftb/the fake TB) advances. Kept on the recomp C stack they spin
> forever (the poll-yield detector watches RAM-flag reads, not mftb spins). A blanket call-model change
> is therefore the WRONG fix; reverted (no env-gated dual path left, per done-right-over-working).
> REFINED FIX DIRECTION — kill the handoff at its SOURCE, not the handoff mechanism. The tail handoffs
> that corrupt the r31 are into libc string functions (vsnprintf 80339874, fwrite 80338f8c, __va_arg
> 80337ccc, fwide 8033bc08) whose internal jump-table `bctr` branches land at interior addresses that
> aren't recomp entries → tail_ppc → longjmp. These are STANDARD C LIBRARY routines; a PC-native
> program just uses the host's. Two surgical options:
>   (1) Native overrides of the libc string/format functions (vsnprintf/sprintf/fwrite/__va_arg…),
>       like the existing OSReport→printf mapping — they then run as clean single-entry native C with
>       no interior handoff. Lowest-risk + most "PC native"; needs careful guest-ABI marshaling of the
>       format string + va_list from guest memory, and the output buffer (callers seen writing to stack
>       buffers e.g. 804277bc, so the formatted result IS consumed — must be byte-faithful).
>   (2) Recompiler-level: make the interior jump-table targets dispatchable (register them as entries /
>       keep intra-function branches as gotos) so the branch never escapes to a non-recomp address.
>       The [[linear-truncation-bug]]/pointer-discovery area; needs /recompile + regression test.
> Whichever: getting past the r31 clobber also reveals the next crash (Invalid read ea=0 PC=802a6160).
>
> ✅ FIXED (2026-06-09) — the chosen fix was the RECOMPILER one (option 2), and the root turned out to
> be a function-collection TRUNCATION, not a coverage gap. `func_collect`'s linear pass stopped at the
> first `blr`/tail exit, but `func_80337ccc` (`__va_arg`) has a `blr` @80337d14 that an EARLIER forward
> `bc 0x80337d18` @80337cf8 JUMPS OVER (switch: case1 ends in blr, case2 reached via the bc). Collection
> truncated at 80337d14, so 80337d18..80337dbc became a mid-function `tail_ppc` to a non-recomp address
> → the handoff. Same class as the earlier initAllCheckData truncation, different trigger (a jumped-over
> blr vs a forward `b` to a loop test). Fix (tools/recompiler/func_collect.cpp): track the furthest
> in-function forward branch target; only treat an exit as the function end when nothing branches past
> it. + recomp_test regression case. Regenerated (13480 funcs): all interior tail_ppc gone, the libc
> bodies (__va_arg/vsnprintf/fwrite/fwide) are whole, and the boot wild read 0x28040060 is GONE
> (verified headless). No native override or call-model change needed — the most PC-native outcome (the
> functions now run fully as native recomp code, no Dolphin handoff). force_jit/tail-sync were
> diagnostics only; none shipped.
>
> `__OSInitAudioSystem` (func_803433b4) SPIN — **FIXED (2026-06-09) by a PC-native port that drops the
> HW-settle busy-waits** (`runtime/overrides/os_init_audio_native.cpp`). ROOT CAUSE: the recompiled
> __OSInitAudioSystem runs as native C on our call stack and never returns to the CPU loop, so it cannot
> advance Dolphin's CoreTiming / fake time base. Its DSP-boot + ARAM-init sequence is full of waits that
> only clear once CoreTiming/TB advances: DSP-reset poll (DSP_CONTROL bit0 — HLE clears it synchronously
> anyway), two AR-DMA-complete polls (DSP_CONTROL 0x20 INT_ARAM — set by the `DSP::CompleteARAM` CoreTiming
> event; the data is already moved synchronously inside `Do_ARAM_DMA`), a DSPInitCode poll (DSP_CONTROL
> 0x400 — DSPHLE sets it on the DSPInit 1→0 edge and only clears it after FakeTimeBase advances 130 ticks),
> a mail-from-DSP poll (`INITUCode::Initialize()` pushes mail synchronously on SetUCode), and an OSGetTick
> 2194-tick settle delay. Under DSP-HLE every awaited signal is *deferred latency*; the functional work
> (ucode load, ARAM DMA data movement, mail push) is synchronous. A PC build has no reason to busy-wait on
> hardware settle, so the native port does every MMIO config access in order (identical Dolphin DSP-HLE /
> ARAM state) and drops each wait loop + the OSGetTick delays. VERIFIED: boot advances from this spin all
> the way to the boot-sequencer (`mountStageArchive`) — pure-Dolphin A/B confirms the rest of boot is
> reachable. (Matches the user directive: port init to PC, remove cycle-waiting that a PC build doesn't
> need — do NOT make the wait elapse via a time-base/downcount/spin-detector; those were rejected.)
>
> NEW FRONTIER (2026-06-09, sharpened) — with audio init AND thread-exit unblocked, boot reaches the
> **TApplication per-frame state machine** `func_802a5f50` (unnamed in the sparse reference; sits
> between `mountStageArchive` 802a5998 and `__ct__12TApplication` 802a7b08; called from the main loop
> 80005628). It is a yielding state machine on `this`=gpApplication **803e9700**: early states init
> (create the JDrama TDisplay + TDirector), later states DRAW (`JDrama::TDisplay::startRendering`
> 802f7fd8 / `endRendering` 802f80d0 bracketing the director draw). It crashes at `802a6338` reading
> `[this+28]+96` where `[this+28]` (the TDisplay member) = garbage **0x28040000**.
> A/B GROUND TRUTH (pure-Dolphin, Software backend, REPL `/r`): `[803e9704]` (this+4, TDirector,
> vtable 803df0c8) = **0x80902a40**; `[803e971c]` (this+28, TDisplay, vtable 803e1dc0) = **0x8056dd90**;
> state `[8040e190]` (=`[r13-0x6030]`) = **3**. Native crashes with this+28 = 0x28040000 (never created —
> not even null; an out-of-RAM junk address), i.e. **the state machine runs a DRAW state before the
> INIT states created the TDisplay/director.** Same root the doc flagged at lines ~72–100: under native
> scheduling the state never advances 0→3. NB the bitfield state `[8040e190]` is gated by bit0 =
> `[this+4].director->method@0x64()==4` and bit1 = `OSIsThreadTerminated(loader 803fcbe8)`; bit1 is now
> satisfiable (loader exits → MORIBUND via the new native OSExitThread bookkeeping) — re-check whether
> the director (this+4) is now created and only the TDisplay (this+28) lags, or the machine still stalls
> earlier. DIRECTION (user, 2026-06-09): **own this natively — replace this hard-to-debug GC frame
> state machine with native PC code** (port `func_802a5f50` + the TDisplay/TDirector create path so the
> ordering is explicit), rather than chasing the recomp-level corruption. The crash is too fast to catch
> by polling (Software boots to it in <2s); inspect via pure-Dolphin A/B and/or a one-shot state dump in
> the FATAL handler.
>
> DEEPER TRACE (2026-06-09, force_jit bisection) — confirmed facts:
> • `this` = gpApplication = **803e9700** (RELIABLE: the force_jit crash dump shows r31=803e9700; the
>   normal-path dump's regs are stale Dolphin-sync values). `func_802a5f50` layout: `[this+8]` u8 = boot
>   **state byte** (2 = wait-for-loader-thread → OSIsThreadTerminated/OSJoinThread; 3 = director
>   bitfield-init `[8040e190]`; else → DRAW path lbl_802a615c); `[this+4]` = TDirector; `[this+28]` =
>   JDrama TDisplay; `[this+32 + i*4]` (i<4) = 4 viewports.
> • PRIMARY bug: **`[this+4]` (director) is NULL under native** (Dolphin = 0x80902a40). Force_jit'ing
>   endRendering (802f80d0) moves the crash from the `[this+28]` read (802a6338) to the director-null
>   deref at **802a6160** (`[this+4]->vtable`, ea=0). So the deepest wrong state is the un-created
>   director; the `[this+28]` clobber is a separate timing-dependent recomp effect of endRendering's
>   RECOMP path (its own body writes only the TDisplay `this`+`[sp]`, never gpApplication+28 — a callee /
>   register-restore artifact, not a direct bad store; secondary).
> • The director is created off the **loader thread 803fcbe8** (`mountStageArchive` body, entry 802a7878,
>   r3=this=803e9700). Under native it **opens the disc then exits almost immediately (val=0)** without
>   completing the archive load + director creation — so OSIsThreadTerminated/join now succeed (the
>   thread-exit fix) but the joined thread did nothing useful. NEXT: RE `802a7878` — why it returns early
>   under native scheduling (native_dvd read result? an OS wait native short-circuits? object-create vs
>   heap ordering, cf. the prior "null archive" frontier). That early exit, not the frame state machine,
>   is the thing to own/port natively.
>
> ROOT FOUND (2026-06-09) — it is the **null-archive / director-create** family, not a frame-fn or
> register bug. REPL/dump ground truth at the native crash (gpApplication 803e9700): **state[+8]=2**
> (stuck at "wait-for-loader-thread"), **dir[+4]=0** (director NEVER created), disp[+28]=**8056dd90
> (VALID — matches Dolphin; the 0x28040000 at 802a6338 is an r31/register artifact in the render path,
> NOT memory corruption)**, loadedArchive[8040e194]=**8131f0e0 (archive DID load — native_dvd served 28
> reads, all result=len, ~110KB)**. Pure-Dolphin here: state=5, dir=80902a40, disp=8056dd90. So the
> archive **loads fine**; what fails is **mounting it + creating the director**. The director/scene is
> built by `func_802a6dd0` (mountStageArchive+0x1438): `new` a 108-byte scene obj → vtable init →
> **`JKRMemArchive::mountFixed` 802c40ec** → more vtable init; called from 7 scene-init states in the
> 802axxxx jump table. Under native, with boot stuck at state 2, those states aren't reached / the mount
> doesn't complete, so dir stays null and the per-frame TApplication fn (802a5f50) renders a null
> director → crash. This is the SAME "JKRMemArchive::mountFixed returned NULL — heap not ready / thread
> ORDERING" frontier from [[blocking-call-interp-spin]] / paired-single notes. NEXT: determine whether
> (a) the boot state machine never advances 2→3 under native (find the [this+8] writer — it is NOT in
> 802a5f50/802a5b44, so an external fn consuming the archive) or (b) mountFixed is called but returns
> NULL (heap-not-ready ordering). Tools: REPL /trace?a=803e9708 (state byte) + /trace?a=803e9704 (dir)
> native-vs-Dolphin; /poll for A/B snapshots. The fix is to own the archive-mount / heap-ready ordering
> natively (the long-standing native-threading frontier), not the frame state machine.
>
> FULL CHAIN (2026-06-09) — boot orchestration: `80005600` (per-frame, this=gpApplication) →
> `802a6398` = the **scene state machine** (loops on `[this+8]`, the `bctr@802a63e0` jump-table the doc
> referenced; state 7 = exit). Early state-cases create the scene/director via **`802a6dd0`**
> (mountStageArchive+0x1438): `JKRHeap::becomeCurrentHeap([r13-24360])` → **alloc the 108-byte scene
> obj from the JKR current heap `[r13-24368]` = 8040e290** → `JKRMemArchive::mountFixed` (802c40ec) the
> loaded archive → store the director. The later state (2) calls `802a5f50` (draw), which needs the
> director. Native reached state 2 ⇒ the create-case RAN, but produced a null director ⇒ **the JKR heap
> `[8040e290]` / `[r13-24360]` was not ready when 802a6dd0 ran** (Dolphin: current-heap=804278c0,
> heap-obj[8040e298]=80427820, dir=80902a40, state=5). `[8040e290]` is the JKR **current heap** global,
> set by `becomeCurrentHeap`/`becomeSystemHeap`/`~JKRHeap` (802c3730/802c3720/802c3524). So the fix is a
> **heap-ready-before-scene-create ordering** fix (the user's condvar / make-sequential instinct): under
> native scheduling the thread/step that creates+installs that heap hasn't run when the scene-create
> fires. IMMEDIATE NEXT (before adding any sync — don't guess): find which thread/step creates the heap
> object `[r13-24360]`=80427820 and installs it as current, and confirm via A/B that under native it is
> null/late at the moment 802a6dd0 runs; then enforce that ordering (condvar/sequential) at that seam.
>
> CORRECTION (2026-06-09, via SUNBRIGHT_FATAL_HOLD REPL inspection) — the heap-race hypothesis above is
> **WRONG**. New tool: `SUNBRIGHT_FATAL_HOLD=1` (+ `SUNBRIGHT_WATCHDOG=0`) parks the process at the fault
> instead of aborting, so the SUNBRIGHT_PROBE REPL stays up and you read guest state at the crash via /r
> (memory_bridge.cpp fatal_hold_or_abort). At the native fault ALL of these match Dolphin and are healthy:
> currentHeap[8040e290]=**804278c0**, rootHeap[8040e298]=**80427820**, loadedArchive[8040e194]=**8131f0e0**,
> disp[this+28=803e971c]=**8056dd90 (VALID)**. Only dir[this+4]=0, state[this+8]=2. Heaps/archive are
> READY — NOT a heap-ordering race. TWO separate problems:
> 1) **r31-clobber recomp bug = the actual native crash (802a6338):** fault reads `[r31+28]`=0x28040000 but
>    MEMORY [803e971c]=8056dd90 is fine ⇒ **r31 (=this) was corrupted by a render-path callee that fails to
>    restore non-volatile r31.** 802a5f50's render path (6174→6338) calls 802ca1e0×3, GX
>    803630c8/80363138/8034a4d4/80362c34, [this+52] vtables, 8001e920, endRendering 802f80d0; force_jit of
>    endRendering moves the crash ⇒ clobberer is in endRendering's subtree. Recompiler register-preservation
>    bug to bisect (or own that callee). THIS is the concrete next target.
>    DEAD-END (don't repeat): force_jit of waitForRetrace (802fc9a4) HANGS — the VI-retrace wait breaks
>    under pure JIT — INCONCLUSIVE, not proof it's the clobberer. waitForRetrace's recomp is correct
>    (saves r31@802fc9b0, restores@802fcb44; VIWaitForRetrace 8034f684 is recompiled, parks via native
>    OSSleepThread). Solid: force_jit of endRendering (802f80d0) cleanly moves the crash to the
>    director-null (802a6160), so the r31 clobberer is in endRendering's subtree — bisect its callees
>    (802fc9a4/802ca1e0/802f917c/8035d8f0) with FATAL_HOLD + read the LIVE recomp r31 (g_cur_recomp_cpu),
>    not the stale ppc dump, to pin it.
>    UPDATE: the live-recomp-reg dump (memory_bridge.cpp, g_cur_recomp_cpu) did NOT fire at the 6338
>    fault ⇒ g_cur_recomp_cpu is NULL there ⇒ 802a5f50 at the crash is NOT running as pure recomp; it is
>    on the interpreter/hybrid path (run_jit_sync), and the ppc r31=802a6324 is stale ([802a634c]=806da118
>    != the faulting 0x28040000). So the r31 clobber is most likely a recomp<->interpreter REGISTER-SYNC
>    hazard (cpu<->ppc around a run_jit_sync callee in the render path), i.e. the call-model handoff, NOT
>    a single mistranslated function. Next: find which render-path callee runs interpreted (function_needs_jit)
>    and audit the cpu<->ppc sync (cpu_to_dolphin_state/dolphin_state_to_cpu) around it for r31 fidelity.
>    UPDATE2 (backtrace symbolized): the faulting read is **PowerPC::ReadFromJit<u32>** — DOLPHIN's
>    JIT/fastmem path, not our recomp's sb_r32. So 802a5f50 at the crash runs as **Dolphin JIT-compiled
>    code** (a mid-function branch-target block whose entry isn't a registered recomp func, so the JIT
>    hook didn't route it to recomp), and ppc IS the live state there — but ppc.gpr[31] is already
>    corrupt going INTO that JIT block. Thread 0 (main TApplication) is nthr-ADOPTED (adopt_current,
>    sunbright_adopt_cpu_thread) and runs under Dolphin's CPU loop; nthr's per-thread ctx save/restore
>    (nthr_ctx_save/restore, full gpr/fpr/cr/lr/ctr/xer/gqr/srr — verified complete) handles its parks.
>    So r31 is lost at a **recomp->JIT(->park) handoff in the render path** (recomp computes/holds r31 in
>    its CPUState, exits to Dolphin JIT, and the synced ppc.gpr[31] is wrong). NEXT: trace thread 0's
>    execution mode through the render path (DBG_SWITCH + which blocks are recomp vs Dolphin-JIT) and the
>    exact point r31 diverges in ppc — likely a recomp block exits without flushing r31 to ppc, or a JIT
>    block runs before a recomp store of r31 commits.
>
> NEW FRONTIER (2026-06-09, after the GXDrawDone native port) — boot no longer deadlocks (runs the
> full RUN_SECONDS, no FATAL, no freeze) but presents ~0 VI fields (vps=0). A JASystem audio Kernel
> thread (entry 803171ec portCmdInit) runs under the INTERPRETER and spins ~30M interp-steps/s
> (pc cycles 8031b9fc/803212a4/8031d300/… — audio cmd loop), hogging the cooperative nthr token and
> starving the render thread → no frames. interp_wall_frac is low (~0.036) so it is not a wall-time
> hog, it is a SCHEDULER-fairness/yield issue: the interp audio thread rarely hits a native block
> point so nthr never switches to the renderer. NEXT: get that audio thread off the interpreter
> (recompile its JIT-only entry / port it) OR make it yield, like [[blocking-call-interp-spin]].
>
> NEW FRONTIER (2026-06-09, after the recompiler jump-table fix landed) — boot now renders **7 VI
> fields** then DEADLOCKS: the main thread parks in **GXDrawDone (8035dae8)** via
> func_802a5f50→THPPlayerDrawDone(8001e920)→GXDrawDone→OSSleepThread, waiting for the GPU draw-done
> (PE_FINISH) interrupt, which the native idle/IRQ driver does not deliver → no wakeup → watchdog
> freeze (Dispatch +0 across the board, all threads blocked). The recompiler jump-table fix is
> CONFIRMED working: the scene state machine 802a6398 now dispatches its bctr cases (func_802a63f0
> etc.) in-function, no JIT handoff, and reaches real rendering. NEXT: make the native idle driver
> deliver the PE_FINISH / GX draw-done interrupt (or own GXDrawDone natively) so the waiter wakes.
>
> ROOT CAUSE FOUND (2026-06-09) — the boot JIT handoff is a RECOMPILER jump-table gap. SUNBRIGHT_DBG_TAIL
> shows `func_802a5b44` (per-frame scene-change reader, called from 802a5f50's render path @802a6048)
> does `tail_ppc(cpu, ctr)` for its computed `bctr` jump table (base 0x803df3f0, indexed by a small
> state) to targets **802a5c5c / 802a5f30 / ...** which DBG_TAIL flags **non-recomp**. But those targets
> are **INTERIOR LABELS of func_802a5b44's own recomp body** (`goto lbl_802a5c5c` exists in it) — they
> are just not REGISTERED function entries. So tail_ppc(ctr)->IsRecompiled(802a5c5c)=false (it only knows
> entries) -> siglongjmp handoff to Dolphin's CPU loop/JIT -> the render continuation (incl. 802a5f50)
> runs under Dolphin JIT -> ppc.gpr[31] diverges -> the 802a6338 crash. (Pointer-discovery's
> 'preceded-by-a-terminator' heuristic in tools/recompiler/main.cpp accepts 802a6398's table @803df424
> — those cases ARE registered, e.g. 802a63e4 — but rejects 802a5b44's interior case labels @803df3f0.)
> FIX (recompiler, per user directive 'no JIT handoffs in boot'): recognize the `lwz rX,(base+idx*4);
> mtctr; bctr` jump-table pattern, read the table, and either register the case targets as recomp
> entries OR emit a C `switch(idx){case k: goto lbl_target;}` so the bctr stays IN-function (no tail_ppc
> handoff). Add a recompiler unit test (sunbright-recomp-test) for the pattern. This is the debug-path
> 'behavior wasn't recompiled -> fix the recompiler' branch; the r31-clobber/JIT-exec notes above are
> all downstream SYMPTOMS of this one missed-jump-table handoff.
> 2) **null-director chicken-egg:** scene state machine `802a6398` (loops on [this+8]; jump table @803df424,
>    states 0..9). Common tail `lbl_802a6644` each iteration: `if(r29==0) call 802a5f50` (DRAW) → director
>    cleanup → **clears [this+4]=0 @802a667c** → state-2 case `lbl_802a669c` (re)creates the director via
>    `802a6dd0` IFF pad-gate `[8040e1b8]&(1<<[this+32].f120)`==0 (gate is OPEN under native: padmask=0). So
>    the DRAW runs BEFORE the director-create in the same frame; first state-2 frame has [this+4]=0, which
>    Dolphin's draw tolerates but native faults on (issue #1, and the [this+4] deref at 802a6160 seen under
>    force_jit). "make it sequential / condvar" does NOT apply (no heap race); the real bug is recomp
>    register preservation in the render path.
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

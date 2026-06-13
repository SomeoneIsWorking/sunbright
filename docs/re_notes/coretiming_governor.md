# CoreTiming vs. the host-clock governor — RE + native-time-ownership design

**Status:** RE + design note. NOT a code change. CoreTiming is load-bearing for boot; this
documents how it works in our hybrid, catalogs the conflict bugs it has caused, and designs a
PC-native event/timer service so emulated time is *owned* rather than negotiated.

**Scope of citations:** `externals/dolphin/Source/Core/Core/CoreTiming.cpp`,
`.../HW/SystemTimers.cpp`, `.../CoreTiming.h`; `runtime/dolphin_hook.cpp`;
`runtime/overrides/{aid_native,sms_vi_native}.cpp`; cross-refs to `CLAUDE.md`.

---

## 1. How CoreTiming actually works in our setup

### 1.1 The event model (Dolphin)
CoreTiming is a single-threaded, cycle-driven event scheduler keyed on a monotonically rising
`global_timer` (PPC clock ticks; GC core clock = 486,000,000 Hz, `SystemTimers.cpp:233`).

- **Event queue** — a min-heap of `Event{time, fifo_order, userdata, type}`
  (`CoreTiming.cpp:284-285`, heap maintained with `push_heap`/`pop_heap`/`make_heap`).
- **`ScheduleEvent(cycles_into_future, type, userdata)`** — pushes `GetTicks()+cycles_into_future`
  onto the heap (`CoreTiming.cpp:258-300`). From a non-CPU thread it goes through a lock-protected
  `m_ts_queue` and is merged at the next `MoveEvents()` (`:333-345`).
- **The downcount is the slice budget.** `Init` sets `downcount = CyclesToDowncount(MAX_SLICE_LENGTH=20000)`
  (`:87`). The JIT/interpreter decrements `downcount` as it executes; when it hits 0 the dispatcher
  calls **`Advance()`**.
- **`Advance()`** (`CoreTiming.cpp:347-388`) is the heartbeat:
  1. `MoveEvents()` (drain the off-thread queue).
  2. `cyclesExecuted = slice_length - DowncountToCycles(downcount)`; `global_timer += cyclesExecuted`
     (`:356-357`). **This is how time advances: by how far the downcount was driven down.**
  3. Fire every event with `time <= global_timer`, calling `type->callback(system, userdata,
     global_timer - time)` — the **`cyclesLate`** is `global_timer - time` (`:364-370`).
  4. Set the next `slice_length` to the gap to the nearest pending event, capped at `MAX_SLICE_LENGTH`
     (`:375-379`), then `downcount = CyclesToDowncount(slice_length)` (`:381`).
  5. **`CheckExternalExceptions()`** — deliver any newly-pending IRQ. The comment at `:384-387`
     is load-bearing: "Pokemon Box refuses to boot if the first exception from the audio DMA is
     received late."
- **`GetTicks()`** (`:237-246`) returns `global_timer`, plus, while a slice is in progress
  (`!m_is_global_timer_sane`), the partial `slice_length - downcount`. So **time read mid-slice is
  interpolated from the downcount**, which matters for us (we drive the downcount directly).
- **`Idle()`** (`:574-588`) zeroes the remaining downcount and books it as idle cycles — used to
  "skip to the next event" without executing guest code.

### 1.2 Who schedules the periodic events (`SystemTimers::Init`, `SystemTimers.cpp:268-289`)
All registered at init and **self-rescheduling** (each callback re-`ScheduleEvent`s itself with
`period - cycles_late`, so a late/batched fire does not drift):

| Event | Period source | Callback | Effect |
|---|---|---|---|
| `VICallback` | `vi.GetTicksPerHalfLine()` | `:119-126` | `VideoInterface::Update()` — VI field timing, XFB present, VI IRQ raise |
| `AudioDMACallback` | `GetAudioDMACallbackPeriod()` (≈ 32 kHz·4/32 B DMA, `:78-93`) | `:85-93` | `DSP::UpdateAudioDMA()` — pushes audio + raises INT_AID |
| `DSPCallback` | `DSP_UpdateRate()` | `:68-76` | `UpdateDSPSlice()` — DSP HLE step |
| `GPUSleeper` | `TicksPerSecond/1000` | `:107-117` | `Fifo::GpuMaySleep()` — lets the GPU thread sleep |
| `PatchEngine` | `vi.GetTicksPerField()` | `:135-159` | AR/patch frame hook (unused by us) |
| `DecCallback` | `DecrementerSet()` on demand | `:128-133` | sets `EXCEPTION_DECREMENTER` |
| `IPC_HLE` | Wii only | — | not used (GC) |

Note `DecrementerSet`/`TimeBaseSet`/`GetFakeTimeBase` all derive the guest TB/DEC **from
`GetTicks()`** (`SystemTimers.cpp:172-208`). **TB is a pure function of CoreTiming ticks.** This is
why a frozen `global_timer` freezes guest time entirely (any `OSGetTime`/timeout loop spins
forever) — documented at `dolphin_hook.cpp:288-297`.

DI (DVD) completions and EXI/memcard completions are **not** in the periodic table — they are
one-shot `ScheduleEvent`s posted by their device handlers, due `cycles_into_future` later. These are
exactly the "deliver later" events that the governor strands (see §2).

### 1.3 Dolphin's Throttle (the pacer we fight)
`Throttle(target_cycle)` (`CoreTiming.cpp:436-495`) sleeps the CPU thread until the host time that
corresponds to `target_cycle` at the configured emulation speed (`SleepUntil`, `:410-434`). It is
driven by `MAIN_EMULATION_SPEED`. **We set `MAIN_EMULATION_SPEED=0` on recomp runs**
(`main_sdl.cpp`; CLAUDE.md "Whole-game crawl fixed"), which makes `IsSpeedUnlimited()` true
(`:397-400`) so Throttle becomes a no-op and our governor is the sole pacer. `UpdateVISkip`/`GetVISkip`
(`:522-535`) and `m_throttle_disable_vi_int` are also disabled in that mode.

### 1.4 How OUR governor currently drives CoreTiming (`dolphin_hook.cpp`)
We do **not** run Dolphin's CPU dispatcher loop for recomp code. Recomp runs on the native C stack,
so nothing decrements the downcount or calls `Advance()` on its own. We synthesize both:

- **`charge_guest_time()`** (`:372-443`), called at the top of every `call_ppc` (`:478`):
  - Armed only after the GC OS sets the current-thread pointer (`:376-382`) — charging earlier
    vectors device/DEC events into the not-yet-installed-handlers window (run12 crash, `:374-375`).
  - `ppc.downcount -= kCyclesPerCall` (96, a coarse per-call cost, `:298`,`:386`).
  - When the downcount expires: mask EE (so `Advance`'s `CheckExternalExceptions` only makes IRQs
    *pending*, never delivers mid-tree — `:389-395`), call `Advance()` (`:397`), then **catch
    emulated time up to the host clock in ONE batched `Advance`** (`:398-423`): compute the
    host-clock `target` from the file-static anchor (`g_host0`/`g_ticks0`), set
    `downcount = slice_length - deficit` so the next `Advance` jumps `global_timer` straight by
    `deficit`, fire it. Self-rescheduling periodic events re-fire inside that one Advance until they
    catch up (`:402-405`) — **no event-by-event walk** (the old walk advanced one 20k slice per
    iteration = ~19 ms/frame of futile spinning = the gameplay jitter, `:405-406`; CLAUDE.md
    "Frame jitter = pacing"). Then deliver any pending IRQ natively at this boundary (`:432-441`).
  - **`g_anchored`/overclock guard** (`:412`): the batched jump only runs at 1.0× OC (the
    downcount↔cycles identity only holds there).

- **`sb_time_ahead()`** (`:329-371`) — the governor predicate every advancing path consults:
  - Boot/loading runs **uncapped** until the first real audio push (`na_ever_pushed`, `:340`).
  - Steady state: compute host-clock `target = g_ticks0 + elapsed·TicksPerSecond`; if emulated time
    is >250 ms behind, **slip the anchor** (`:355-358`) so a stall never triggers a fast-forward
    burst.
  - **Audio servo on top** (`:359-369`): the host audio sink is the master clock. While the native
    sink fill is below `kCushionMs=80`, emulated time may run up to `kMaxLeadMs=200` past the host
    clock to refill it; once cushioned, lock to 1×. `kCushionMs` MUST exceed the sink's start gate
    (`kGateMs=60`) or boot deadlocks (`:341-343`).

- **Every advancing path is gated by `sb_time_ahead()`** so they don't sum into an over-rate
  (`:301-306`):
  - `charge_guest_time` (`:383`),
  - `sunbright_wait_vi_field` — the VI heartbeat (`:1539`): force one VI field of `Idle()+Advance()`
    unless already ahead; invoked from the native `VIWaitForRetrace` override
    (`overrides/sms_vi_native.cpp:152`),
  - `sunbright_poll_yield` (`:1448`) and `idle_run` (`:1900`).

So today emulated time is a **negotiation**: our governor pushes the downcount and calls Advance,
but Dolphin still *owns* the event queue, the periodic reschedules, the `cyclesLate` accounting, and
(notionally) Throttle. The conflict surface is exactly that boundary.

---

## 2. Catalog of CoreTiming-vs-governor conflict bugs (each one a black box we already hit)

Every entry is a place where "Dolphin schedules on its own timeline; our governor parks/jumps that
timeline" produced a real freeze/crash/jitter. Cross-referenced to CLAUDE.md.

1. **Time-parked 0-cycle device events never fire.** The host-clock governor parks `global_timer`
   at the target; any event scheduled `0` cycles into the future (CP `SetTokenFinish`, PE token,
   memcard `memcardTransferCompleteA`) is `time <= global_timer` only on the *next* Advance — but
   Advance stops the instant time reaches target, so the event is unreachable.
   - CP `m_interrupt_waiting` wedged the GPU loop at 99% = the **logo boot freeze** → fixed by
     servicing CP/PE natively in `poll_yield` (`dolphin_hook.cpp:1452-1464`; CLAUDE.md
     "Time-independent device service").
   - memcard EXI DMA completion lost → `__CARDSync` slept forever → **file-select dead** → fixed by
     a native CARD layer (`overrides/native_card.cpp`; CLAUDE.md "File-select fixed").

2. **Dolphin's Throttle sleeping inside a guest worker's token slice = 6–12 fps at "speed 1.0".**
   Two governors fought: ours parks time, then when a worker thread caught CoreTiming up via
   `charge_guest_time`, Dolphin's `Throttle` slept ~16 ms **per VI field** inside that slice
   (`CoreTiming.cpp:436-495` → `SleepUntil`). Fixed by `MAIN_EMULATION_SPEED=0` (CLAUDE.md
   "Throttle/governor conflict"; "Whole-game crawl fixed"). **This is the cleanest evidence that the
   two pacers are incompatible** — the durable answer is to not run Dolphin's pacer at all.

3. **Frame jitter from the event-by-event catch-up walk.** Closing a multi-ms deficit by looping
   `Idle()+Advance()` advanced only one ≤20k-cycle slice per iteration (`CoreTiming.cpp:360,377`),
   burning ~19 ms/frame and never closing the deficit → stddev 12 ms. Fixed by the **single batched
   Advance** (`dolphin_hook.cpp:398-423`; CLAUDE.md "Frame jitter = pacing", stddev 12→0.88 ms).
   This works *because* periodic events self-reschedule with `period - cycles_late`
   (`SystemTimers.cpp:74,91,124`) so a far jump re-fires them rather than skipping them — but it is a
   delicate property of Dolphin's reschedule arithmetic that we are leaning on, not owning.

4. **AudioDMACallback firing at the wrong time / starving.** `AudioDMACallback` (`SystemTimers.cpp:85-93`)
   both pushes audio AND raises INT_AID. Under the governor its CoreTiming delivery starved
   (`/nintr intr7 ~2.5/s vs ~57/s`) while samples kept being pushed → voices froze → the
   **dead-jingle** class. Fixed by owning the AID raise natively at the `UpdateAudioDMA` --wrap seam
   and delivering from `poll_yield` (`overrides/aid_native.cpp`; CLAUDE.md "AID/DSP-interrupt chain
   native"). The audio DMA *event* still rides CoreTiming, but its *interrupt* is now ours — a split
   that exists precisely because the CoreTiming timeline can't be trusted to deliver on our clock.

5. **`Advance`'s terminal `CheckExternalExceptions` delivering mid-tree.** Because recomp never
   consults `ppc.pc` mid-tree, an IRQ delivered inside `Advance` (at `CoreTiming.cpp:387`) lands on
   the mid-tree global ppc with no ISR to run → the `cc006800` "Unable to resolve" MMIO storm
   (run13). We mask EE around every governor-driven Advance (`dolphin_hook.cpp:389-395`,`1447`,`1533`)
   to force pending-only. This is a standing hazard baked into the seam.

6. **`Idle()+Advance()` not moving the timer (unprimed slices).** Outside the JIT loop,
   `Idle()+Advance()` alone left `global_timer` at +0 (native_threading.md Attempt 1) because the
   slice/downcount bookkeeping wasn't primed; only `charge_guest_time` keeping the downcount alive
   makes the idle driver's Advance move time (`dolphin_hook.cpp:1904-1912`). Another symptom of
   driving Dolphin's slice machine from outside its intended loop.

7. **Device service starvation forcing native poll_yield service.** Because parked time strands
   CP/PE/DSP/JAS delivery, `poll_yield` had to grow native CP level service, PE token drain, JAS
   deferred-mail flush, AID pump, and JAS driver pump (`dolphin_hook.cpp:1452-1483`). Each is a
   subsystem we now service on our clock *next to* CoreTiming because CoreTiming couldn't be trusted
   to fire it on time.

**Pattern:** every fix above is a *piecemeal extraction* of one event/IRQ out of CoreTiming onto our
clock, while the rest still rides Dolphin's timeline. The conflict is structural: two schedulers,
one clock. The design below proposes consolidating ownership.

---

## 3. Design — a PC-native event/timer service the governor drives directly

**Goal:** emulated time and event firing are produced by *our* host-clock-paced timeline. CoreTiming
becomes a thin shim (or is bypassed for the events we own), not a co-scheduler.

### 3.1 Core idea
Introduce a `SbTimeline` owned by the runtime:

```
struct SbEvent { u64 due_ticks; u64 seq; void(*cb)(u64 userdata, s64 late); u64 userdata; };
class SbTimeline {
  u64 now_ticks;                 // OUR global timer (the single source of truth for guest time)
  min-heap<SbEvent> queue;       // our event queue
  // schedule(delta, cb, userdata); advance_to(target_ticks) { fire all due, set now_ticks }
};
```

The governor (`sb_time_ahead`'s host-clock anchor) computes `target_ticks` from wall time + audio
servo exactly as today, and calls `timeline.advance_to(target_ticks)` in ONE step. Firing is
identical in spirit to `CoreTiming::Advance` `:364-370` but on *our* heap, with `late = now - due`.

Crucially, **`GetTicks()` must return `SbTimeline::now_ticks`** so the guest TB/DEC
(`SystemTimers.cpp:GetFakeTimeBase/GetFakeDecrementer`) stay coherent. That is the one hard
coupling: TB is derived from ticks, so whoever owns ticks owns time.

### 3.2 Seams — what to intercept vs. keep
We already have `GetGlobals()`/`GetOverclock()` added to the fork's `CoreTiming.h` (`:160,171`), so
we can modify Dolphin minimally (it is our fork now — CLAUDE.md "Dolphin fork").

**Intercept / own:**
- **Tick source.** Make `GetTicks()` read our `now_ticks`. Simplest faithful route: keep
  `m_globals.global_timer` as the single integer but stop letting Dolphin's `Advance` own its
  progression — our `advance_to` writes it. (Today `charge_guest_time` already writes the slice so
  Advance jumps it; this formalizes that.)
- **Event registration/scheduling.** Provide a runtime `sb_schedule_event` and route the events we
  choose to own through it. For events Dolphin's own subsystems schedule (VI/DSP/AudioDMA/DI), the
  cleanest fork-side seam is to make `CoreTimingManager::ScheduleEvent` *for the owned event types*
  forward to `SbTimeline` instead of the heap (a per-EventType "owned" flag set at
  `RegisterEvent`). This keeps Dolphin's callbacks (`VICallback`, `AudioDMACallback`, …) intact —
  they still run, just fired by our heap on our clock — so we inherit Dolphin's correct VI/DSP
  device logic without reimplementing it.
- **Firing.** `advance_to` fires the owned heap; `late` is passed so the self-reschedule arithmetic
  (`period - cycles_late`) keeps working unchanged.

**Keep as Dolphin's (do NOT port):**
- The device callbacks themselves (`VideoInterface::Update`, `DSP::UpdateAudioDMA`, DI completion
  handlers) — these are correct HW logic; we only change *when* they fire, not *what* they do.
- Save-state of the queue (we don't use save states on recomp runs).
- Throttle/SleepUntil — already inert (`MAIN_EMULATION_SPEED=0`); leave dead.

### 3.3 Interrupt delivery (unchanged discipline)
Our timeline must keep the EE-masked / pending-only / native-dispatch discipline (CLAUDE.md
"Interrupt-delivery hazard"). `advance_to` makes IRQs pending; `native_dispatch_pending` /
`sunbright_deliver_pending_recomp` deliver at the recomp boundary, exactly as
`charge_guest_time` does today (`dolphin_hook.cpp:432-441`). No `CheckExternalExceptions` inside a
recomp tree, ever.

### 3.4 Migration path (opt-in, incremental — gated by an env flag, e.g. `SUNBRIGHT_OWN_TIME`)
CoreTiming is load-bearing for boot, so migrate one event class at a time behind a flag, A/B-able
against the current path (which already works). Order by isolation/payoff:

1. **AudioDMA timeline first** (smallest, highest payoff, lowest risk). We already own the AID
   *raise* (`aid_native.cpp`) and the audio sink is already our master clock. Owning the
   *AudioDMACallback firing cadence* on the audio servo removes the last place audio timing rides
   CoreTiming. Verify with `SUNBRIGHT_DBG_NAUDIO` (push/s vs 32028 Hz) and the WAV dumps.
2. **VI-field timeline.** Fold `sunbright_wait_vi_field`'s forced field and the periodic `VICallback`
   into one owned VI event fired at host frame cadence. This unifies the heartbeat (today it is
   *both* a periodic CoreTiming event *and* a forced field in the override — a double source). Verify
   via the watchdog VI-field counter + present-ring (`/interp60`).
3. **DI / one-shot completions.** Route DVD/EXI completion `ScheduleEvent`s to the owned heap so
   "deliver later" can't be stranded by parked time — generalizing the per-device native fixes
   (CARD) into one mechanism.
4. **DSP/decrementer last** — DSP HLE step + DEC; lowest urgency, most boot-critical.

At each step: keep the old path under the flag-off branch, run the harness/headless, only flip the
default when a step is verified for a full boot→gameplay run.

### 3.5 Risks (flagged honestly)
- **Boot ordering is fragile.** `charge_guest_time` is deliberately *not armed* until the OS sets
  the current-thread pointer (`:376-382`); owning time earlier re-creates the run12 wild-vector
  crash. The native timeline must inherit the exact same arming gate.
- **The `period - cycles_late` reschedule contract.** Our batched far-jump correctness depends on
  every periodic callback rescheduling relative to `cycles_late` (`SystemTimers.cpp:74,91,124`). If
  we own scheduling we must preserve `late = now - due` precisely, or VI/DSP/audio drift.
- **TB/DEC coherence.** `GetFakeTimeBase` reads `GetTicks()` every call from many threads
  (`SystemTimers.cpp:203-208`); `now_ticks` must be advanced atomically/consistently or guest
  timeouts misfire. CoreTiming today assumes single-CPU-thread access (`:235-236`) — our timeline
  must hold the same invariant (only the token-holding CPU thread advances).
- **Hidden CoreTiming consumers.** Anything in Dolphin that reads `global_timer` directly
  (PerformanceMetrics, GpuMaySleep cadence) must be checked; folding them into `now_ticks` keeps
  them coherent, *not* folding them desyncs them.
- **Save states / determinism** — abandoned on our path, so no risk, but don't pretend the queue
  serialization still works.
- **The biggest risk is scope creep into a from-scratch scheduler**, which `docs/native_threading.md`
  records as already-tried-and-abandoned for the *thread* scheduler ("wrong layer"). The event
  timeline is a smaller, well-bounded object — keep it that way; do NOT reimplement device logic.

---

## 4. Conclusion

**Is a native event/timer service worth it? Yes — and we are already 70% of the way there by
accident.** Every conflict bug in §2 was fixed by yanking one more thing off CoreTiming's timeline
onto our host clock (CP/PE/CARD/AID/JAS service in `poll_yield`, the batched catch-up Advance, the
forced VI field, `MAIN_EMULATION_SPEED=0`). The result is a half-owned timeline: our governor
computes the target and drives the downcount, but Dolphin still owns the heap, the reschedule
arithmetic, and the firing — and that split is exactly where the remaining black-box bugs live
(time-parked events, double VI-field source, the standing mid-tree-delivery hazard). Consolidating
into one `SbTimeline` that the governor drives directly turns "negotiate with CoreTiming" into "own
the clock," which is the project's stated direction (CLAUDE.md "Dolphin independence",
"PC-game architecture directive").

**It is NOT worth a blind big-bang port.** CoreTiming is load-bearing for boot (arming order, the
`period - cycles_late` contract, TB coherence). The win comes from an opt-in, one-event-class-at-a-time
migration with the current working path as the A/B baseline.

**Smallest valuable first step: own the AudioDMACallback cadence on the audio servo
(`SUNBRIGHT_OWN_TIME`-gated).**
- **Why first:** lowest risk (we already own the AID raise and the sink is already our master
  clock), highest payoff (removes the last audio-timing dependence on CoreTiming — the §2.4 starve
  class), trivially verifiable headlessly (`SUNBRIGHT_DBG_NAUDIO` push/s vs 32028 Hz + WAV dumps),
  and it exercises the whole `SbTimeline` mechanism on one well-understood event before touching
  boot-critical VI/DSP.
- **Exact seams:**
  - Fork `CoreTiming.h`: add a per-`EventType` `owned` flag (set in `RegisterEvent`), already have
    `GetGlobals()`/`GetOverclock()`.
  - Fork `CoreTimingManager::ScheduleEvent` (`CoreTiming.cpp:258`): if the event type is `owned`,
    forward to `sb_schedule_event(delta, cb, userdata)` instead of the heap. Mark
    `m_event_type_audio_dma` owned at `SystemTimers::Init` (`SystemTimers.cpp:271`).
  - Runtime `SbTimeline` in `dolphin_hook.cpp` (next to `charge_guest_time`): heap + `advance_to`
    called from the existing governor catch-up site (`:398-423`), reusing the `g_host0`/`g_ticks0`
    anchor and the audio servo. Keep Dolphin's `AudioDMACallback` body unchanged — it just fires
    from our heap. Keep EE-masked pending-only delivery.
  - Leave VI/DSP/DI on Dolphin's heap until step 2+.

This delivers a real architectural win (audio time fully owned), is fully reversible, and validates
the timeline design before it touches the boot-critical events.

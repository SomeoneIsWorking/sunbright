# 2026-08-06 — 60fps: the in-between frame was DISCARDED BY THE SWAPCHAIN, not dropped by the pacer

> **Measurement supersession (2026-08-22):** The Mailbox/FIFO queue semantics and dead-counter
> diagnosis remain valid. The later general lesson that alternating wall-clock A/B arms makes clock
> cost suitable for optimization selection is superseded: alternation reduces one confounder but
> cannot make host elapsed time an internal-work measure. Use deterministic work counts and bounded
> no-loss sampling instead.

User report, from playing: interpolated 60fps "drops the interpolated frames when it can't produce
60fps due to performance". Correct, and the cause is not performance.

## Root cause

`sms-recomp/host/main.cpp` set `acfg.vsync = false`. aurora maps that to **Mailbox**
(`extern/aurora/lib/webgpu/gpu.cpp`, `best_present_mode`), confirmed at runtime:

    [aurora::gpu] Using surface format BGRA8Unorm, present mode Mailbox

Mailbox holds ONE pending image, and a newer present **replaces** it — the older image is discarded
and never scanned out. That is the correct trade for a renderer emitting one image per tick and
chasing lowest latency. It is fatal for interpolation, which emits **two** images per tick: whenever
both land inside a single display refresh, the swapchain throws the in-between away *by design*.

Every counter still reads 60 fps, because both images were genuinely presented. The display simply
never saw the first one.

**Fix:** interpolated runs select `vsync = true` → **FifoRelaxed** (else Fifo), where every presented
image is queued and displayed for at least one refresh. Nothing is discarded, and the display's own
refresh does the spacing. Verified in both directions — `SBR_60FPS=1` gives `FifoRelaxed`, without
it the mode stays `Mailbox`, which is still right for the one-image-per-tick case.

## The pacing work that preceded this was tuning a policy whose output was being thrown away

`aurora_replay_midpoint` slept until the tick's midpoint so the in-between image would be *shown* at
the half-tick. It returned early when the tick had already passed that point.

**The counter that would have exposed all of this was dead.** The pacing log printed
`"{} ticks already past the midpoint"` from a `skipped` variable that was declared and never
incremented — the early return happened before the counter block. It printed `0` for every run ever
taken. Once fixed: **94% of ticks late, mean overrun 8.8 ms.**

That sleep is now obsolete under a queued present mode and is skipped, kept behind
`SBR_MIDPOINT_SLACK` so the policies stay comparable.

## A measurement mistake worth keeping

The "hold the in-between longer when late" policy was first A/B'd as **two separate runs**: the
second scored 99.8% late, 36.5 ms overrun, 14% fewer ticks, and I reverted it as a regression.

That comparison was worthless. This machine was carrying five other agents' workloads
(`spiderman_port` ×5, `xenia_oracle`, `tomba2_port`) at **load average 40**, and the two arms ran
minutes apart under completely different contention. A wall-clock pacing policy cannot be A/B'd on a
machine whose load is not controlled.

Redone as an **in-run A/B** — `SBR_MIDPOINT_SLACK=ab` alternates the policy every 300 ticks, so both
arms see the same contention whatever it happens to be:

| arm | late ticks | in-between given visible time |
|---|---|---|
| hold | 2396 | **473 (19.7%)**, mean 7.20 ms |
| no-hold | 2393 | 0 by construction |

Tick counts equal to within 3 in 2400 — the policy cost **nothing** in simulation rate. The two-run
comparison had been measuring the machine's other tenants.

**Lesson, general:** any wall-clock A/B on a shared machine must alternate its arms INSIDE one run.
Two runs minutes apart are two different machines.

## And the framing that was wrong

I reported the resulting 19.7% ceiling as "set by frame cost, not by the pacing policy — nothing in
the pacer can help the other 80%", and the user rejected that: *"you have the code, you can change
how this works."* They were right. The ceiling was not a property of frame cost at all; it was the
swapchain discarding the frames the pacer had correctly produced. Accepting a measured limit as a
constraint, when the whole stack is ours to change, is how a fixable defect becomes a permanent
footnote.

## What is NOT established here

Headless has no display, so the runtime cannot observe whether an image was scanned out. That the
mode is now FifoRelaxed is verified; that the judder is gone is a **headed** check and belongs to
the user, per the standing directive in `docs/60fps/effects.md`. The specific things to watch are
the dash-trail ghost and Mario's shadow, both reported juddery.

Frame cost is still a real and separate problem — the split reports guest logic and present+render
each consuming a large share of the 33.4 ms tick — but it is not what was dropping the frames.

# Reducing Dolphin dependence (strategic direction)

> **⚠ SUPERSEDED (2026-06-14):** the GPU carve-out below is reversed. The user now wants the
> **renderer owned too** and Dolphin removed ENTIRELY. The master plan is
> `docs/native_port_plan.md` — read that first. This doc remains valid for the non-GPU subsystems
> and the rationale/method; ignore only the "Keep Dolphin for GPU" line.

> **Direction (set 2026-06-05, GPU clause now superseded):** Be *less constrained to Dolphin*.
> ~~Keep Dolphin for **GPU rendering only**~~ (now in scope — see native_port_plan.md §3). Gradually
> own everything with PC-native implementations of the *behavior* (not faithful HW/OS emulation — see
> `port-not-emulate`). North star: a real PC port that needs no emulation at all.

## Why
The hard, slow-to-diagnose bugs this project keeps hitting share a root: non-graphics behavior
runs *through Dolphin* (CoreTiming, the GC OS scheduler, the interpreter fallback, DVD/EXI/DSP
timing), so we can only observe it indirectly. Example (2026-06-05): the slow-boot "Nintendo
logo" took a long investigation because the cause was a yield-spin waiting out **Dolphin's
emulated DVD seek latency**, with **Dolphin's CoreTiming** not advancing under our recomp — two
Dolphin internals interacting. Owning these makes the behavior native code we can instrument,
step, and fix directly.

## What we depend on Dolphin for today, and the plan for each

| Subsystem | Today | Plan |
|---|---|---|
| **GPU / GX→Vulkan** | Dolphin VideoBackend + GP FIFO | **KEEP.** Explicit carve-out. Highest complexity, lowest replacement value. |
| **CPU execution** | Ours (recomp); Dolphin interp only for JIT-only funcs (mtmsr/rfi/MMU/HW-SPR), boot handoff, OS scheduler | Recompile more (rfi/mtmsr now modeled). Shrink the interp fallback toward zero. |
| **OS / threading / sync** | Dolphin's CPU loop runs the GC scheduler (OSLoadContext handoff); OS primitives single-stepped | **OWN (highest leverage).** Guest threads → native host threads, native non-blocking sync primitives (the `nthr` *logic*, host-thread substrate — NOT a faithful PPC scheduler). Removes interp dependency for OS code + the context-switch bounce + scheduler opacity. |
| **Time base / event scheduling** | Dolphin CoreTiming (`mftb`, interrupts, device completion) | **OWN (enabler).** A native time/event model so recomp credits time itself; decouples timing from Dolphin. Root of the boot-spin pain. |
| **DVD / asset loading** | Dolphin DVD thread + emulated seek latency (band-aided with FastDiscSpeed) | **OWN (easy, do early).** Serve the game's DVD/JKR read API from the extracted files natively, instant. Supersedes the FastDiscSpeed band-aid. |
| **EXI memcard / SI controllers** | Dolphin EXI/SI | Memcard = native file I/O (easy). Controllers already routed via our input override. |
| **DSP / audio mixing** | Dolphin DSP-HLE + ARAM; output via Cubeb | **HARD — keep for now.** GC DSP microcode. The JAudio engine above it is game code we recomp; revisit later. |
| **Memory / MMIO** | Fast RAM path is ours; MMIO via Dolphin MMU | Keep MMIO via Dolphin until the owning device is native. |

## Priority order (incremental, each independently shippable)
1. **Native time/event model** — so recomp drives time; precondition for owning OS + devices cleanly.
2. **Native OS threading + sync** (host threads, native primitives) — kills the biggest opacity/pain source.
3. **Native DVD/asset loading** (file-backed, instant) — removes a device + the latency coupling.
4. **Native EXI memcard** (file-backed).
5. Keep GPU (and DSP mixing) on Dolphin.

## Cross-cutting: observability
Part of "less constrained" is *seeing* what happens. Owning a subsystem natively means we can
instrument it with our own env-gated diagnostics (cf. `SUNBRIGHT_DISPATCH_PROFILE`,
`SUNBRIGHT_INTERP_PROFILE`, the probe). Build the diagnostic alongside each native subsystem.

## Dolphin as a debugging ORACLE is fine (≠ runtime dependence)
Reducing *runtime* dependence does NOT mean giving up Dolphin as a **reference/oracle** for
debugging. Comparing "what does Dolphin load/decode/show here vs what do we" is the core
differential method (`SUNBRIGHT_DISABLE_RECOMP=1`, the DIFF harness) and is encouraged — it's how
we find where our owned code diverges from correct. Keep using it.

## Honest caveats
- This is a large, multi-step effort; do it incrementally, verifying each step against the
  Dolphin baseline (`SUNBRIGHT_DISABLE_RECOMP=1`) so we don't lose correctness while gaining
  independence.
- GPU and DSP-microcode are genuinely hard and stay on Dolphin for the foreseeable future.
- "Own the behavior, don't emulate the hardware" — a native host-thread scheduler that
  *replicates what the game observes*, not a cycle-faithful GC OS. (See `port-not-emulate`,
  `done-right-over-working`.)

# Keep going — resume Sunbright work from a cold session

Pick up the in-flight work without re-deriving context. Read first, then act.

> ## 🛑 SETTLED DECISIONS — DO NOT REOPEN, DO NOT ASK ABOUT THESE
> These were decided by the user and locked. If your reading makes one feel "open" or
> "unratified," **you are misreading stale historical context — trust the lock, not your
> instinct.** Re-asking these is the #1 way this command has wasted the user's time. Only
> raise one if you have *new evidence that directly contradicts it*, and then say exactly
> what contradicts it — never relitigate from first principles.
>
> 1. **Execution substrate = PC-native host threads. Fibers are DROPPED.** (Pinned 2026-06-03
>    via full transcript audit; the user never endorsed fibers.) The `nthr` scheduler *logic*
>    + switch-hook *mechanism* survive; the substrate does not become fibers. Do NOT ask
>    "fibers vs host threads" — it is answered.
> 2. **One proper path, no env-gated dual logic, no stopgaps kept "to preserve working state."**
>    Done-right-but-not-working beats hacked-but-working ([[done-right-over-working]]).
> 3. **On a laid-out checklist, proceed step by step — do NOT pause to ask which approach**
>    ([[dont-ask-keep-porting]]). Verify each step, then continue.
>
> If you catch yourself drafting an `AskUserQuestion` about any of the above: stop, re-read
> the PINNED section of `docs/native_threading.md`, and proceed instead.

## 1. Reconstruct state (read, don't guess)
1. `git log --oneline -15` and `git status` — what landed recently and what's dirty.
2. Read the **top section of `docs/native_threading.md`** ("✅ Execution-model decision —
   PINNED") — the substrate is DECIDED (PC-native host threads); do not reopen it.
3. Read `docs/native_threading.md` "Next session — ordered integration checklist" for the
   step sequence and which steps are ✅ done.
4. Skim `MEMORY.md` + the `native-threading-plan`, `done-right-over-working`,
   `dont-ask-keep-porting`, and `abort-coredump-hang` memories.

## 2. Current focus (as of 2026-06-03)
**Goal:** native OS threading so the audio-init/blocking stall clears. The stall reproduces
deterministically as a fail-fast `abort()` (exit 134) at `run_jit_sync(80343fe4→803488c0)` —
run headless: `SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_AUTOSTART=1 SUNBRIGHT_RUN_SECONDS=N ./build/sunbright`.

**Done:** the `nthr` scheduler logic + per-context switch hooks, validated in isolation
(`SUNBRIGHT_NTHR_SELFTEST=1`, 5/5 PASS). Fail-fast at the step-budget root cause. NOT wired
into the game yet.

**✅ EXECUTION SUBSTRATE — PINNED (do NOT reopen):** **PC-native host-thread execution; fibers
dropped.** Confirmed 2026-06-03 by a full transcript audit at the user's instruction — the user
never endorsed fibers ("Weren't we going to drop fiber in favor of PC native execution?"). North
star: a real PC port, **not limited by Dolphin or the game's own code**; replicate what the game
does under Dolphin with PC-native architecture; make **non-blocking** native versions of the
blocking OS primitives; one proper path, no env-gating. The gating sub-goal is removing the
Dolphin-interpreter dependency for the OS thread/sync primitives. The `nthr` scheduler logic +
switch-hook mechanism carry over (park/resume = host-thread condvar, not `swapcontext`). Full
rationale + the ordered checklist are in `docs/native_threading.md`.

## 3. How to work here
- Verify each step against a headless run before the next; commit + push verified milestones
  (don't wait to be asked). One `main` branch.
- No env-gated dual logic paths / stopgaps — one proper path (`done-right-over-working`).
- On a laid-out checklist, proceed step by step; don't pause to ask which approach
  (`dont-ask-keep-porting`). The substrate is pinned — do NOT re-ask which model; if something
  genuinely new contradicts it, surface that specifically, don't relitigate the settled fork.
- Scratch artifacts under `scratch/`, never `/tmp`.

## 4. Then
State the reconstructed status in 3–5 lines, name the single next concrete action from the
checklist, and proceed — the substrate is settled.

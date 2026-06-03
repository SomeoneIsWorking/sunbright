# Keep going — resume Sunbright work from a cold session

Pick up the in-flight work without re-deriving context. Read first, then act.

## 1. Reconstruct state (read, don't guess)
1. `git log --oneline -15` and `git status` — what landed recently and what's dirty.
2. Read the **top section of `docs/native_threading.md`** ("⚠ Execution-model decision
   status") — this is the live thread of work and may flag an OPEN decision.
3. Read `docs/native_threading.md` "Next session — ordered integration checklist" for the
   step sequence and which steps are ✅ done.
4. Skim `MEMORY.md` + the `native-threading-plan`, `done-right-over-working`,
   `dont-ask-keep-porting`, and `abort-coredump-hang` memories.

## 2. Current focus (as of 2026-06-03)
**Goal:** native OS threading so the audio-init/blocking stall clears. The stall reproduces
deterministically as a fail-fast `abort()` (exit 134) at `run_jit_sync(80343fe4→803488c0)` —
run headless: `SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_AUTOSTART=1 SUNBRIGHT_RUN_SECONDS=N ./build/sunbright`.

**Done:** the `nthr` scheduler logic + per-fiber switch hooks, validated in isolation
(`SUNBRIGHT_NTHR_SELFTEST=1`, 5/5 PASS). Fail-fast at the step-budget root cause. NOT wired
into the game yet.

**⚠ OPEN DECISION — settle before large work:** the *execution substrate* is unresolved.
The committed "fibers-on-the-EmuThread" model was Claude's instinct, not user-ratified; the
user's north star is **true PC-native — recreate the game's execution on PC, not limited by
Dolphin or the game's own code.** Fibers exist only to dodge Dolphin's `IsCPUThread`/`s_core_mutex`
wall, which itself only exists because guest code still runs under Dolphin's *interpreter*.
The PC-native path: reduce/remove the interpreter dependency (native scheduler + MSR/critical-
section code) → real host threads → Dolphin demoted to swappable backends. The `nthr` scheduler
logic + switch hooks carry over either way.
**Do NOT relabel the substrate "decided" or rip out working code until the user confirms.**

## 3. How to work here
- Verify each step against a headless run before the next; commit + push verified milestones
  (don't wait to be asked). One `main` branch.
- No env-gated dual logic paths / stopgaps — one proper path (`done-right-over-working`).
- On a laid-out checklist, proceed step by step; don't pause to ask which approach
  (`dont-ask-keep-porting`). But the OPEN execution-substrate decision above is a genuine fork
  that the user owns — surface it, don't silently pick.
- Scratch artifacts under `scratch/`, never `/tmp`.

## 4. Then
State the reconstructed status in 3–5 lines, name the single next concrete action, and either
proceed (if the open decision is already settled in the docs) or get the substrate direction
confirmed first.

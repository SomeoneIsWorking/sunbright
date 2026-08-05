# `origin/main` had diverged 346 commits in decomp/sms — stale pre-rebase line, and a live trap

Noticed when a routine `git push origin HEAD:main` on the decomp submodule was rejected as
non-fast-forward: `origin/main` was **346 commits ahead** of the branch holding all the real work.

## What it actually was

* The decomp submodule was being worked on branch **`sunbright`**, not `main`.
* `origin/main` was frozen at **2026-07-17** and is an ancestor of the `pre-rebase-backup-20260717`
  tag — i.e. it is the **pre-rebase line**. The 2026-07-17 rebase onto `doldecomp/sms` produced a
  new history, which was pushed to `sunbright`, and `main` was simply never moved.

## Was anything lost? No — checked, not assumed

Comparing the 346 commits present only on `origin/main` against `sunbright` by subject:

* **344** have an identical subject on `sunbright` — carried through the rebase.
* **1** is a merge commit (`Merge upstream/main: 30 commits of …`). Rebases flatten merges; its
  content arrived via upstream. Expected.
* **1** looked genuinely lost: `Title sparkle-pane init: fix duplicated i<8 loops to single i<13`,
  a real bug fix for `TCardLoad::load()` leaving sparkle indices 8-12 uninitialised.

That last one is **present in the current source**. `src/GC2D/CardLoad.cpp` contains no `i < 8`
loops at all, and the logic survives under upstream's renamed fields (`mSparkleAnimState`,
`mSparkleTimer`), refined further into an 11-vs-13 per-region split. The subject did not match only
because upstream's renaming reworded the commit.

**Verifying by subject alone would have been sloppy** — one apparent miss out of 346 had to be
opened and checked against the live source before concluding nothing was lost.

## Why it was not fine, even with nothing lost

A stale `main` that *looks* authoritative is a trap, and it nearly sprang: the push that exposed
this was `HEAD:main`, which would have force-overwritten the wrong line had it not been rejected.
The repo's own rule is one branch per repo named `main`; two branches with the real work on the
non-obvious one is how that happens.

The same pattern existed in **aurora**, mirrored: local branch `sunbright`, but pushes were going to
`fork/main`, so `fork/main` was current (`a42970d`) while `fork/sunbright` sat stale at `1fef737`.

## Fix, in the order that keeps it recoverable

1. **The pre-rebase line was protected on the remote by the `main` branch ref alone.** Of four
   `pre-rebase-backup*` tags, only one was pushed — and `merge-base --is-ancestor` showed it did
   **not** cover `origin/main`. Moving `main` first would have orphaned 346 commits remotely.
   So: push the dated backup tags first (purely additive), then re-verify coverage. Only after
   `origin/main` was reachable from a **pushed** tag was the move safe.
2. Force-move `origin/main` to the real work; switch the local branch to `main`.
3. Delete `sunbright` in both repos only after confirming it pointed at the *identical commit*
   (decomp) / was an *ancestor* (aurora) — so the deletion could not lose anything.

Result: decomp/sms and aurora each have one branch, `main`, and the superproject's submodule
pointers are unchanged (`dcc521a3`, `a42970d`) because only branch names moved, not commits.

## Left alone, deliberately

`extern/dolphin_fork` is still on a `sunbright` branch. Dolphin is retired per CLAUDE.md, so it is
inactive rather than a live trap; consolidating it is cleanup, not a fix, and was not folded into a
history-touching change.

# The stale claims, triaged (2026-08-12)

`info.py claim check` reports a claim as STALE when the code its evidence rests on has changed
since it was verified. That is the right default — it cannot know whether the change mattered — but
23 unexplained "stale" lines is a backlog nobody can act on. This splits them by asking a question
the tool does not: **did the responsible commit EDIT the file, or only MOVE it?**

Method: for each stale claim, take the commits the check names, and for each dependency file ask
`git show --name-status -M` whether that path appears as an `R100` rename (content identical) or as
a real edit. Reproduce with the snippet at the bottom.

**Nothing here is marked fresh.** A claim's evidence is a measurement, and re-confirming it needs
the measurement, not an argument about whether the diff looked relevant. This is triage: it says
which ones a run has to re-establish and which are stale for a bookkeeping reason.

## EDITED — the code behind them really changed (14)

    C001 C002 C003 C005 C009 C010 C011 C012 C013 C014 C028 C033 C040 C041

These need a run. Two deserve a note rather than a queue slot: **C040 and C041 went stale on a
single commit, 0ad8b3f** — the GPU submission ceiling — which touched `native_render.cpp` only to
add a pass rate limit and a staleness flag on the readback. That changes WHICH frames get scored;
it does not change whether pinning a texture unit alters a frame, which is a property of the scene.
That reasoning is recorded and they are NOT confirmed on it: the point of a rot check is that "the
diff looked irrelevant to me" is not evidence.

## MOVED ONLY — stale for a bookkeeping reason (1)

    C006

Its dependency was relocated with identical content. A move cannot invalidate a claim's subject —
but it CAN leave the `depends:` path pointing somewhere wrong, which is what
`tools/info/registry_paths.py` checks and what it found in C003.

## UNATTRIBUTABLE by this method (10)

    C015 C016 C017 C019 C020 C022 C025 C026 C035 C038

The dependency path does not appear under that name in the commits the check names — the file was
renamed at some earlier point, or the staleness comes from a symbol-scope match this comparison
does not model. **This is not a clean result for them.** They are stale, unattributed, and this
triage therefore covers 15 of 25.

## What changed since the first pass

The first run of this triage covered 13 of 23 and left 10 claims unreachable because they declared
no dependency at all — the check was inferring one from their prose, or failing to. Six now declare
what they rest on (C001, C002, C006, C011, C019, C020), chosen by reading each claim's evidence
rather than by pattern-matching its text.

The visible effect is that the stale count went UP, 23 to 25, and blind claims went 0 to 0 by way
of 1. That is the honest direction: a claim nobody could check is now a claim known to need
re-checking. A registry that reports fewer problems after an audit has usually hidden two.

## The dominant cause

    010e232  5 claims   organize: split sms-recomp/runtime into devices/ and render/
    0ad8b3f  4 claims   SB_TURBO removed the only limit on GPU submission
    3177b31  3 claims   60fps: the interpolation AUDIT

One reorganisation commit accounts for more staleness than any behaviour change in the list.

## Reproduce

```bash
python3 ~/.claude/skills/project-info/info.py claim check > chk.txt
```
then for a claim's commit C and dependency F:
```bash
git show --name-status -M C | grep F      # R100 => moved, anything else => edited
```

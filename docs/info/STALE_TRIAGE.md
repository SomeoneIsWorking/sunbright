# The stale claims, triaged (2026-08-12)

> **2026-08-22 update:** This is a dated snapshot, not the current claim queue. The elapsed-time
> audit falsified C016, C021, and C024; C019/C020/C022/C023 were rewritten around no-loss sampling
> or deterministic work counters and re-confirmed. Run `info.py claim check` for the live backlog.

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

## Result (regenerate with `python3 tools/info/stale_triage.py`)

    EDITED       24   C001 C002 C003 C005 C009 C010 C011 C012 C013 C014 C015 C016 C017
                      C019 C020 C022 C025 C026 C028 C033 C035 C038 C040 C041
    MOVED ONLY    1   C006
    UNRESOLVED    0

All 25 attributed. `EDITED` needs a run; `MOVED ONLY` is stale for a bookkeeping reason and is
still NOT confirmed — a claim's evidence is a measurement, and only a measurement restores it.

Two of the EDITED deserve a note rather than a queue slot: **C040 and C041 went stale on a single
commit, 0ad8b3f** — the GPU submission ceiling — which touched `native_render.cpp` only to add a
pass rate limit and a staleness flag on the readback. That changes WHICH frames get scored, not
whether pinning a texture unit alters a frame. Recorded, and deliberately not used to confirm them:
the point of a rot check is that "the diff looked irrelevant to me" is not evidence.

## Getting from 13/23 to 25/25 — three wrong guesses, one look

Worth keeping, because the same mistake caused all three. Each time I reasoned about where the
data must be instead of looking at it.

1. **"They are rename victims."** Taught the triage to walk `git log --follow`. No change.
2. **"They are submodule paths whose PARENT commits move a gitlink."** Wrote a gitlink differ. No
   change either — because `info.py claim check` already runs git INSIDE the submodule, so the
   hashes it prints are aurora's own. One glance at a `claim check` block, whose commit subjects
   were plainly aurora's, settled in seconds what two rounds of inference had not.
3. **The last three were a regex.** The dependency line is `path.cpp#symbol  [symbol-scope]` when a
   claim narrowed its scope, and the parser required the bracket to follow the path directly, so it
   dropped every symbol-scoped dependency on the floor.

The tool is coarser than the check it reads: a symbol-scoped claim is classified by whether the
FILE changed. That over-reports work and never under-reports it, which is the right direction for a
list whose purpose is deciding what to re-measure.

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

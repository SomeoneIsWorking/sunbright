# The 23 stale claims, triaged (2026-08-12)

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

## EDITED — the code behind them really changed (11)

    C003  GX TEV compare mode: bias==3 selects compare
    C005  BP write-mask register 0xFE is used constantly
    C009  Delfino black background was an EFB-copy quad with no render-to-texture
    C010  GX cull and per-draw scissor were never implemented
    C012  2D/J2D override work already exists
    C013  Super Mario Eclipse integration is deferred
    C014  interp60 pairing tag must be (J3DShape << 32 | mDrawMatrices)
    C028  the 60fps sub-frame's asymmetry is monotone in alpha
    C033  interpolated 60fps requires a queued present mode
    C040  the native renderer still scores edgeIoU 32.2% / lumaCorr +0.72
    C041  no TEV-stage ablation recovers the gap; no draw samples above unit 0

Two of these deserve a note rather than a queue slot. **C040 and C041 went stale on a single
commit, 0ad8b3f** — the GPU submission ceiling — which touched `native_render.cpp` only to add a
pass rate limit and a staleness flag on the readback. That changes WHICH frames get scored; it does
not change whether pinning a texture unit alters a frame, which is a property of the scene. I am
recording that reasoning and NOT confirming them on it: the whole point of the rot check is that
"the diff looked irrelevant to me" is not evidence.

## MOVED ONLY — stale for a bookkeeping reason (2)

    C015  the 22.4 MB/tick indexed-array storage upload is architectural
    C038  the interpolation residual attributed to GAPS is not a fixable gap

Their dependency files were relocated by `010e232` ("split sms-recomp/runtime into devices/ and
render/") with identical content. A move cannot invalidate a claim's subject — but it CAN leave the
`depends:` path pointing somewhere wrong, which is what `tools/info/registry_paths.py` checks and
what it found in C003 today.

## UNATTRIBUTABLE by this method (10)

    C001 C002 C006 C011 C017 C019 C022 C025 C026 C035

For these the dependency path does not appear under that name in the commits the check names —
usually because the file was renamed at some earlier point, or because the dependency was inferred
from the claim's prose rather than declared. **This is not a clean result for them.** They are
stale, unattributed, and the honest reading is that this triage covered 13 of 23. Declaring
`depends:` on them (see `info.py claim add --depends path.cpp#symbol`) is what would bring them
into reach.

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

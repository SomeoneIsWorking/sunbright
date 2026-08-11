# How much of the game the graphics census has actually seen (2026-08-12)

`docs/graphics/graphics_db.tsv` is a census: it records what the port has been OBSERVED to draw.
The file says so at the top. What it did not say, and what a reader would reasonably infer wrongly
from "all 24 stages have rows", is how UNEVEN that observation is.

## The measurement

All 24 stages (1-15, 20-28) contribute at least one row. But eight of them contribute exactly 15
rows each, and those eight row sets are **identical**:

    stage10  stage11  stage12  stage15  stage25  stage26  stage27  stage28

Only ONE row is present in every sampled stage, so those fifteen are not "the universal set" — they
are a common scaffolding that fifteen-row runs happen to stop at. Sixteen stages go beyond it.

## Why it matters

A stage whose sample is exactly the same fifteen emitters as seven other stages has not been
sampled. Two situations produce that and the registry cannot distinguish them:

* the stage really draws nothing else (plausible for stage15 — file-select — and maybe some of
  25-28), or
* the run ended before the stage's own content loaded.

The consequence is a rule, not a nuance: a row's `seen:` list is evidence of where a graphic HAS
been seen, and never evidence of where it is absent. An empty worklist means every OBSERVED graphic
has a verdict, which is a much smaller statement than it looks.

## Distribution

    rows                                        65
    rows present in every sampled stage          1
    stages contributing beyond the common set   16
    stages sampling only the common set          8
    rows observed in exactly one stage          20

    stage-exclusive rows:  stage8 8,  stage4 3,  stage9 3,  stage6 2,  stage1 2,  stage2 2

Deep in a handful of stages, shallow in the rest — and the deep ones are the ones that were played
through rather than booted.

## Reproduce

```python
import collections
rows = [l.rstrip("\n").split("\t") for l in open("docs/graphics/graphics_db.tsv")
        if not l.startswith("#") and l.strip() and not l.startswith("key")]
by_stage = collections.defaultdict(set)
for p in rows:
    for s in filter(None, p[3].split(",")):
        by_stage[s].add(p[0])
thin = [s for s, v in by_stage.items() if len(v) == 15]
print(sorted(thin), "identical:", len({frozenset(by_stage[s]) for s in thin}) == 1)
```

## What would fix it

Longer runs in the eight thin stages, or a per-stage note recording that a stage genuinely has no
distinctive emitters. Either turns "unknown" into a fact; leaving the row counts to speak for
themselves leaves a reader to guess, and the guess that all 24 stages are covered is the wrong one.

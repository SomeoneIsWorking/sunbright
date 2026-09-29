---
id: 0041
kind: issue
status: open
created: 2026-10-01
---

# The decomp declares methods it never defines, and no gate could see it

## The defect

`tools/decomp/hostcheck.py` (I040) proves every decomp translation unit still parses, in both
build modes. It structurally cannot prove a method has a body: **a declaration is a parse-clean
construct.** A class can declare a method, nothing in the tree can define it, and the gate reports
green. So can a `virtual` whose body is `{ }` — which is strictly worse, because it compiles,
occupies a vtable slot, and silently does nothing.

The 2026-09-28 repair pass produced a run of these, and each was found by *reading*, not by a tool:

| found in | method | shape |
|---|---|---|
| `decomp/sms/src/Enemy/generator.cpp` | `TGenerator::load`, `TGenerator::perform` | `virtual`, no body anywhere |
| `decomp/sms/libs/JSystem/src/JKernel/JKRThread.cpp` | `JKRTask::~JKRTask`, `JKRTask::run` | `virtual`, no body anywhere |
| `decomp/sms/src/MoveBG/MapObjRailBlock.cpp` | `TRailBlock::control` | parsed clean, body was `{ }` |
| `decomp/sms/src/System/MSoundMainSide.cpp` | `MSSTageSimpleEnvironmentMonte::proc` | `virtual`, body was `{ }` |

Three separate subagents, three separate files, the same shape. That is the definition of work a
diagnostic should own.

## The fix

`tools/decomp/hollow_methods.py` (I041) reports it, with **NO BODY** (declared, defined nowhere) and
**EMPTY BODY** (defined with a body that does nothing) kept separate, and with pure virtuals,
overloaded names and preprocessor-guarded declarations counted rather than silently folded in.

Its `--selftest` is wired into the canonical gate through `tools/selftest_all.py`, and fourteen
independent sabotages of its production rules are each caught by name.

## Why it is not a gate

The decomp is a *partial* decompilation, so most of what it reports is upstream's own
not-yet-decompiled work rather than a defect here. A gate that is permanently red is ignored; a
threshold that drifts as the decomp improves is not a gate. So the tool measures and ranks, and its
controls are gated.

## What the number is NOT

Two bounded audits classified real samples, and the split is the useful part:

- **459 holes in the game-code MapObj/Enemy families recovered 0 of 459.** Every one of those
  implementation files is **1 line — empty — in `upstream/main`**. A grep across all 8431
  out-of-line definitions in `upstream/main` matched 0 of them, and across all 328 commits and every
  ref, 456 of the 459 appear in **no diff at all** — no commit removed them, because there was never
  anything to remove.
- **A second family told the opposite story.** In the J3D Blocks and JPA visitor headers, 124 of 273
  were the tool's own false positives and the rest were genuine, with upstream leaving 11 inline
  empty destructors undefined.

A brief I wrote claimed the recoverable case was dominant. A subagent falsified that for the largest
single population in the tree, with evidence, and was right to. The framing was a hypothesis.

## The three that matter most

A missing definition is a hole you can find by reading. These compile, occupy a vtable slot, and do
nothing, so no parse-based gate can ever see them. All three are inherited from upstream
byte-for-byte, so no merge caused them and none has an upstream body to recover:

- `decomp/sms/include/MoveBG/MapObjBall.hpp:17` — `TMapObjBall::getDepthAtFloating` (`virtual`)
- `decomp/sms/include/MoveBG/MapObjBall.hpp:131` — `TBigWatermelon::control` (`virtual`)
- `decomp/sms/include/Player/Mario.hpp:621` — `TMario::~TMario` (`virtual`)

Recovering these needs DOL reverse-engineering, not a merge. That is the honest top of the queue.

## Also found, and separate

**The mechanism that deletes definitions is our own fork, not the upstream merge.** Named commits:
`ab11c517` dropped `TAnimalBase`'s constructor and `execWalk`; `236a1424` and `7b65c8bf` dropped
7 `TLightWithDBSet` accessors; `eb2fd714` dropped `TMapObjTree::touchPlayer` and its constructor under
the premise "the upstream decomp ships an empty `MapObjTree.cpp`" — which is **stale**: upstream
ships 344 lines where we hold 210. The pattern is a native-RE port commit replacing a whole upstream
`.cpp` with a cold-RE version and not re-deriving the methods it had no evidence for.

Recovered and landed from that: `TGuide`'s constructor (three live call sites, no definition
anywhere), `TAnimalBase`'s two, and the `TMapObjTree` pair.

**A stale premise in a commit message is a live hazard.** `eb2fd714` discarded 134 lines of upstream
because a claim about upstream was out of date. A reader of the history alone would have no way to
know. See the GMSP01 note in `tools/decomp/hostcheck.py`'s header for the same shape: a version
configuration nobody maintains, upstream included.

## Acceptance

- `python3 tools/decomp/hollow_methods.py --selftest` passes, and each of the fourteen sabotages
  fails by name.
- The recoverable population is worked down to zero, one header at a time, with each recovered body
  naming its upstream `path:line` and its removing commit.
- The backlog population is recorded as reverse-engineering work, not as merge damage, so the two
  are never conflated again.
- The three vtable-reachable `{ }` bodies are recovered from the DOL or explicitly accepted as
  retail behaviour with the evidence for that.

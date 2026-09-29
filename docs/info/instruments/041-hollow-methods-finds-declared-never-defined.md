---
id: I041
kind: instrument
status: measuring
created: 2026-10-01
---

## Instrument

`tools/decomp/hollow_methods.py` — which methods does the decomp declare and never give a body?
`--selftest` is a control suite; `--json` and `--top N` rank the findings.

## Why it exists

`hostcheck.py` (I040) parses every translation unit. That is exactly what it is good at, and
exactly what makes it unsafe to trust for this: **a declaration is a parse-clean construct.** A class
can declare a method, nothing in the tree can define it, and every gate in the repository reports
green. So can a `virtual` whose body is `{ }`, which is worse, because it is reachable through the
vtable and silently does nothing.

The decomp repair pass produced a run of them, each found by hand, not by a tool:

| found by | method |
|---|---|
| subagent, `decomp/sms/src/Enemy/generator.cpp` | `TGenerator::load`, `TGenerator::perform` — `virtual`, no body anywhere |
| subagent, `decomp/sms/libs/JSystem/src/JKernel/JKRThread.cpp` | `JKRTask::~JKRTask`, `JKRTask::run` — `virtual` |
| subagent, `decomp/sms/src/MoveBG/MapObjRailBlock.cpp` | `TRailBlock::control` — parsed clean, body was `{ }` |
| subagent, `decomp/sms/src/System/MSoundMainSide.cpp` | `MSSTageSimpleEnvironmentMonte::proc` — `virtual`, body `{ }` |

Three separate subagents, three separate files, the same shape. That is what a tool is for.

## What it measures

Two verdicts, both defects, over the class declarations in `decomp/sms/include` and
`decomp/sms/libs`:

- **NO BODY** — declared, and no definition anywhere in the tree.
- **EMPTY BODY** — defined, but the body does nothing observable (`{ }`, or a bare constant return).

It is deliberately **not** a linker. It does not resolve calls, check signatures, verify vtables or
layout, or check header self-containment, and it does not claim to.

Three things are counted and reported rather than folded into the total, because a scanner that
silently drops cases is a scanner that reports as if it had covered them:

- **pure virtuals** (178). Implemented by a subclass by definition, so they can never be hollow.
- **overloaded names** (55). One finding per name; presence of a definition is a property of the name.
- **declarations behind a preprocessor region** (20). See the version-macro limit below.

## What it does not cover

- **Only the GMSJ01 configuration.** Exactly one version macro is defined, because that is what
  `configure.py` builds. Code inside `#ifdef VERSION_GMSP01` is never parsed, by this tool or by
  I040. A GMSP01 mode was measured and rejected: only 2 of 580 units fail under it, and one of those
  two is upstream's own inconsistency (`SMSGetGameVideoHeight()` called with no argument in a region
  where `Resolution.hpp` declares the 0-argument overload only for non-GMSP01 — upstream's file makes
  the same call at the same place). Gating on a configuration nobody maintains, upstream included, is
  not coverage. The other failure was real and is fixed: three `std::powf` calls in `decomp/sms/src/MSound/MSound.cpp`,
  a C++11 spelling absent from this toolchain's C++98 mode.
- **C++ name resolution.** A nested class reachable under two qualifiers, or a same-named class in two
  scopes, is handled by matching every suffix of the qualifier chain. That errs toward missing a hole,
  never toward inventing one.
- **Templates and macros.** A method whose name comes from a macro is invisible to it.

## Its own false-positive history, measured

This is the part worth keeping. The first real number it produced was **1575**, and a 40-item audit
found **13 of 40 were wrong** — a 33% false-positive rate, not the ~8% a 12-item sample had suggested.
Every class of error was a real report that was simply wrong, and each is now a control:

| count | error | control |
|---|---|---|
| 51 | `virtual ~TFoo() { }` keyed as the CONSTRUCTOR `TFoo`, reporting a hole in a constructor that is defined | destructor keys as `~Name`, and must not collide with the constructor |
| 9 | an in-class definition whose body starts on the *next* line read as a declaration | brace-balanced lookahead, driven through `scan_lines` |
| 4 | a nested class DEFINED under one qualifier, DECLARED under another | indexed under every suffix of the chain |
| 74 | pure virtuals reported as holes | classified `PURE`, counted separately |
| 77 | a nested class defined with only its innermost qualifier — how dolandecomp actually spells it | alias matching, control drives the real seam |
| 2370 | **declarations lost entirely**: the class stack popped on the closing brace of any method with a wrapped body, so every declaration after it vanished from the scan. The denominator was 6060; it is 8252. | free function after a class must not be attributed to the class |

A seventh defect was found by this tool's own control while it was being written: `_wrapped_body`
returning an empty string made every wrapped body look trivial, and the control caught it.

Current state: **1193 findings**, and a 15-item random sample found **0 of 15 wrong**. The number
moved 1575 → 1506 → 1248 → 1193, and every step down was a false positive removed, not a hole closed.

## What the 1193 findings actually ARE, sampled and classified

The number is not "1193 defects". Two bounded audits classified real samples, and the split matters
more than the total:

- **Upstream's own not-yet-decompiled work.** A 459-hole audit over the game-code MapObj/Enemy
  families (`Igaiga`, `MapObjMamma`, `MapObjBall`, `MapObjMare`, `MapObjMonte`, `MapObjPinna`,
  `MapObjFence`, `MapObjRicco`, `Talk2D2`, and 22 `TMario` methods) recovered **0**. Every one of
  those implementation files is **1 line — empty — in `upstream/main`**: `decomp/sms/src/MoveBG/MapObjBall.cpp`
  is 1 line upstream and 772 lines here. A single grep across all 8431 out-of-line definitions in
  `upstream/main` matched 0 of the 459. And across all 328 commits and every ref, 456 of the 459
  appear in **no diff at all** — there is no commit that removed them, because there was never
  anything to remove. These are dolandecomp work that was never done, not damage on our side.
- **Genuinely recoverable — dropped by our own fork.** A separate audit over the J3D Blocks and JPA
  visitor families found a different story: 273 findings there, of which 124 were the tool's own false
  positives and 79 were genuine, with 11 inline empty destructors that upstream leaves undefined.

The honest taxonomy, and the reason a single headline number is misleading here:

| population | status |
|---|---|
| our own fork deleted the definition | **recoverable now** — take upstream's body, preserve the fork's RE |
| upstream never decompiled it | **backlog** — needs DOL reverse-engineering, not a merge |
| tool false positive | **fixed**, each class now has a control |

A brief that told a subagent the recoverable case was "dominant" was corrected by that subagent's
evidence, which is the correct outcome: the framing was a hypothesis, and it was falsified for the
largest single population in the tree.

## Three findings that are worse than the rest

A missing definition is a hole you can find by reading. A `virtual` whose body is `{ }` compiles,
occupies a vtable slot, and does nothing, so it is invisible to every parse-based gate:

    decomp/sms/include/MoveBG/MapObjBall.hpp:17    TMapObjBall::getDepthAtFloating
    decomp/sms/include/MoveBG/MapObjBall.hpp:131   TBigWatermelon::control
    decomp/sms/include/Player/Mario.hpp:621        TMario::~TMario

All three are inherited from upstream byte-for-byte, so no merge caused them, and none has an upstream
body to recover. They are the honest top of a reverse-engineering queue.

## Why it is `measuring` and not `trusted`, and why it is not a gate

A gate must be able to go red on a regression. This one reports 1193 findings on a **partial
decompilation**, where most are upstream's own not-yet-decompiled methods rather than defects on our
side. Adding it to `tools/verification.py` would mean either a permanently red gate or a threshold
that drifts as the decomp improves — and a threshold nobody can justify is not a gate.

Its `--selftest` **is** wired into the gate, through `tools/selftest_all.py`, which discovers every
tool carrying that flag. Fourteen independent sabotages — each reverting one production rule — were
run against it, and every one is caught by name:

    guard skip · class stack pop · lookahead at the call site · scan_lines call ·
    index_definitions call · find_inline_bodies call · classify filter · dedupe ·
    destructor keying · const-qualifier strip · include-guard exclusion ·
    alias suffix matching · pure-virtual rule · pure counted as PURE

Four of those controls were **dead** when first written — they exercised a helper while production
called it with different arguments, so they passed while the production call site was sabotaged.
That is why the suite drives `scan_lines`, `index_definitions` and `analyse` rather than the
functions those call, and why the sabotage harness verifies the file actually changed before
believing a result: an earlier version of that harness reported PASS for three sabotages that had
never been applied.

## Falsifier

This entry is wrong if a planted hole in a real header is not reported, or if a random sample of
reports contains a method that is in fact defined. Both have been run; the first found its target,
the second found 0 of 15. Re-run the second when the count changes materially, and treat any
non-empty result as a new false-positive class to fix and control.

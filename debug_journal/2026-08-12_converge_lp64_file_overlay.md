# Converging to upstream deleted a class of fix the compiler cannot see

2026-08-12. Rebasing `decomp/sms` onto `doldecomp/sms` and then converging unmarked files to
upstream is the project's accelerator: every file we keep our own version of is a conflict we
pay for again in three days. Two convergence batches ran today. The first adopted 25 files and
the second 48, and both quietly took something back.

## What was lost, and why nothing noticed

**Batch 1 — a hand-RE'd port.** Adopting upstream's `BathtubKiller.cpp` deleted
`TBathtubKillerManager::countActiveKillers`, reverse-engineered from the US DOL at `0x8012f204`
and ported by hand. `AnimalBase.hpp` lost a declaration added for a native port. Both files
build green without them, because nothing in the boot path calls either *yet*.

**Batch 2 — the LP64 file-overlay fix.** Four headers, one root cause:

    J3DModelLoader.hpp   J3DJointFactory.hpp   J3DMaterialFactory.hpp   J3DMaterialFactory_v21.hpp

The J3D2 block structs in these **overlay raw big-endian file bytes**. Every field the decomp
typed as a pointer is a **32-bit file offset** — which is a correct type on a GameCube, where
`void*` is 4 bytes. On a 64-bit host it is 8, so the struct stops matching the on-disk layout,
every subsequent field reads from the wrong place, and `JSUConvertOffsetToPtr`'s `void*`
overload is selected instead of the `u32` one. Ours type them `u32` and say so.

Result: **built green, segfaulted on every run.**

## The reason the guard did not hold

`classify()` split files into "native-guarded" and "unmarked convergence candidate" by looking
for `SMS_NATIVE_PLATFORM`, `SMS_AURORA`, `uintptr_t`, `sb_*`, `STOPGAP`. Not one of those
appears in any of the six files. The LP64 headers say `LP64/native` in a comment; the ported
functions carry a provenance line naming the US address they came from.

So the marker set encoded one idea of what "our work" means — *platform adaptation* — and was
blind to the other two: **a decomp gap we filled**, and **a struct whose TYPES are the fix**.

## What changed

* `NATIVE_MARKERS` now also matches `LP64`, `big-endian`, `byteswap`, `Native port of`, `RE'd`
  and `for the native port`. Verified in both directions — the six files classify GUARDED, and
  files that are genuine candidates still classify unmarked.
* `converge` **runs** the result once through `run-safe.sh` and reports loudly when an adoption
  builds but does not run. Validated against both classes: passes on the good tree, prints
  `exit 139 (SEGFAULT)` on a tree with `J3DModelLoader.hpp` deliberately re-adopted. A gate
  only ever exercised on the passing case is decoration.
* Adoption now **bisects** instead of reverting a failing batch of ten whole, and groups
  candidates so a header and its `.cpp` move together. The old behaviour adopted **0 of 40**
  here — four batches, four reverts — and a tool that converges nothing reads as "upstream has
  nothing to give us".

## The generalisable part

A convergence check that only compiles can only catch fixes that are *syntactically* load
bearing. Three whole categories are invisible to it:

1. a struct whose **field types** are the fix (this one),
2. a function that is **correct but not yet called**,
3. anything guarded by a runtime condition the smoke test does not reach.

The runtime gate covers (3) partially and (1) here only because the boot path loads models. It
says so in its own output — "a floor, not proof; it exercises one stage and cannot see a fix
the boot path never uses." The marker set is what covers (2), and it is only as good as the
conventions our own comments follow, which is an argument for writing that provenance line on
every hand-RE'd port.

## Verification that was accepted

Two runs of the final converged build dump a Delfino frame at 200 presents **byte-identical**
to the pre-convergence build's. Worth knowing separately: that frame is reproducible — six
runs produced the same md5 five times, with one differing frame from the *same binary*, so the
renderer has a rare run-to-run non-determinism unrelated to any of this.

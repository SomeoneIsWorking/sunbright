---
id: 40
title: The decomp's object list named a source that does not exist, and nothing read the list
status: closed
symptom: `configure.py` declared `uart_consolle_io.c`, a file that has never existed, in the middle of the MSL C library's object list. A build reading that list could not find the source. Nothing in the repository noticed, because nothing compared the list against the tree.
state_items: S007,S001
tags: decomp,verification,tooling,upstream-defect
created: 2026-10-01
updated: 2026-10-01
---

## Root cause

`tools/decomp/hostcheck.py` discovered its work by globbing `src/` and `libs/` for `*.cpp`. A glob
can only report what it happened to find, so the checker's coverage was a function of where files
sat rather than a statement about what the decomp builds. It said "580 units checked" and had no
way to notice a unit that stopped being declared, a unit declared twice, or a declared source that
was not there at all — which is exactly the shape of the defect it had just been built to catch,
one level up.

`decomp/sms/configure.py` is the authority for what the decomp's real build compiles: 738 object
names, each with a source path. Reconciling the two was never done, by hand or otherwise. The first
hand reconciliation (finding that the scan had missed every middleware unit under `libs/`) was
recorded as a comment in the tool, which is a note about a past event, not a check.

## What it found

`configure.py:620` declared

    Object(Matching, "PowerPC_EABI_Support/Msl/MSL_C/MSL_Common_Embedded/uart_consolle_io.c")

with a doubled `l`. The file on disk is `uart_console_io.c`, and the identical object is declared
correctly a few lines above in the `PowerPC_EABI_H` library. It is upstream's own typo, present at
the sync point `6ae2aa86` and introduced by `69035bd6` ("Match fdlibm & PPCArch.c"), so a real
`ninja` run of the decomp would fail to find the source. Fixed at our side, with a comment naming
the upstream spelling and the reason, since the same typo will otherwise come back with the next
sync.

## The check, and its control

`hostcheck.py` now reads `configure.py` as an AST — it parses rather than imports, because importing
runs its argparse, writes build files and wants the CodeWarrior toolchain — and reconciles the
declared objects against the tree, using configure.py's own path rule (`configure.py:1296-1300`: a
middleware object is named by the first path component of the object name, not by the library that
lists it, because one library builds another's sources). The unit list is derived from the
declaration rather than from a glob, and **both directions of drift fail the gate**:

- a declared object with no file on disk, and
- a C++ file in the tree that `configure.py` never declares.

Coverage is now reported with denominators rather than implied:

    hostcheck: corpus: configure.py declares 737 object(s); 580 C++ unit(s) parsed;
    154 .c, 2 .cp, 1 .s present but not parsed (not C++);
    0 declared-but-absent; 0 on-disk-but-undeclared

**This check earned its place on its first run**, by finding the typo above.

The controls had to be proven, not assumed, and the first version of the coverage control failed
that test: it asserted on the `declared_absent` *field* rather than on `problems()`, which is what
`main()` turns into a non-zero exit. Deleting the reporting outright left the self-test still
passing — the check had been removed and the instrument said PASS. It now asserts on `problems()`,
and re-running that same sabotage makes the self-test fail and name the missing behaviour.

End-to-end control on the real tree: reintroducing upstream's typo turns the gate red naming the
exact file (`hostcheck: FAIL`, exit 1), and restoring it turns it green.

## Not claimed

- The 154 `.c`, 2 `.cp` and 1 `.s` objects are declared and present and are **not parsed**. They are
  counted in the report rather than quietly dropped, but the gate's "clean" says nothing about them.
  They are MSL C, the PowerPC EABI runtime and assembly, which a host C++ parse would fail on for
  reasons that are not defects; covering them properly is a separate piece of work with its own
  controls, not a flag.
- Reconciling the two sets does not prove the decomp *builds*. It proves every declared C++ source
  exists, is declared, and parses in both modes. Duplicate symbols, missing definitions and vtable
  layout are still outside reach (issue 39).
- `configure.py` lists `uart_console_io.c` twice in the same library, once in `PowerPC_EABI_H` and
  once in `MSL_C.PPCEABI.bare.H`. The set is keyed by resolved path, so the duplicate is invisible
  here. Whether it should be there twice is upstream's business and was not changed.

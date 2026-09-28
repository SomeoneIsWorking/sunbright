---
id: I040
kind: instrument
status: trusted
created: 2026-10-01
---

## Instrument

`tools/decomp/hostcheck.py` — does the decompiled source still parse? Paired with
`tools/decomp/stale_names.py` (investigation, not a gate) and the host shim
`tools/decomp/decomp_host_shim.h`.

## What it measures

Every translation unit `decomp/sms` declares, game code under `src/` and middleware under
`libs/`, in BOTH build modes, because the two fail for different reasons and neither substitutes
for the other:

    plain   C++98, SMS_NATIVE_PLATFORM off   — what upstream doldecomp/sms maintains
    native  C++17, SMS_NATIVE_PLATFORM on   — what this port builds the decomp as

The unit list is derived from `configure.py`'s own object list (parsed as an AST) and reconciled
against the tree in BOTH directions: a declared object with no file, and an on-disk C++ file the
build never declares, each fail the run. Coverage is printed with denominators — 737 declared
objects, 580 C++ units parsed, 154 `.c` + 2 `.cp` + 1 `.s` present but not parsed. It found a real
defect on its first run: `configure.py` declared `uart_consolle_io.c`, a source that has never
existed (upstream's typo, fixed at our side — issue 40).

`-fsyntax-only`. It never links the game, never executes a guest instruction, and produces no
artifact a player could launch. The retired `sms-boot` executor is not being reconstructed by it.

Host-only compatibility (Gekko builtins, the MMIO placement macro, the version macro, `nullptr`)
comes only from the shim, which deliberately does not supply a missing field, type, or header:
those are the defects the tool exists to report.

## Validated by

Two controls, both required before the tool may report a pass:

- a real translation unit discovered from the tree compiles clean in BOTH modes, and
- a copy of that same unit with one reference to a nonexistent member comes back with a
  diagnostic naming that field in BOTH modes, while the un-broken copy is still clean — the second
  half is what makes the first discriminating rather than a checker that always fails.

`--selftest` reports the file it probed, so the claim is attached to an artifact rather than to a
number. An end-to-end control was also run on the real tree rather than a copy: a bogus member
reference appended to `decomp/sms/src/Player/MarioMain.cpp` turned the gate red naming the field in
both modes, and restoring the file turned it green.

It also refuses rather than passes: a missing shim, a `--only` filter that matches no unit, an
unreadable `configure.py`, or either direction of corpus drift is a refusal or a failure with a
named reason, so an empty scan can never read as a clean result. `--corpus-only` reports coverage
and stops.

Current result over the whole tree: **580 units, 580 clean, 0 broken, in each mode**, from a
canonical-gate run that ended `verification passed: 15 asset-free steps` (exit 0).

What that result has actually been shown to do: it went from 238 broken units in plain mode and 204
in native mode (first scan, `src/` only) to 0 (2026-10-01) by naming real defects — half-applied
upstream renames, a wrong-case include, a deleted header, duplicated definitions from the merge —
and it went red again the moment one was reintroduced on purpose. Its first end-to-end gate run
failed, on the new shim file's own clang-format violation, before passing.

The coverage control needed its own correction, and the correction is why it is worth reading: the
first version asserted on a data field rather than on the reporting path that fails the run, so
deleting that reporting left the self-test still passing. It now asserts on `problems()`, and
repeating that sabotage fails the self-test by name (issue 40).

## Guard integration

`tools/verification.py` runs it as the "decomp host compile" step of the canonical gate
(`tools/verify.py`), and `tools/selftest_all.py --asset-free` runs its `--selftest` as part of the
asset-free instrument suite. A translation unit therefore cannot quietly stop parsing again.

## Known failure modes

- Parsing is not linking. A missing definition, a duplicate symbol at link time, or a vtable/layout
  mismatch is outside its reach; this instrument says nothing about whether the decomp would link.
- Header self-containment is not checked: the scan discovers `.cpp` files, so a header that only
  breaks on its own is invisible.
- The 154 `.c`, 2 `.cp` and 1 `.s` objects are declared, present and **not parsed**. They appear in
  the coverage report with a count so the denominator is honest, but the gate's "clean" says nothing
  about them; they are MSL C, the PowerPC EABI runtime and assembly (issue 40).
- `plain` mode is a host parse of the upstream target, not a CodeWarrior build. `__MWERKS__` and
  `GEKKO` are deliberately undefined, so MWCC-only source paths (`asm { }` blocks) are not covered.
  Byte-identical matching is not claimed anywhere; only that the source parses in both modes.
- The decomp's include path carries this port's own `sms-boot/shims` and `sms-boot/assets`,
  because eight game translation units include `<sms_boot_*.h>` OUTSIDE any `SMS_NATIVE_PLATFORM`
  guard (eleven distinct headers across those eight). The coupling is real and reported rather
  than stubbed, but it means the decomp is not self-contained; whether those headers belong in a
  retired tree's directory is a title-owner decision (issue 39).
- `stale_names.py` reads git refs (default `main` vs `upstream/main`), so it reports the COMMITTED
  state, not the working tree, and it exits 0 by design: it is a candidate list for a human to read
  each class declaration, not a pass/fail gate.

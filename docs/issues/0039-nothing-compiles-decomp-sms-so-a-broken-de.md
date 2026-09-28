---
id: 39
title: Nothing compiles decomp/sms, so a broken decomp read as a healthy one
status: closed
symptom: `decomp/sms` contained code that referenced members its own classes did not declare, included headers it did not have, and duplicated whole definitions left by the 2026-09-28 upstream merge. All of it was invisible, because no target in this repository compiled the decomp.
state_items: S007,S001
tags: decomp,verification,tooling,merge-residue
created: 2026-09-28
updated: 2026-10-01
---

## Root cause

Two independent causes, one of which hid the other.

**The blind spot.** The retired `sms-boot` executor was the only thing that ever
compiled `decomp/sms`. It is gone by design — `CMakeLists.txt` builds
`native-render`, `title-adapter` and `tools/gcnport_boot`, and `run.sh` refuses
the decomp runtime — so the decomp had no compiler pointed at it at all. Zero
references to `decomp/sms` appear in `build/compile_commands.json`. That is the
correct end state for the executor, and it left source that cannot compile
looking exactly like source nobody has touched. Two audio files in
`JAISystemInterface.cpp` had been broken for months and were found only by
reading them, during the upstream sync.

**The supply.** The 2026-09-28 merge with `doldecomp/sms` added a fresh batch.
It renamed fields upstream-side and applied some renames to a declaration but not
to its use-sites — `TMarDirector::unk7D` became `mScenario` in the member list
while five call sites kept the old name. Upstream commit `d9cb70af` ("Rename all
definitively-commented unk fields to their known names") renamed both halves
together; a text merge took one. The same merge also brought upstream's version
of a file past a fork file that had gutted it, leaving two definitions of the
same function and two declarations of the same class in one translation unit.

## What was measured

`tools/decomp/hostcheck.py` parses every game translation unit with a host
compiler. It is a parse-and-check: it never links the game, never executes a
guest instruction, and produces nothing launchable. The retired executor is not
being reconstructed.

The decomp has two build modes and they fail differently, so both are checked:

    plain   C++98, SMS_NATIVE_PLATFORM off   — what upstream maintains
    native  C++17, SMS_NATIVE_PLATFORM on   — what this port builds

| corpus | mode | before | after |
|---|---|---|---|
| `src/` only, 382 units (first scan) | plain | 144 clean / 238 broken | 382 clean / 0 broken |
| `src/` only, 382 units (first scan) | native | 178 clean / 204 broken | 382 clean / 0 broken |
| `src/` + `libs/`, 580 units (final scan) | plain | — | **580 clean / 0 broken** |
| `src/` + `libs/`, 580 units (final scan) | native | — | **580 clean / 0 broken** |

The first scan covered only `src/`, so it never saw the middleware under `libs/` at all -- the same
blind spot one level up, and the reason the pre-repair figures above are quoted against 382 units
rather than 580. The 580-unit baseline was never recorded on the broken tree, so it is left as "—"
rather than invented; the number that matters is the one the gate can reproduce on demand.

The failures collapsed to a handful of root causes rather than hundreds of
problems, and each was fixed at the layer that owns it:

| cause | fix |
|---|---|
| accessors returning a field the class had renamed | bind the accessor to the member it names |
| use-sites on a pre-rename member | bind to the current name, evidence from the member list / `git log -S` / the upstream copy at the sync point |
| an enumerator value spelled `MEANING_0x40` | map by value onto the header's enumerator |
| a header include in the wrong case (`J3d` vs `J3D`) | spell it as the tree spells it |
| `<cstdint>` / `constexpr` in a port shim a game TU includes *unconditionally* | make exactly those shims C++98-parseable (10 of them) |
| C++11 in a game TU the C++98 target also has to parse | use the C++98 spelling; keep the fork's LP64/endian/diagnostic deltas |
| two definitions of one function, or a class declared in both a header and its `.cpp` | keep the complete one; the header keeps the declaration or the forward declaration |
| a body sitting under a name that does not match its own declaration | give the body the function whose declaration it matches (this is how `TTrembleModelEffect::init` and `SMS_ResetDamageFogEffect` were untangled) |
| a member the code writes that no header declares | declare it at the offset the code proves, with the evidence in a comment |

Not one of these was silenced: no `#if 0` around live code, no commented-out
statements, no `#define` hiding a name, no shim field invented to satisfy a
call. Where a fix could have changed recovered game behaviour it was not taken —
the semantic expressions were left alone and only the names, includes and C++98
spellings moved.

## The decomp is not self-contained

Eleven game translation units include a `<sms_boot_*.h>` header from `sms-boot/shims` — this
port's own shim directory — and eight of them do so **outside** any `SMS_NATIVE_PLATFORM`
guard, calling into it from unguarded code; eleven distinct headers are reached that way (three
TUs pull in a second one). So the retained decomp, deliberately kept as
readable recovered source and a native-layout evidence adapter, depends on
headers owned by the retired executor's directory, and those headers have to
parse as C++98 because the decomp's own target is C++98.

`hostcheck.py` puts that directory and `sms-boot/assets` on the include path,
because the dependency is real (the port's real native build resolves it too,
`sms-boot/CMakeLists.txt:192`) and hiding it behind a stub would hide a fact.
Ten of the eleven headers reached that way needed a C++98 spelling and got one,
each with its reason in a comment; the eleventh, `sms_boot_reset_fruit.h`, already
parsed and was left alone. The other eleven shim headers, which only native-guarded
code includes, keep their `constexpr` and `<cstdint>` spelling, because downgrading
those would have been a gratuitous change to the port's own build. Every one of the ten
edits is proven rather than assumed: the translation unit that includes that header
unguarded is in the checked corpus and compiles clean in `plain` mode.

Whether the shims belong in a retired tree's directory at all is a design
decision and is not made here. The mechanical consequence is now visible instead
of latent: the scan below reports it, and the decomp gate fails if the coupling
is ever removed without a decision.

## Instruments, and the gate that now runs them

- `tools/decomp/hostcheck.py` — per-TU pass/fail for both build modes, grouped by
  cause, with `--json`, `--only` and `--mode`. Its `--selftest` proves both
  classes against the shipping artifact: a real TU must come back clean, and a
  copy of it referencing a nonexistent member must come back with an error
  naming that field — otherwise the probe would not be discriminating. It
  refuses to report a pass when the shim is missing or when no unit matched, so
  an empty scan can never read as a clean result.
- `tools/decomp/stale_names.py` — placeholder names this fork uses that upstream
  no longer has (STALE: a rename applied to one side only) versus names both
  sides still share (LOCAL: genuinely unnamed). An investigation tool, not a
  gate: it reports candidates and each still wants its class declaration read
  before anything changes.
- `tools/decomp/decomp_host_shim.h` — the four host-only things the target
  toolchain supplied (Gekko builtins, the MMIO placement macro, the version
  macro, `nullptr`). It deliberately does NOT stub a missing field or header:
  those are the defects this is looking for.
- **`tools/verification.py` now runs `tools/decomp/hostcheck.py` as the
  "decomp host compile" step of the canonical gate** (`tools/verify.py`), and
  both new tools are in the gate's ruff check set. Before this, nothing in the
  project would have noticed a translation unit going back to not parsing.

## Verified

    python3 tools/decomp/hostcheck.py
      hostcheck[native]: checked 580 unit(s) (580 clean, 0 with errors)
      hostcheck[plain]:  checked 580 unit(s) (580 clean, 0 with errors)
      hostcheck: PASS

    python3 tools/decomp/hostcheck.py --selftest
      PASS (baseline …/J2DGrafContext.cpp compiles clean in both modes; adding a
      nonexistent member turns it red by name in both modes, and removing it
      turns it green again)

    # end-to-end control, on the real tree, not a temp copy: a bogus member
    # reference appended to src/Player/MarioMain.cpp turns the gate red and names
    # the field in both modes; restoring the file turns it green again.

    python3 tools/selftest_all.py --asset-free     # discovers hostcheck's --selftest
    python3 tools/verify.py                        # includes the new gate step
      ...
      hostcheck: PASS
      100% tests passed, 0 tests failed out of 54
      cpp-quality: clang-format passed for 261 file(s); clang-tidy passed for 153 translation unit(s)
      verification passed: 15 asset-free steps      (exit 0)

The first end-to-end gate run failed, on this change's own new shim file failing clang-format. That
is recorded here rather than tidied away: the gate caught a defect introduced by the work that added
it, which is the behavior being claimed.

## Not claimed

- Byte-identical matching is not verifiable in this environment and is not
  claimed: the Metrowerks CodeWarrior compiler is proprietary, is not present and
  is not downloaded by the decomp's own tooling, and only the USA image exists
  while this fork's symbol map and splits are GMSJ01. `configure.py`'s
  `NonMatching` objects cannot be advanced here. What this issue tracks —
  recovered-and-named source that parses in both build modes — is done.
- `hostcheck.py` parses translation units. It does not link them, so a missing
  definition, a duplicate symbol at link time, or a vtable layout mismatch is
  still outside its reach, and header self-containment is not checked (it only
  discovers `.cpp` files).
- A handful of the fixes carry forward-looking notes in the source (the `tbw_`
  subagents' RE comments, the two `TShine`-shaped members in
  `MapEventSinkShadowMario`, the `TBathWaterManager` gate wiring). Those are
  port-owned notes about known residuals, not unfinished work in this issue.

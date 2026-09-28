#!/usr/bin/env python3
"""hostcheck.py — does the decompiled source still compile?

Nothing in this repository compiles `decomp/sms`. That is deliberate: the retired
executor that used to is gone and must not come back. But it leaves a hole with
a shape this project keeps getting bitten by -- code that references a field name
which does not exist, or includes a header upstream deleted, sits in the tree
being no one able to notice. Two files in JAISystemInterface.cpp were found that
way, by reading, months after they broke.

This is a parse-and-check, not a build and not a run. It never links the game,
never executes a guest instruction, and produces no artifact a player could
launch. It asks one question of every game translation unit -- does it still
compile -- and answers with the file, the line, and the diagnostic.

Host-only compatibility (Gekko builtins, the MMIO placement macro, the version
macro) comes from tools/decomp/decomp_host_shim.h, force-included. Nothing else
is stubbed: a missing field or a deleted header is a defect and is reported as
one, because a checker that supplied those would certify broken code.

Output is per-TU pass/fail plus every distinct diagnostic shape, so a red result
says which of the two dozen causes it is rather than dumping 8,000 lines.
"""

from __future__ import annotations

import argparse
import ast
import collections
import dataclasses
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SMS = REPO / "decomp" / "sms"
SHIM = REPO / "tools" / "decomp" / "decomp_host_shim.h"

INCLUDE_DIRS = (
    "include",
    "libs/JSystem/include",
    "libs/dolphin/include",
    "libs/PowerPC_EABI_Support/include",
    "libs/THPPlayer/include",
    "libs/OdemuExi2/include",
    "libs/TRK_MINNOW_DOLPHIN/include",
    # The decomp's own SMS_NATIVE_PLATFORM sources include <sb_log.h>, which this
    # port's native logging owner provides from sms-boot/shims. Paths resolve
    # against the decomp submodule, so this climbs out of decomp/sms and back to
    # the repository root first. Adding it is faithful -- the decomp really does
    # depend on that header -- and it makes the coupling visible instead of
    # hiding it behind a stub. Tracked as issue 39.
    "../../sms-boot/shims",
    # The same dependency, one directory over: the decomp's loaders include
    # <bmd_swap.h>, <anm_swap.h>, <timg_swap.h> and their siblings, which this
    # port's BE->host asset byteswappers provide from sms-boot/assets. The port's
    # real native build puts that directory on the include path
    # (sms-boot/CMakeLists.txt:192) and compiles the wrappers themselves
    # (sms-boot/CMakeLists.txt:231-237), so the dependency is real; reporting
    # "file not found" for a header the shipping build resolves would be the
    # checker being wrong about the build, not the decomp being broken.
    "../../sms-boot/assets",
)

# The decomp has TWO build modes and they are not interchangeable.
#
#   plain   C++98, SMS_NATIVE_PLATFORM off. This is what the decomp targets and
#           what upstream doldecomp/sms maintains. `nullptr` is a macro == 0 here.
#   native  C++17, SMS_NATIVE_PLATFORM on. This is what THIS port builds the
#           decomp as, and it is why the port's own shim headers under
#           sms-boot/shims use `constexpr` and std::uint32_t.
#
# Checking only one of them produces confident nonsense. A C++98-only check
# reports the shim headers as broken when they are correct for the mode they were
# written for; a native-only check hides that the tree still has not been
# reconciled with upstream's C++98 target. Both are checked by default.
#
# __MWERKS__ is deliberately NOT defined in either mode: it selects MWCC-only
# source paths, including MSL inline `asm { }` blocks no host compiler parses,
# which is a different target rather than a defect. GEKKO is not defined either,
# for the same reason.
#
# VERSION_GMSJ01 is defined because the real build defines exactly one VERSION_*
# macro and the decomp's version.h keys every per-version constant off it. With
# none defined, GMSJ01(x) and GMSP01(x) both expand to nothing and every
# VERSION_SELECT site becomes an empty expression -- 160 diagnostics that say
# nothing about the source. GMSJ01 is this fork's default version (configure.py
# lists it first, and the symbol map is GMSJ01).
MODES: dict[str, tuple[str, ...]] = {
    "plain": ("-std=c++98", "-Dnullptr=0"),
    "native": ("-std=c++17", "-DSMS_NATIVE_PLATFORM=1"),
}
BASE_FLAGS = ("-fsyntax-only", "-w", "-DVERSION_GMSJ01=1")

DIAG = re.compile(
    r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
    r"(?:fatal )?(?P<severity>error|warning): (?P<msg>.*)$",
    re.MULTILINE,
)

NOISE = re.compile(r"^$")


def declared_objects(sms: Path = SMS) -> dict[str, str]:
    """configure.py's own object list, as {repo-relative source path: library}.

    This is the authority for what the decomp's real build compiles, so the
    checker reads it instead of globbing. A glob can only ever report what it
    happened to find: the first version of this tool scanned `src/` and missed
    every middleware unit under `libs/` while still printing a confident pass,
    which is the same blind spot one level up as the merge itself. Reconciling
    against the declaration means a unit that is added, renamed or moved cannot
    fall out of coverage without this tool saying so.

    configure.py is parsed as an AST rather than imported: importing it runs its
    argparse, writes build files and wants the CodeWarrior toolchain paths. The
    two entry shapes are `DolphinLib(lib, [Object(flag, "path"), ...])` calls and
    the plain dicts in `config.libs`.

    The path rule is configure.py's own (configure.py:1296-1300): a middleware
    object is named by the FIRST path component of the object name, not by the
    library that lists it, because one library can compile another library's
    sources -- `MSL_C.PPCEABI.bare.H` builds `PowerPC_EABI_Support/...`.
    """
    tree = ast.parse((sms / "configure.py").read_text(encoding="utf-8"))
    middleware: list[str] = []
    libs: dict[str, list[str]] = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        target = node.targets[0]
        if getattr(target, "id", "") == "middleware_libs":
            middleware = [e.value for e in node.value.elts]
        elif getattr(target, "attr", "") == "libs":
            for libdef in node.value.elts:
                if isinstance(libdef, ast.Call):
                    fn = getattr(libdef.func, "id", "")
                    if fn not in ("DolphinLib", "DolphinLibUnpatched"):
                        raise ValueError(f"unrecognised config.libs entry: {fn}")
                    libs[libdef.args[0].value] = [
                        o.args[1].value for o in libdef.args[1].elts
                    ]
                else:
                    fields = {k.value: v for k, v in zip(libdef.keys, libdef.values)}
                    libs[fields["lib"].value] = [
                        o.args[1].value
                        for o in fields["objects"].elts
                        if isinstance(o, ast.Call)
                    ]
    if not libs:
        raise ValueError(
            "configure.py declared no libraries; refusing to check a corpus "
            "derived from a file that says nothing about it"
        )
    out: dict[str, str] = {}
    for lib, objs in libs.items():
        for name in objs:
            top, _, rest = name.partition("/")
            out[f"libs/{top}/src/{rest}" if top in middleware else f"src/{name}"] = lib
    return out


def source_files(sms: Path = SMS) -> dict[str, str]:
    """Every source-ish file under src/ and libs/, as {path: extension}."""
    out: dict[str, str] = {}
    for root in ("src", "libs"):
        base = sms / root
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if p.is_file() and p.suffix in (".c", ".cpp", ".s", ".cp"):
                out[str(p.relative_to(sms))] = p.suffix
    return out


@dataclasses.dataclass
class Corpus:
    """The checker's own coverage, with denominators and nothing unaccounted for."""

    declared: dict[str, str]
    parsed: list[str]
    not_parsed: dict[str, int]
    declared_absent: list[str]
    undeclared_on_disk: list[str]

    def problems(self) -> list[str]:
        """Everything that makes a 'clean' result smaller than it sounds.

        Either direction is a defect, not a curiosity: a declared object with no
        file is a source the real build cannot find, and an on-disk C++ file the
        build never declares is source this tool would happily report as covered
        while the build ignores it.
        """
        out = []
        for path in self.declared_absent:
            out.append(f"configure.py declares {path}, which does not exist")
        for path in self.undeclared_on_disk:
            out.append(f"{path} is in the tree but configure.py never declares it")
        return out

    def summary(self) -> str:
        skipped = ", ".join(f"{n} {ext}" for ext, n in sorted(self.not_parsed.items()))
        return (
            f"corpus: configure.py declares {len(self.declared)} object(s); "
            f"{len(self.parsed)} C++ unit(s) parsed; {skipped or 'no'} present but not "
            f"parsed (not C++); {len(self.declared_absent)} declared-but-absent; "
            f"{len(self.undeclared_on_disk)} on-disk-but-undeclared"
        )


def corpus(sms: Path = SMS) -> Corpus:
    declared = declared_objects(sms)
    disk = source_files(sms)
    cpp = {p for p, ext in disk.items() if ext == ".cpp"}
    not_parsed: collections.Counter[str] = collections.Counter(
        ext for p, ext in disk.items() if ext != ".cpp" and p in declared
    )
    return Corpus(
        declared=declared,
        parsed=sorted(declared.keys() & cpp),
        not_parsed=dict(not_parsed),
        declared_absent=sorted(p for p in declared if p not in disk),
        undeclared_on_disk=sorted(cpp - set(declared)),
    )


def units(sms: Path = SMS) -> list[Path]:
    """Every C++ translation unit configure.py declares and the tree contains.

    Derived from the declaration, not from a glob, and cross-checked by
    `corpus()`: a declared unit with no file is reported rather than skipped
    quietly, and a file the build never declares is reported rather than counted
    as coverage.
    """
    return [SMS / rel for rel in corpus(sms).parsed]


def compile_one(
    path: Path, mode: str = "plain"
) -> tuple[str, list[tuple[str, int, str]]]:
    cmd = ["clang++", *BASE_FLAGS, *MODES[mode], "-include", str(SHIM)]
    for rel in INCLUDE_DIRS:
        cmd += ["-I", str(SMS / rel)]
    cmd.append(str(path))
    r = subprocess.run(
        cmd, capture_output=True, text=True, errors="replace", cwd=SMS, check=False
    )
    out = r.stdout + r.stderr
    found = []
    for m in DIAG.finditer(out):
        if m.group("severity") != "error":
            continue
        raw = m.group("file")
        try:
            rel = str(Path(raw).resolve().relative_to(SMS))
        except ValueError:
            rel = raw
        found.append((rel, int(m.group("line")), " ".join(m.group("msg").split())))
    name = str(path.relative_to(SMS))
    return name, found


def shape(message: str) -> str:
    """Collapse a diagnostic to its cause, so 286 repeats of one intrinsic
    read as one problem rather than 286."""
    ident = re.search(r"use of undeclared identifier '([A-Za-z_]\w*)'", message)
    if ident:
        return f"undeclared identifier {ident.group(1)!r}"
    notfound = re.search(r"'([^']+)' file not found", message)
    if notfound:
        return f"missing header {notfound.group(1)!r}"
    return message[:96]


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--json", action="store_true", help="machine-readable summary")
    ap.add_argument("--only", default="", help="substring filter on the TU path")
    ap.add_argument(
        "--mode",
        choices=sorted(MODES) + ["both"],
        default="both",
        help="which build mode to check; 'both' is the default because "
        "the two modes fail for different reasons",
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove the checker reports both a clean TU and a broken one",
    )
    ap.add_argument(
        "--corpus-only",
        action="store_true",
        help="report coverage against configure.py's object list and stop",
    )
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not SHIM.is_file():
        print(
            f"hostcheck: REFUSES: {SHIM} is missing, so nothing is being checked and "
            f"a clean result would say nothing about the decomp"
        )
        return 1

    try:
        coverage = corpus()
    except (OSError, SyntaxError, ValueError) as exc:
        print(f"hostcheck: REFUSES: cannot read the decomp's object list: {exc}")
        return 1
    print(f"hostcheck: {coverage.summary()}")
    corpus_problems = coverage.problems()
    for problem in corpus_problems:
        print(f"hostcheck: CORPUS: {problem}")
    if args.corpus_only:
        print(f"hostcheck: {'FAIL' if corpus_problems else 'PASS'}")
        return 1 if corpus_problems else 0

    targets = [p for p in units() if args.only in str(p)]
    if not targets:
        print(
            f"hostcheck: REFUSES: no translation unit matched {args.only!r}; "
            f"an empty scan is not a pass"
        )
        return 1

    modes = sorted(MODES) if args.mode == "both" else [args.mode]
    failed_modes = []
    report: dict[str, dict] = {}
    for mode in modes:
        results: list[tuple[str, list]] = []
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            results = list(pool.map(lambda p, m=mode: compile_one(p, m), targets))

        clean = [n for n, e in results if not e]
        broken = [(n, e) for n, e in results if e]
        kinds: collections.Counter[str] = collections.Counter()
        for _, errs in broken:
            kinds.update({shape(m) for _, _, m in errs})

        print(
            f"hostcheck[{mode}]: checked {len(results)} unit(s) "
            f"({len(clean)} clean, {len(broken)} with errors)"
        )
        if kinds:
            print(f"hostcheck[{mode}]: {len(kinds)} distinct cause(s):")
            for kind, n in kinds.most_common(40):
                print(f"  {n:5}  {kind}")
        if broken:
            failed_modes.append(mode)
            if not args.json:
                print(f"hostcheck[{mode}]: first diagnostics:")
                for name, errs in broken[:20]:
                    for f, line, msg in errs[:2]:
                        print(f"  {name}  {f}:{line}: {msg[:100]}")
        report[mode] = {
            "checked": len(results),
            "clean": len(clean),
            "broken": len(broken),
            "causes": dict(kinds),
            "files": {n: [f"{f}:{l}: {m}" for f, l, m in e] for n, e in broken},
        }

    if args.json:
        print(json.dumps({"corpus": coverage.summary(), "modes": report}, indent=2))
    verdict = "FAIL" if (failed_modes or corpus_problems) else "PASS"
    detail = []
    if corpus_problems:
        detail.append(f"{len(corpus_problems)} corpus problem(s)")
    if failed_modes:
        detail.append(f"modes with errors: {', '.join(failed_modes)}")
    print(f"hostcheck: {verdict}" + (f" ({'; '.join(detail)})" if detail else ""))
    return 1 if (failed_modes or corpus_problems) else 0


def selftest() -> int:
    """A checker that has only ever passed is not a checker.

    Three controls, and each needs both answers to mean anything:
      * coverage: a declared object with no file, and a file nobody declares, must
        both be reported, and a reconciled tree must be quiet;
      * a real translation unit from the tree must come back clean, and
      * a copy of it with one field reference broken must come back with an error
        naming that field.
    If the first is not clean the tool is not measuring the tree; if the second is
    not red it cannot detect the defect class this whole tool exists for.
    """
    corpus_failures = selftest_corpus()
    if corpus_failures:
        print("hostcheck selftest: FAIL — the coverage control did not discriminate:")
        for f in corpus_failures:
            print(f"  {f}")
        return 1

    sample = pick_clean_unit()
    if sample is None:
        print(
            "hostcheck selftest: FAIL — no translation unit in the tree compiles clean "
            "in BOTH build modes, so there is no baseline to prove the checker against. "
            "That is a real finding about the tree, not about this tool."
        )
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        broken = Path(tmp) / "broken.cpp"
        # The red case is a self-contained probe appended to a TU that is known
        # clean, and gated on a define so the same file compiles when the flag is
        # absent. It does not depend on the sample's own class structure -- an
        # earlier version looked for `class X {` to inject into and failed on any
        # file that did not open with one, which is a fact about the sample, not
        # about the checker.
        text = sample.read_text(encoding="utf-8", errors="replace")
        text += (
            "\n#ifdef SUNBRIGHT_HOSTCHECK_SELFTEST_BROKEN\n"
            "namespace { struct HostcheckSelftestProbe { int mReal; }; }\n"
            f"int hostcheck_selftest_fn(HostcheckSelftestProbe p) "
            f"{{ return p.{BROKEN_MEMBER}; }}\n"
            "#endif\n"
        )
        broken.write_text(text, encoding="utf-8")
        fired, clean_when_off = [], []
        for mode in sorted(MODES):
            base = ["clang++", *BASE_FLAGS, *MODES[mode], "-include", str(SHIM)]
            for rel in INCLUDE_DIRS:
                base += ["-I", str(SMS / rel)]
            off = subprocess.run(
                [*base, str(broken)],
                capture_output=True,
                text=True,
                errors="replace",
                cwd=SMS,
                check=False,
            )
            if BROKEN_MEMBER not in (off.stdout + off.stderr) and off.returncode == 0:
                clean_when_off.append(mode)
            on = subprocess.run(
                [*base, "-DSUNBRIGHT_HOSTCHECK_SELFTEST_BROKEN", str(broken)],
                capture_output=True,
                text=True,
                errors="replace",
                cwd=SMS,
                check=False,
            )
            if BROKEN_MEMBER in (on.stdout + on.stderr):
                fired.append(mode)
        if set(fired) != set(MODES):
            missing = sorted(set(MODES) - set(fired))
            print(
                f"hostcheck selftest: FAIL — a reference to a nonexistent member did not "
                f"produce a diagnostic naming it in mode(s) {', '.join(missing)}, so this "
                f"check cannot detect the defect class it exists for"
            )
            return 1
        if set(clean_when_off) != set(MODES):
            # Without this, a checker that simply always failed would pass the
            # red case, which is the failure mode that matters most here.
            print(
                "hostcheck selftest: FAIL — the same file compiled WITHOUT the bad "
                "member reference, so the red case is not discriminating: the probe "
                "is broken, not the tree"
            )
            return 1
    print(
        f"hostcheck selftest: PASS (baseline {sample.relative_to(SMS)} compiles clean in "
        f"both modes; adding a nonexistent member turns it red by name in both modes, "
        f"and removing it turns it green again; the coverage control reports a "
        f"declared-but-absent object and an undeclared on-disk file, and is quiet on a "
        f"reconciled tree)"
    )
    return 0


def selftest_corpus() -> list[str]:
    """Prove the coverage check can go red in BOTH directions.

    A coverage check that has only ever said "clean" is a rubber stamp. These
    controls run on a synthetic tree rather than the real one, so they can
    construct the two failures without deleting a file from the decomp:
      1. a declared object with no file on disk   (the real `uart_consolle_io.c`
         typo, which is how this check earned its place);
      2. a C++ file in the tree the build never declares.
    """
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        sms = Path(tmp)
        (sms / "src").mkdir()
        (sms / "libs" / "JSystem" / "src").mkdir(parents=True)
        (sms / "configure.py").write_text(
            "middleware_libs = ['JSystem']\n"
            "config = None\n"
            "config.libs = [\n"
            "    {'lib': 'main', 'objects': [\n"
            "        Object(Matching, 'Game/Present.cpp'),\n"
            "        Object(NonMatching, 'Game/Gone.cpp'),\n"
            "    ]},\n"
            "    {'lib': 'JSystem', 'objects': [\n"
            "        Object(Matching, 'JSystem/JKernel/JKRHeap.cpp'),\n"
            "    ]},\n"
            "]\n",
            encoding="utf-8",
        )
        present = "int present() { return 0; }\n"
        (sms / "src" / "Game" / "Present.cpp").parent.mkdir(parents=True)
        (sms / "src" / "Game" / "Present.cpp").write_text(present, encoding="utf-8")
        heap = sms / "libs" / "JSystem" / "src" / "JKernel" / "JKRHeap.cpp"
        heap.parent.mkdir(parents=True)
        heap.write_text(present, encoding="utf-8")

        c = corpus(sms)
        if c.parsed != [
            "libs/JSystem/src/JKernel/JKRHeap.cpp",
            "src/Game/Present.cpp",
        ]:
            failures.append(f"expected the two present declared units, got {c.parsed}")
        # Assert on problems(), not on the fields: problems() is what main() turns
        # into a non-zero exit, so a control that only checks the data would keep
        # passing if the reporting were removed. That was this control's first
        # version's own bug, caught by deleting the reporting and re-running it.
        if c.problems() != [
            "configure.py declares src/Game/Gone.cpp, which does not exist"
        ]:
            failures.append(
                f"a declared object with no file did not fail the check: {c.problems()}"
            )

        # Now the other direction: a C++ file nobody builds.
        stray = sms / "src" / "Game" / "Stray.cpp"
        stray.write_text(present, encoding="utf-8")
        c2 = corpus(sms)
        if c2.problems() != [
            "configure.py declares src/Game/Gone.cpp, which does not exist",
            "src/Game/Stray.cpp is in the tree but configure.py never declares it",
        ]:
            failures.append(
                f"an on-disk C++ file the build never declares did not fail the check: "
                f"{c2.problems()}"
            )

        # And a clean synthetic tree must actually be clean, or the controls
        # above would be satisfied by a check that always complains.
        (sms / "src" / "Game" / "Gone.cpp").write_text(present, encoding="utf-8")
        stray.unlink()
        c3 = corpus(sms)
        if c3.problems():
            failures.append(f"a fully reconciled tree still reported {c3.problems()}")

        # A configure.py that says nothing about its objects is not a corpus.
        (sms / "configure.py").write_text("config = None\n", encoding="utf-8")
        try:
            corpus(sms)
        except ValueError:
            pass
        else:
            failures.append("a configure.py with no object list was accepted silently")
    return failures


BROKEN_MEMBER = "mHostcheckSelftestNoSuchField"


def pick_clean_unit(max_probe: int = 60) -> Path | None:
    """A translation unit that compiles clean in every mode, discovered not pinned.

    Pinning a named file made the self-test fail the moment an unrelated real
    defect landed in that file, which reads as "the checker is broken" when the
    truth is "the tree is broken". Discovering one keeps the control valid as
    defects are fixed, and the probe is bounded so this stays fast.
    """
    for path in units()[:max_probe]:
        if all(not compile_one(path, mode)[1] for mode in MODES):
            return path
    return None


if __name__ == "__main__":
    sys.exit(main())

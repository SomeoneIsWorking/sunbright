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
import collections
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


def units() -> list[Path]:
    """Every translation unit configure.py declares, game code and middleware.

    The first version scanned only src/, which quietly covered 382 of the ~438
    objects in configure.py: the 22 J3D middleware units under libs/ were never
    checked at all. `configure.py` lists a middleware object as "JSystem/JKernel/
    JKRHeap.cpp" and its source lives at libs/JSystem/src/JKernel/JKRHeap.cpp, so
    scanning both roots is what matches the decomp's own object list.
    """
    found: set[Path] = set()
    for root in ("src", "libs"):
        found.update((SMS / root).rglob("*.cpp"))
    return sorted(found)


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
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not SHIM.is_file():
        print(
            f"hostcheck: REFUSES: {SHIM} is missing, so nothing is being checked and "
            f"a clean result would say nothing about the decomp"
        )
        return 1

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
        print(json.dumps(report, indent=2))
    verdict = "FAIL" if failed_modes else "PASS"
    print(
        f"hostcheck: {verdict}"
        + (f" (modes with errors: {', '.join(failed_modes)})" if failed_modes else "")
    )
    return 1 if failed_modes else 0


def selftest() -> int:
    """A checker that has only ever passed is not a checker.

    Two controls: a real translation unit from the tree must come back clean, and
    a copy of it with one field reference broken must come back with an error
    naming that field. If the first is not clean the tool is not measuring the
    tree; if the second is not red it cannot detect the defect class this whole
    tool exists for.
    """
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
        f"and removing it turns it green again)"
    )
    return 0


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

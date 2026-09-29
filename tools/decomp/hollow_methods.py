#!/usr/bin/env python3
"""hollow_methods.py — find methods the decomp declares and never gives a body.

`hostcheck.py` parses translation units. That is the one thing it is good at and
the one thing it is bad at being trusted for: a declaration is a parse-clean
construct, so a class can declare a method, nothing in the tree can define it, and
every gate in this repository reports green. A `virtual` whose body is `{ }` is
worse than a missing one -- it is reachable through the vtable and it silently
does nothing.

Both shapes are real here, and the decomp repair pass produced a run of them by
restoring includes without restoring bodies:

    TGenerator::load, TGenerator::perform            (virtual, no body anywhere)
    JKRTask::~JKRTask, JKRTask::run                  (virtual, no body anywhere)
    TRailBlock::control                              (virtual, body was `{ }`)
    MSSTageSimpleEnvironmentMonte::proc              (virtual, body was `{ }`)
    MSStageProc::setBgmPosition                      (body was `{ }`)

So this checks the *definition* side of every method a header declares. It is not
a linker: it does not resolve calls or check signatures, and it deliberately
refuses to count a method as hollow when the declaration is inside a
preprocessor region this tool cannot see, because a `#ifdef VERSION_GMSP01`
declaration is absent from the build the decomp actually makes (see
`hostcheck.py`'s header for why only VERSION_GMSJ01 is parsed).

Two verdicts, both defects:
    NO BODY      declared, and no definition anywhere in the tree
    EMPTY BODY   defined, but the body is `{ }` or `{ return <constant>; }`-free
                 -- concretely, a definition whose braced body is empty or holds
                 only `return false;` / `return 0;` / `return TRUE;`

Report shape is per file, with a denominator: how many classes were scanned, so a
silent zero cannot read as a clean tree.

Usage:
    tools/decomp/hollow_methods.py [--top N] [--json] [--selftest] [--class NAME]
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SUB = REPO / "decomp" / "sms"

HEADER_ROOTS = ("include", "libs")

# `virtual` presence, and the declaration form. Deliberately shallow: it reads
# headers, it does not parse C++.
CLASS_RE = re.compile(r"^\s*class\s+([A-Za-z_]\w*)\s*(?::|\{)")
VIRTUAL_RE = re.compile(r"\bvirtual\b")
# The head of a member declaration OR an in-class definition. The tail is
# classified separately by `classify_member`, because whether a line is a
# declaration or a definition is decided by what follows the parameter list, and
# a regex that tried to span a body could not tell `{ }` from `;`.
DECL_RE = re.compile(
    r"^[ \t]*(?:virtual\s+|inline\s+|static\s+|explicit\s+)*"
    r"(?:"
    r"(?P<type>[A-Za-z_~][\w:~<>\s\*&]*?)\b(?P<name>[A-Za-z_~]\w*)\s*\([^;{]*\)"
    r"|"
    r"(?P<bare>[A-Za-z_~]\w*)\s*\([^;{}]*\)"
    r")"
)


def classify_member(line: str, following: Sequence[str] = ()) -> tuple[str, str] | None:
    """(name, kind) for a member line, or None when it is not a member.

    kind is one of:
      DECLARED  ends in `;` or is a pure `= default` -- the body is elsewhere
      EMPTY     an in-class body that does nothing observable
      DEFINED   an in-class body that does something

    `following` is the lines after this one. Without it, an in-class definition
    whose body starts on the NEXT line -- which is how clang-format writes
    `virtual void receiveMessage(u32, u32)\n{\n    mCount = 0;\n}` -- reads as
    DECLARED and is reported hollow. Nine verified-defined methods in one sample
    were exactly that.
    """
    m = DECL_RE.match(line)
    if not m:
        return None
    # A constructor or destructor declares no return type, so the bare form has no
    # `name` group; either way the method name is the identifier before the parens.
    name = m.group("name") or m.group("bare")
    # A DESTRUCTOR must be keyed as `~Name`. Without the tilde, `virtual
    # ~TLightWithDBSet() { }` keys as the constructor `TLightWithDBSet` and reports
    # a hole in a constructor that is defined in the tree -- 51 of the 1575
    # findings were this one collision.
    if m.group("type") and m.group("type").rstrip().endswith("~"):
        name = "~" + name
    elif m.group("bare") and name.startswith("~"):
        name = "~" + name.lstrip("~")
    tail = line[m.end() :].strip()
    # `perform(u32) const;` -- a trailing qualifier sits between the parens and the
    # terminator, and without stripping it every const method read as a definition.
    for qualifier in ("const", "noexcept", "override", "final"):
        if tail.startswith(qualifier) and (
            len(tail) == len(qualifier) or not tail[len(qualifier)].isalnum()
        ):
            tail = tail[len(qualifier) :].strip()
    if tail.startswith("=") and not tail.endswith("{"):
        # `= 0` is a PURE VIRTUAL. It is by definition implemented by a subclass, so
        # reporting it as a hole is wrong by construction -- 74 of them in the J3D
        # Blocks family alone, and the same mechanism across the tree.
        if re.match(r"=\s*0\s*;?\s*$", tail):
            return name, "PURE"
        return name, "DECLARED"
    if tail.startswith(";"):
        return name, "DECLARED"
    if not tail:
        # The tail is empty, so the terminator, a member initialiser list, or the
        # opening brace is on a later line. A `;` there is still a declaration.
        nxt = _next_code(following)
        if nxt.startswith(";"):
            return name, "DECLARED"
        if not nxt:
            return name, "DECLARED"
        if _is_member_init(nxt):
            return name, "DEFINED"
        if nxt == "{":
            # A lone `{` means the body is on the following lines, which says
            # NOTHING about whether it is empty. Reading it as EMPTY was wrong and
            # the self-test caught it: `{ mCount = 0; }` split over four lines is a
            # real body. Balance the braces and test the text between them; if the
            # window runs out first, assume real work, because a missed EMPTY is
            # recoverable and a phantom EMPTY is a claim about the game.
            body = _wrapped_body(following)
            if body is None:
                return name, "DEFINED"
            # TRIVIAL_BODY expects the braces, and `_wrapped_body` returns the text
            # BETWEEN them, so put them back before testing.
            return name, "EMPTY" if TRIVIAL_BODY.search("{" + body + "}") else "DEFINED"
        return name, "EMPTY" if TRIVIAL_BODY.search(nxt) else "DEFINED"
    if TRIVIAL_BODY.search(line) or TRIVIAL_BODY.search(tail):
        return name, "EMPTY"
    return name, "DEFINED"


def _next_code(following: Sequence[str]) -> str:
    for raw in following:
        s = raw.strip()
        if s:
            return s
    return ""


def _wrapped_body(following: Sequence[str]) -> str | None:
    """The text between the outermost braces that open in `following`, else None."""
    depth = 0
    seen = False
    parts: list[str] = []
    for raw in following:
        for ch in raw:
            if ch == "{":
                depth += 1
                seen = True
            elif ch == "}":
                depth -= 1
                if seen and depth == 0:
                    return "".join(parts)
            if seen and depth > 0:
                parts.append(ch)
    return None


def _is_member_init(first_code: str) -> bool:
    """True for a member initialiser list, which is real work in a constructor."""
    return first_code.startswith((":", ", m"))


# An out-of-line definition. The return type is OPTIONAL because this codebase
# writes both `void TFoo::perform(u32)` and `TCameraMultiPlayer::removePlayer(u32)`
# with no return type at all; a matcher that required one reported 788 hollow
# methods on a tree with a handful, which is a report nobody can act on.
DEF_RE = re.compile(
    r"^[ \t]*(?:[A-Za-z_][\w:<>*&,\s]*[ \t])?"
    r"((?:[A-Za-z_]\w*::)*)([A-Za-z_]\w*)::([~A-Za-z_]\w*)\s*\(",
    re.MULTILINE,
)


def qualifier_key(qualifiers: str, last: str) -> str:
    """Key a method on the LAST TWO qualifier components.

    A nested class is written `void TBGTentacle::TNode::calcVelocity(...)` but
    declared as `void calcVelocity(...)` inside `class TNode`. Without this the
    definition never matches its declaration and the method is reported hollow.
    """
    parts = [p for p in qualifiers.split("::") if p]
    parts.append(last)
    return "::".join(parts[-2:])


# A body that does nothing observable. Deliberately broad about the constant: a
# getter that returns 0 and touches nothing else is exactly the shape that hides a
# missing decompilation, and this tool's job is to make a human look. Every finding
# is reported, not asserted dead.
TRIVIAL_BODY = re.compile(
    r"\{\s*(?:return\s+(?:false|true|TRUE|FALSE|NULL|nullptr|0|1|-?\d+(?:\.\d+)?f?)\s*;\s*)?\}\s*$"
)


@dataclass
class Method:
    cls: str
    name: str
    path: str
    line: int
    virtual: bool
    kind: str = "unknown"  # NO BODY | EMPTY BODY
    defined_at: str = ""


@dataclass
class Scan:
    classes: int = 0
    declared: int = 0
    pure: int = 0
    overloads: int = 0
    hollow: list[Method] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def summary(self) -> str:
        kinds = collections.Counter(
            f"{m.kind} ({'virtual' if m.virtual else 'plain'})" for m in self.hollow
        )
        detail = ", ".join(f"{k} x{n}" for k, n in sorted(kinds.items())) or "none"
        return (
            f"hollow: {self.classes} class(es), {self.declared} declared method(s), "
            f"{self.pure} pure virtual(s) (implemented by a subclass, never hollow); "
            f"{len(self.hollow)} hollow ({detail})"
        )


INCLUDE_GUARD_HINT = re.compile(r"_HPP?$|_HPP_$|^_")


def include_guards(lines: list[str]) -> set[str]:
    """Macros that are include guards, not conditional regions.

    `#ifndef FOO_HPP` / `#define FOO_HPP` / ... / `#endif` wraps a whole file. It
    is a preprocessor region, but not a CONFIGURATION region: nothing inside it is
    conditional on a build option. Counting it as one marks every declaration in
    every header as skipped -- 4737 of 4737, which silently suppressed this tool's
    entire NO BODY pass while the summary still read like coverage.
    """
    guards: set[str] = set()
    for i, line in enumerate(lines):
        m = re.match(r"#\s*ifndef\s+([A-Za-z_]\w*)", line.strip())
        if not m:
            continue
        macro = m.group(1)
        for follow in lines[i + 1 : i + 4]:
            s = follow.strip()
            if s.startswith("#define"):
                if re.match(r"#\s*define\s+" + re.escape(macro) + r"\b", s):
                    guards.add(macro)
                break
            if s.startswith(("#if", "#endif")):
                break
        if macro not in guards and INCLUDE_GUARD_HINT.search(macro):
            guards.add(macro)
    return guards


def guard_state(lines: list[str], upto: int) -> bool:
    """True when the line is inside a preprocessor region this tool cannot resolve.

    Depth is tracked and include guards are excluded. #ifdef is deliberately never
    evaluated: a declaration behind a live #if is skipped and counted, because the
    decomp's real build defines exactly one VERSION_ macro and a declaration behind
    another one is absent from that build (see hostcheck.py's header). Reporting it
    would be a false positive that trains people to ignore the tool.
    """
    guards = include_guards(lines)
    depth = 0
    for line in lines[:upto]:
        s = line.strip()
        m = re.match(r"#\s*ifndef\s+([A-Za-z_]\w*)", s)
        if m and m.group(1) in guards:
            continue
        if s.startswith("#if"):
            depth += 1
        elif s.startswith("#endif"):
            depth = max(0, depth - 1)
    return depth > 0


def scan_lines(lines: list[str], rel: str, scan: Scan) -> None:
    """Scan one header's lines into `scan`. The real call site, and the test's.

    Extracted so the self-test drives THIS function rather than re-implementing the
    lookahead. A control that calls `classify_member(line)` while production calls
    `classify_member(line, lines[i + 1 : i + 24])` passes forever and proves nothing;
    that is exactly what the first version of this control did.
    """
    # A stack of (class name, brace depth just inside its body). A `}` only ends a
    # class when it closes THAT class's body. Matching on the first `}` instead --
    # which is what the first version did -- pops the class on the closing brace of
    # any method with a wrapped body, and every declaration after it disappears from
    # the scan without being counted. A control that fed `classify_member` directly
    # could not see this, which is why the test drives this function.
    stack: list[tuple[str, int]] = []
    depth = 0
    for i, line in enumerate(lines):
        m = CLASS_RE.match(line)
        if m and not line.rstrip().endswith(";"):
            depth += line.count("{") - line.count("}")
            stack.append((m.group(1), depth))
            scan.classes += 1
            continue
        depth += line.count("{") - line.count("}")
        while stack and depth < stack[-1][1]:
            stack.pop()
        if not stack:
            continue
        member = classify_member(line, lines[i + 1 : i + 24])
        if not member:
            continue
        name, kind = member
        cls = qualifier_key("::".join(n for n, _ in stack[:-1]), stack[-1][0])
        if kind == "EMPTY":
            # `void* getObj(int i) { }` is an EMPTY BODY vtable slot, written WITHOUT
            # a `Class::` qualifier, so only this scan knows its enclosing class.
            scan.hollow.append(
                Method(
                    cls, name, rel, i + 1, bool(VIRTUAL_RE.search(line)), "EMPTY BODY"
                )
            )
            continue
        if kind == "DEFINED":
            continue
        if kind == "PURE":
            scan.pure += 1
            continue
        scan.declared += 1
        if guard_state(lines, i):
            scan.notes.append(
                f"{rel}:{i + 1} {cls}::{name} (behind a preprocessor region)"
            )
            continue
        scan.hollow.append(Method(cls, name, rel, i + 1, bool(VIRTUAL_RE.search(line))))


def scan_headers(sub: Path, top: int = 1) -> Scan:
    """Every declared method in `sub`'s header roots. `sub` is a parameter so the
    self-test can run the real pipeline over a synthetic corpus."""
    scan = Scan()
    for root in HEADER_ROOTS:
        base = sub / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.hpp")) + sorted(base.rglob("*.h")):
            rel = str(path.relative_to(sub))
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            scan_lines(lines, rel, scan)
    return scan


def dtor_key(m: re.Match[str]) -> str:
    """The method name as the DECLARATION side will spell it.

    `TLightWithDBSet::~TLightWithDBSet()` puts the tilde on the class qualifier and
    `~TLightWithDBSet` on the method, so the method group is already right. This
    exists so the two sides are guaranteed to produce the same string rather than
    merely looking like they do.
    """
    return m.group(3)


def definition_on_line(line: str) -> list[re.Match[str]]:
    """Out-of-line definitions on one line.

    `void TFoo::bar(u32);` inside a .cpp is a declaration and must not count, or
    the hole it describes never gets reported. The rule lives in one function so
    the scanner and its self-test cannot drift apart.
    """
    if line.rstrip().endswith(";"):
        return []
    return list(DEF_RE.finditer(line))


def index_definitions(lines: list[str], rel: str, out: dict) -> None:
    """Index one .cpp's out-of-line definitions into `out`. Real call site and test's.

    A control that calls `definition_on_line` cannot see whether the bare-name alias
    index is wired up, so it passed while the production path was sabotaged.
    """
    for i, line in enumerate(lines):
        for m in definition_on_line(line):
            where = f"{rel}:{i + 1}"
            name = dtor_key(m)
            qualified = qualifier_key(m.group(1), m.group(2))
            out.setdefault((qualified, name), where)
            # A nested class can be DEFINED under one qualifier and DECLARED under
            # another: declared inside `class JASBasicWaveBank`, defined as
            # `TBasicWaveBank::TWaveGroup::TWaveGroup()`. Resolving C++ inheritance is
            # out of scope, so index the bare class name too. A same-named class in
            # two scopes then hides a real hole; that direction is the safer one,
            # because a missed hole is a rank-order problem and a phantom hole is a
            # report nobody trusts.
            bare = qualified.split("::")[-1]
            if bare != qualified:
                out.setdefault((bare, name), where)


def find_definitions(sub: Path) -> dict[tuple[str, str], str]:
    """{('Class','method'): 'path:line'} for every out-of-line definition in `sub`."""
    out: dict[tuple[str, str], str] = {}
    for root in ("src", "libs"):
        base = sub / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.cpp")):
            rel = str(path.relative_to(sub))
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            index_definitions(lines, rel, out)
    return out


def find_inline_bodies(sub: Path) -> dict[tuple[str, str], str]:
    """{('Class','method'): location} for every method DEFINED in a header.

    Every header-defined body counts as defined, not only the trivial ones. An
    `inline const TBossEel* TBossEelEye::getOwner() const { return mOwner; }` in
    the header is a real implementation; a matcher that registered only the `{ }`
    cases reported it hollow, and each such hit is a false positive the reader has
    to disprove by hand before trusting the rest of the list.
    """
    out: dict[tuple[str, str], str] = {}
    for root in HEADER_ROOTS:
        base = sub / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.hpp")) + sorted(base.rglob("*.h")):
            rel = str(path.relative_to(sub))
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for i, line in enumerate(lines):
                if line.rstrip().endswith(";"):
                    continue
                for m in DEF_RE.finditer(line):
                    key = (qualifier_key(m.group(1), m.group(2)), m.group(3))
                    out.setdefault(key, f"{rel}:{i + 1}")
    return out


def analyse(sub: Path) -> Scan:
    """The whole pipeline, over `sub`. `main()` and the self-test both call this."""
    scan = scan_headers(sub)
    classify(scan, find_definitions(sub), find_inline_bodies(sub))
    return scan


def classify(scan: Scan, defs: dict, inline: dict) -> Scan:
    """Drop declarations that have a definition; keep the two verdicts distinct.

    `inline` holds every method DEFINED in a header, so a real inline
    implementation is not a finding. Entries already carrying the EMPTY BODY
    verdict are verdicts in their own right and are not filtered: a `{ }` virtual
    is reachable through the vtable and silently does nothing.
    """

    def defined(m: Method) -> bool:
        """True when EITHER spelling of the class qualifier has a definition.

        A nested class is DECLARED as a full stack path and very often DEFINED with
        only the innermost qualifier: dolandecomp writes
        `void JPADrawExecStripeCross::exec(...)` for a class nested three deep. Keying
        on one string made 77 of those look hollow. The class name's suffixes are all
        candidate keys.
        """
        parts = m.cls.split("::")
        candidates = {"::".join(parts[i:]) for i in range(len(parts))}
        return any((c, m.name) in defs or (c, m.name) in inline for c in candidates)

    scan.hollow = [m for m in scan.hollow if m.kind == "EMPTY BODY" or not defined(m)]
    for m in scan.hollow:
        if m.kind == "unknown":
            m.kind = "NO BODY"
    return scan


def dedupe(scan: Scan) -> Scan:
    """One entry per (class, method).

    Overloads share a name, and presence-of-definition is a property of the name, so
    listing `J3DColorBlockLightOff::setAmbColor` twice tells the reader nothing. The
    count of distinct names is the honest denominator; the number of OVERLOADS is
    reported separately so nothing is silently hidden.
    """
    seen: set[tuple[str, str]] = set()
    kept: list[Method] = []
    overloads = 0
    for m in scan.hollow:
        key = (m.cls, m.name)
        if key in seen:
            overloads += 1
            continue
        seen.add(key)
        kept.append(m)
    scan.overloads = overloads
    scan.hollow = kept
    return scan


def report(scan: Scan, top: int, as_json: bool) -> int:
    if as_json:
        print(
            json.dumps(
                {
                    "classes": scan.classes,
                    "declared": scan.declared,
                    "pure_virtual": scan.pure,
                    "summary": scan.summary(),
                    "hollow": [m.__dict__ for m in scan.hollow],
                    "skipped_behind_guards": len(scan.notes),
                    "overloads_collapsed": scan.overloads,
                },
                indent=2,
            )
        )
        return 0
    print(scan.summary())
    print(f"  overloaded names collapsed to one finding each: {scan.overloads}")
    print(f"  declarations skipped behind a preprocessor region: {len(scan.notes)}")
    if not scan.classes:
        print(
            "hollow: REFUSES: no class was scanned, so an empty result here means the scan "
            "read nothing, not that the tree is clean."
        )
        return 1
    if not scan.hollow:
        print(
            "hollow: PASS (every method scanned has a definition or a non-trivial body)"
        )
        return 0
    virtual = [m for m in scan.hollow if m.virtual]
    print(f"  of which VIRTUAL (a vtable slot with no behaviour): {len(virtual)}")
    for m in scan.hollow[:top]:
        tag = "virtual" if m.virtual else "plain"
        print(f"  {m.kind:10} {tag:7} {m.cls}::{m.name}  declared {m.path}:{m.line}")
    if len(scan.hollow) > top:
        print(f"  ... and {len(scan.hollow) - top} more (--top to see them)")
    return 1


def selftest() -> int:
    """Both verdicts, and the guard skip, on synthetic input.

    A scanner that has only ever found nothing is not a scanner. These controls
    construct the three answers with no filesystem and no compiler: a declared
    method with no definition, a header-defined trivial body, and a declaration
    behind a preprocessor region that must be skipped rather than reported.
    """
    failures: list[str] = []

    # 1. a declaration with no definition is NO BODY
    scan = Scan(classes=1, declared=1)
    scan.hollow = [Method("TFoo", "perform", "include/A.hpp", 10, True, "NO BODY")]
    if "NO BODY" not in scan.summary():
        failures.append("a NO BODY method is missing from the summary")
    if "virtual" not in scan.summary():
        failures.append("a virtual hollow method is not distinguished in the summary")

    # 2. the trivial-body pattern recognises exactly the nothing-bodies
    for line, want in (
        ("\tvirtual void perform() { }", True),
        ("\tvoid reset() {} ", True),
        ("\tf32 getGravityY() const { return 0.0f; }", True),
        ("\tbool checkLiveFlag(u32 f) { return false; }", True),
        ("\tvoid perform(u32 flags, JDrama::TGraphics* gfx)\n\t{", False),
        ("\ts32 count = 0;", False),
        ("\tvoid draw() { GXFlush(); }", False),
    ):
        got = bool(TRIVIAL_BODY.search(line))
        if got != want:
            failures.append(f"trivial-body match wrong for {line!r}: {got} != {want}")

    # 3. a declaration behind a preprocessor region is skipped, not reported
    guarded = [
        "#ifdef VERSION_GMSP01",
        "u16 SMSGetGameVideoHeight(u32 tvFormat);",
        "#endif",
    ]
    unguarded = ["u16 SMSGetGameVideoWidth();"]
    if not guard_state(guarded, 1):
        failures.append("a declaration behind #ifdef was not detected as guarded")
    if guard_state(unguarded, 0):
        failures.append("a plain declaration was wrongly treated as guarded")
    # The bug this control exists for: an #if closed ABOVE the declaration must not
    # mark it guarded. The first version never balanced #endif and reported every
    # declaration in every file as skipped, which suppressed the whole NO BODY pass
    # while still printing a summary.
    reopened = [
        "#ifdef SMS_NATIVE_PLATFORM",
        "int something;",
        "#endif",
        "void afterTheGuard();",
    ]
    if guard_state(reopened, 3):
        failures.append(
            "a declaration after a CLOSED #ifdef was wrongly treated as guarded"
        )
    nested = [
        "#ifdef A",
        "#ifdef B",
        "void insideBoth();",
        "#endif",
        "#endif",
        "void afterBoth();",
    ]
    if not guard_state(nested, 2):
        failures.append("a declaration inside nested #ifdefs was not detected")
    if guard_state(nested, 5):
        failures.append(
            "a declaration after nested #endifs was wrongly treated as guarded"
        )

    # An INCLUDE GUARD is not a configuration region. Getting this wrong marks every
    # declaration in every header as skipped, which suppresses the whole NO BODY
    # pass while the summary still reads like coverage.
    guarded_header = [
        "#ifndef ENEMY_ENEMY_MANAGER_HPP",
        "#define ENEMY_ENEMY_MANAGER_HPP",
        "",
        "class TLiveManager {",
        "\tvirtual void* getObj(int i) { }",
        "};",
        "",
        "#endif",
    ]
    if guard_state(guarded_header, 4):
        failures.append("an include guard was treated as a conditional region")
    if include_guards(guarded_header) != {"ENEMY_ENEMY_MANAGER_HPP"}:
        failures.append(f"include guard not detected: {include_guards(guarded_header)}")
    # A real configuration region inside the same file is still detected.
    mixed = guarded_header + [
        "#ifdef VERSION_GMSP01",
        "void versionSpecific();",
        "#endif",
    ]
    if not guard_state(mixed, 9):
        failures.append(
            "a VERSION_ region inside an include-guarded file was not detected"
        )
    if guard_state(mixed, 5):
        failures.append(
            "a declaration after the include guard was wrongly treated as guarded"
        )

    # 4. the class/method regexes
    if not CLASS_RE.match("class TFoo : public JDrama::TViewObj {"):
        failures.append("a class declaration was not recognised")
    # The shipping rule decides declaration-vs-definition by what follows the
    # parameter list, and it is the only place that knows an in-class body.
    for line, want in (
        ("\tvirtual void perform(u32, JDrama::TGraphics*);", ("perform", "DECLARED")),
        ("\tvoid perform(u32) const;", ("perform", "DECLARED")),
        ("\tvirtual void* getObj(int i) { }", ("getObj", "EMPTY")),
        ("\tvoid reset() {}", ("reset", "EMPTY")),
        (
            "\tinline const T* getOwner() const { return mOwner; }",
            ("getOwner", "DEFINED"),
        ),
        ("\tvoid draw() { GXFlush(); }", ("draw", "DEFINED")),
        ("\tTFoo();", ("TFoo", "DECLARED")),
        ("\tgfx->perform();", None),
        ("\tJDrama::TViewObj* mViewObj;", None),
    ):
        got = classify_member(line)
        got = (got[0], got[1]) if got else None
        if got != want:
            failures.append(f"classify_member({line!r}) = {got}, want {want}")
    if not DEF_RE.match("void TFoo::perform(u32 flags, JDrama::TGraphics* gfx)"):
        failures.append("an out-of-line definition was not recognised")
    if not definition_on_line("TCameraMultiPlayer::removePlayer(const TVec3<f32>* p)"):
        failures.append("a return-type-less definition was not recognised")
    if definition_on_line("void TFoo::bar(u32);"):
        failures.append("a declaration in a .cpp was counted as a definition")
    if not definition_on_line("void TFoo::bar("):
        failures.append("a definition with a wrapped parameter list was not recognised")

    # The three false-positive classes a 40-sample audit found. Each produced a
    # report that was simply wrong, and each is a control now so it cannot return.
    # 1. destructor-name truncation: `virtual ~TFoo() { }` keyed as the CONSTRUCTOR
    #    TFoo, reporting a hole in a constructor that IS defined. 51 of 1575.
    dtor = classify_member("\tvirtual ~TLightWithDBSet() { }")
    if not dtor or dtor[0] != "~TLightWithDBSet":
        failures.append(f"a destructor keyed as {dtor}, want name '~TLightWithDBSet'")
    ctor = classify_member("\tTLightWithDBSet() : mCount(0) { }")
    if not ctor or ctor[0] != "TLightWithDBSet":
        failures.append(f"a constructor keyed as {ctor}, want name 'TLightWithDBSet'")
    if dtor and ctor and dtor[0] == ctor[0]:
        failures.append("a constructor and its destructor collided on one key")

    # 2. an in-class definition whose body starts on the NEXT line. Driven through
    #    `scan_lines`, the function production actually calls, because a control that
    #    called `classify_member` directly passed while the production call site
    #    dropped the lookahead.
    header = [
        "#ifndef T_CTRL_HPP",
        "#define T_CTRL_HPP",
        "class TProbe {",
        "public:",
        "\tvirtual void wrappedReal(u32 a, u32 b)",
        "\t{",
        "\t\tmCount = 0;",
        "\t}",
        "\tvirtual void wrappedEmpty(u32 a)",
        "\t{",
        "\t}",
        "\tvirtual void wrappedDecl(u32 a, u32 b);",
        "\tTProbe() : mCount(0) { }",
        "};",
        "void freeFunction(u32 a);",
        "#endif",
    ]
    probe = Scan()
    scan_lines(header, "TProbe.hpp", probe)
    got = {(m.cls, m.name): m.kind for m in probe.hollow}
    if ("TProbe", "wrappedReal") in got:
        failures.append("a wrapped real in-class body was reported hollow")
    if got.get(("TProbe", "wrappedEmpty")) != "EMPTY BODY":
        failures.append(
            f"a wrapped `{{ }}` read as {got.get(('TProbe', 'wrappedEmpty'))}, want EMPTY BODY"
        )
    if ("TProbe", "wrappedDecl") not in got:
        failures.append("a wrapped declaration was not reported as hollow")
    if got.get(("TProbe", "TProbe")) != "EMPTY BODY":
        failures.append("an in-class ctor with a member init was misread")
    if ("TProbe", "freeFunction") in got:
        failures.append(
            "a FREE function after the class was attributed to the class -- the "
            "class stack is not popping on the class's own closing brace"
        )
    if probe.declared != 1:
        failures.append(
            f"declared count is {probe.declared}, want 1 (only wrappedDecl)"
        )

    # 4. a declaration behind a preprocessor region must be SKIPPED and COUNTED,
    #    driven through `scan_lines`. A control that called `guard_state` directly
    #    passed while the production call site stopped skipping entirely.
    guarded_header = [
        "#ifndef T_GUARD_HPP",
        "#define T_GUARD_HPP",
        "class TGuarded {",
        "public:",
        "\tvirtual void behindAVersionMacro(u32 a);",
        "#ifdef VERSION_GMSP01",
        "\tvirtual void onlyInTheOtherVersion(u32 a);",
        "#endif",
        "\tvirtual void afterTheRegion(u32 a);",
        "};",
        "#endif",
    ]
    g = Scan()
    scan_lines(guarded_header, "TGuarded.hpp", g)
    names = {m.name for m in g.hollow}
    if "onlyInTheOtherVersion" in names:
        failures.append(
            "a declaration behind #ifdef VERSION_GMSP01 was reported -- the decomp "
            "builds GMSJ01, so it is absent from the build and must be skipped"
        )
    if "behindAVersionMacro" not in names:
        failures.append("an ordinary declaration was not reported")
    if "afterTheRegion" not in names:
        failures.append("a declaration after a CLOSED #ifdef was not reported")
    if not any("onlyInTheOtherVersion" in note for note in g.notes):
        failures.append("the skipped declaration was not counted in the denominator")

    # 3. aliasing, driven through `index_definitions`, the function production calls
    #    for the same reason. A control that called `definition_on_line` passed while
    #    the bare-name index was removed from the production path.
    aliased_src = [
        "TBasicWaveBank::TWaveGroup::TWaveGroup()",
        "    : mLoadFlag(0)",
        "{",
        "}",
    ]
    indexed: dict = {}
    index_definitions(aliased_src, "aliased.cpp", indexed)
    if ("TBasicWaveBank::TWaveGroup", "TWaveGroup") not in indexed:
        failures.append("an aliased nested ctor was not indexed under its qualifier")
    if ("TWaveGroup", "TWaveGroup") not in indexed:
        failures.append("an aliased nested ctor was not indexed under its bare name")

    # A PURE VIRTUAL is defined by its subclass and can never be hollow. 74 of them
    # in the J3D Blocks family alone were being reported as holes.
    for line, want in (
        ("\tvirtual void exec(u32 a) = 0;", ("exec", "PURE")),
        ("\tvirtual void exec(u32 a) =0;", ("exec", "PURE")),
        ("\tvirtual void exec(u32 a) = delete;", ("exec", "DECLARED")),
    ):
        got = classify_member(line)
        got = (got[0], got[1]) if got else None
        if got != want:
            failures.append(f"classify_member({line!r}) = {got}, want {want}")

    # Alias matching: a nested class is DECLARED with its full stack path and
    # DEFINED with only the innermost qualifier, which is how dolandecomp spells it.
    aliased_def = ["void JPADrawExecStripeCross::exec(JPADrawExecEmitterVisitor* v)"]
    idx2: dict = {}
    index_definitions(aliased_def, "jpadv.cpp", idx2)
    deep = Scan(classes=1, declared=1)
    deep.hollow = [
        Method("JPADrawExecCallBack::JPADrawExecStripeCross", "exec", "h.hpp", 1, True)
    ]
    classify(deep, idx2, {})
    if deep.hollow:
        failures.append(
            "a nested class defined with only its innermost qualifier was reported hollow"
        )

    # Overloads must not be listed twice.
    dup = Scan(classes=1, declared=2)
    dup.hollow = [
        Method("TFoo", "set", "h.hpp", 1, False),
        Method("TFoo", "set", "h.hpp", 2, False),
    ]
    dedupe(dup)
    if len(dup.hollow) != 1 or dup.overloads != 1:
        failures.append(
            f"dedupe left {len(dup.hollow)} entries, want 1, overloads={dup.overloads}"
        )

    # END TO END over a synthetic corpus, through the same `analyse()` that `main()`
    # calls. Every control above exercises a rule; this one exercises the WIRING, and
    # it is the only control that fails when `main()` stops calling a stage. Four
    # earlier controls were dead for exactly that reason: they passed while the
    # production call site had been sabotaged.
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "include").mkdir()
        (root / "src").mkdir()
        (root / "include" / "E2E.hpp").write_text(
            "#ifndef E2E_HPP\n"
            "#define E2E_HPP\n"
            "class TEndToEnd {\n"
            "public:\n"
            "\tTEndToEnd();\n"
            "\tvirtual void neverDefinedAnywhere(u32 a);\n"
            "\tvirtual void definedInACpp(u32 a);\n"
            "\tvirtual void definedInThisHeader(u32 a) { return; }\n"
            "\tvirtual void emptyVtableSlot(u32 a) { }\n"
            "\tvirtual void pureSlot(u32 a) = 0;\n"
            "};\n"
            "class TOther {\n"
            "public:\n"
            "\tvirtual void alsoNeverDefined(u32 a);\n"
            "\tvirtual const TOther* definedOutOfClass() const;\n"
            "};\n"
            "// Out of class, in the header. Only `find_inline_bodies` knows about this\n"
            "// one, so without it this method is reported hollow.\n"
            "inline const TOther* TOther::definedOutOfClass() const { return this; }\n"
            "#endif\n"
        )
        (root / "src" / "E2E.cpp").write_text(
            "#include <E2E.hpp>\n"
            "TEndToEnd::TEndToEnd()\n"
            "{\n"
            "}\n"
            "void TEndToEnd::definedInACpp(u32 a)\n"
            "{\n"
            "}\n"
        )
        e2e = analyse(root)
        got = {(m.cls, m.name): m.kind for m in e2e.hollow}
        for want, kind in (
            (("TEndToEnd", "neverDefinedAnywhere"), "NO BODY"),
            (("TOther", "alsoNeverDefined"), "NO BODY"),
            (("TEndToEnd", "emptyVtableSlot"), "EMPTY BODY"),
        ):
            if got.get(want) != kind:
                failures.append(f"end-to-end {want} = {got.get(want)}, want {kind}")
        for absent in (
            ("TEndToEnd", "TEndToEnd"),
            ("TEndToEnd", "definedInACpp"),
            ("TEndToEnd", "definedInThisHeader"),
            ("TOther", "definedOutOfClass"),
        ):
            if absent in got:
                failures.append(f"end-to-end: {absent} was reported but IS defined")
        # Five declarations: the ctor, neverDefinedAnywhere, definedInACpp,
        # alsoNeverDefined and definedOutOfClass. The two in-class BODIES are not
        # declarations -- DEFINED and EMPTY are verdicts and must not inflate the
        # denominator, which is what this count pins.
        if e2e.declared != 5:
            failures.append(
                f"end-to-end declared count is {e2e.declared}, want 5 -- an in-class "
                "body is being counted as a declaration"
            )
        if e2e.classes != 2:
            failures.append(f"end-to-end class count is {e2e.classes}, want 2")
        if ("TEndToEnd", "pureSlot") in got:
            failures.append(
                "a pure virtual was reported hollow -- it is by definition "
                "implemented by a subclass and can never be a hole"
            )
        if e2e.pure != 1:
            failures.append(
                f"end-to-end pure-virtual count is {e2e.pure}, want 1 -- the PURE "
                "verdict is not reaching the scan, so it falls through as a finding"
            )

    # 5. an empty scan must refuse rather than pass
    import contextlib
    import io

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = report(Scan(), 5, False)
    if rc == 0 or "REFUSES" not in buf.getvalue():
        failures.append("a scan of zero classes did not refuse")

    for f in failures:
        print(f"  {f}")
    if failures:
        print("hollow_methods selftest: FAIL — the scanner did not discriminate")
        return 1
    print(
        "hollow_methods selftest: PASS (an in-class `{ }` is EMPTY BODY and `{ return m; }` is "
        "DEFINED, a declaration is DECLARED, a call is not a member, a nested definition "
        "matches its nested declaration, an include guard is not a conditional region, "
        "a declared method with no definition is NO BODY, "
        "header-defined `{ }` is EMPTY BODY, a declaration behind a preprocessor region is "
        "skipped rather than reported, and a zero-class scan refuses instead of passing)"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--top", type=int, default=30, help="how many findings to print")
    ap.add_argument("--json", action="store_true", help="machine-readable summary")
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove the scanner produces both verdicts",
    )
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not (SUB / "include").is_dir():
        print(
            f"hollow_methods: REFUSES: {SUB}/include is missing, so nothing is being scanned"
        )
        return 1
    return report(dedupe(analyse(SUB)), max(1, args.top), args.json)


if __name__ == "__main__":
    sys.exit(main())

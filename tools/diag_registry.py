#!/usr/bin/env python3
"""diag_registry.py — the ONE tracked registry of diagnostic switches, and a check that keeps it honest.

WHY THIS EXISTS. The project rule is that diagnostics go through one configurable logger and that
env/flag sprawl is routed through a single tracked registry with dead entries pruned. Neither half
was true: a scan finds 200+ ad-hoc `getenv("SB_...")` gated prints across 80+ files in the decomp,
plus a separate SBR_* family in the recomp, and nothing anywhere lists them. The costs are concrete
and have all been paid already this project:

  · A switch nobody remembers exists is a switch nobody uses — the diagnostic gets rebuilt instead.
  · A switch for a CLOSED investigation is a tombstone: it keeps dead scaffolding alive in code that
    is read constantly, and the rules say to delete such references rather than annotate them.
  · A switch referenced in notes but absent from the code sends a session chasing a phantom. That
    happened here with `SB_SKIP_GHOST`, cited in the project instructions for a while and never
    present in the source at all.

So this tool does not reformat anything. It ANSWERS three questions that currently have no answer:
what diagnostic switches exist, where each is read, and which ones documentation mentions but the
code does not have (or vice versa).

USAGE
  diag_registry.py scan                 # every switch, where it is read, how it is emitted
  diag_registry.py scan --gated-prints  # only the ad-hoc getenv+fprintf sites (the debt to convert)
  diag_registry.py phantoms             # named in docs/instructions but NOT read anywhere in code
  diag_registry.py write                # regenerate docs/diagnostics.md
  diag_registry.py check                # non-zero if docs/diagnostics.md is stale, or a phantom exists

`check` is the part that keeps this from rotting into another stale doc: wire it into pre-commit and
the registry cannot silently drift from the code the way the last one did.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "diagnostics.md")

# Where switches are READ. Anything else is a mention, not a definition.
CODE_DIRS = ["sms-recomp", "sms-boot", "decomp/sms/src", "decomp/sms/include", "extern/aurora/lib"]
# Where switches are TALKED ABOUT. A name here with no reader is a phantom.
DOC_PATHS = ["CLAUDE.md", "docs", "debug_journal", "run.sh", "run-recomp.sh"]

GETENV = re.compile(r'getenv\s*\(\s*"((?:SB|SBR|AURORA)_[A-Z0-9_]+)"')
# Consumed by the lucent logger through its PREFIX mechanism (lucent::config::set_prefix), so they
# never appear as a literal getenv in this tree. Real switches; not phantoms.
LIBRARY_READ = {"SB_LUCENT_DEBUG", "SB_LUCENT_LOG_FILE",
                "SBR_LUCENT_DEBUG", "SBR_LUCENT_LOG_FILE"}
# The project's own logger. A switch reached this way is already converged, not debt.
LOGGER = re.compile(r'(?:SB_LOGC|SB_LOG_ONCE|SB_LOG_EVERY|lucent::(?:debug|info|warn|error))\s*\(')
MENTION = re.compile(r'\b((?:SB|SBR|AURORA)_[A-Z0-9_]{2,})\b')


def tracked_files(dirs):
    """Only files git knows about — build trees and vendored copies are not the source of truth.

    SUBMODULES need their own `git ls-files`: run from the superproject it lists the submodule as a
    single gitlink and reports NOTHING inside it. The first version of this scan did exactly that
    and silently missed ~200 of the sites it exists to find, reporting 62 switches where there are
    far more. A scan that cannot see most of the tree is not a small inaccuracy — it is the
    "no signal and broken instrument look identical" failure, so the walk is per-repo now.
    """
    # Run git with the inherited GIT_* environment STRIPPED. Git sets GIT_DIR and GIT_INDEX_FILE
    # for hooks, and with those set `git ls-files` run inside a submodule resolves against the
    # SUPERPROJECT instead — every reader in decomp/sms and extern/aurora vanishes, and switches
    # read only there are reported as phantoms. The pre-commit hook caught exactly that on its own
    # first commit: real switches (SB_DUMP_FRAME, SB_STAGE, ...) flagged as unread.
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    out = []
    for d in dirs:
        p = os.path.join(ROOT, d)
        if not os.path.exists(p):
            continue
        # Deepest enclosing git repo for this path — the submodule itself when there is one.
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=p if os.path.isdir(p)
                             else os.path.dirname(p), capture_output=True, text=True,
                             env=env).stdout.strip()
        if not top:
            continue
        rel = os.path.relpath(os.path.join(ROOT, d), top)
        r = subprocess.run(["git", "ls-files", "--", rel], cwd=top, capture_output=True, text=True,
                           env=env)
        out += [os.path.join(top, f) for f in r.stdout.split() if f]
    return out


def read_text(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def scan_code():
    """switch -> {'sites': [(relpath, lineno, gated_print)], }"""
    found = {}
    for path in tracked_files(CODE_DIRS):
        if not path.endswith((".c", ".cpp", ".h", ".hpp", ".cc")):
            continue
        text = read_text(path)
        if "getenv" not in text:
            continue
        lines = text.splitlines()
        for i, line in enumerate(lines, 1):
            for m in GETENV.finditer(line):
                name = m.group(1)
                # A gated PRINT is the debt: the switch guards a raw fprintf/printf rather than a
                # call into the project's logger. Look at the switch's own line and the few after
                # it, because the guard and the print are usually not the same line.
                window = "\n".join(lines[i - 1:i + 6])
                # Only a DIAGNOSTIC print counts. `fprintf(f, ...)` to a real FILE is data
                # output — dumping a PPM, writing a capture — and converting it to the logger
                # would be wrong, not overdue. Matching any fprintf counted those too, which is
                # how a "debt" list filled up with a texture dumper and the logger's own env
                # parser. Restricted to the actual gated-diagnostic idiom.
                # An explicit, reviewed exception. Some stderr writes are NOT diagnostics and must
                # not be converted: a hard error on an explicitly-requested feature that failed
                # (pin_state's unreadable SB_PIN_STATE) has to be loud whether or not any log
                # channel is on. Marking them in-code means the scan stops re-proposing the same
                # conversion every time, and the remaining count is work someone should actually do.
                # NOTE the exemption suppresses only the GATED-PRINT classification. `continue`
                # here would skip registering the switch itself, which turned SB_FIFO_REPLAY into
                # a phantom the moment it was exempted — the tool inventing the very defect it
                # exists to find.
                exempt = "LOGGER-EXEMPT" in window
                gated = not exempt and bool(re.search(
                    r'\bfprintf\s*\(\s*std(?:err|out)\b|(?<![\w:])printf\s*\(|'
                    r'(?<![\w:])puts\s*\(|std::cout', window)) and not LOGGER.search(window)
                del exempt
                found.setdefault(name, []).append(
                    (os.path.relpath(path, ROOT), i, gated))
    return found


def macro_names():
    """Names #defined in the code are MACROS, not switches. SB_LOGC and friends are the logger's own
    API and matching them as phantom env vars is the tool inventing work — the same false-positive
    class as counting comments when sizing a symbol."""
    names = set()
    for path in tracked_files(CODE_DIRS):
        if not path.endswith((".h", ".hpp", ".c", ".cpp", ".cc")):
            continue
        for m in re.finditer(r'^\s*#\s*define\s+((?:SB|SBR|AURORA)_[A-Z0-9_]+)', read_text(path),
                             re.M):
            names.add(m.group(1))
    return names


def scan_mentions():
    named = {}
    for d in DOC_PATHS:
        p = os.path.join(ROOT, d)
        if not os.path.exists(p):
            continue
        paths = tracked_files([d]) if os.path.isdir(p) else [p]
        for path in paths:
            if path.endswith((".png", ".rgba", ".bin")):
                continue
            for m in MENTION.finditer(read_text(path)):
                named.setdefault(m.group(1), set()).add(os.path.relpath(path, ROOT))
    return named


def cmd_scan(a):
    code = scan_code()
    if not code:
        print("no diagnostic switches found — check CODE_DIRS")
        return 0
    gated_total = 0
    for name in sorted(code):
        sites = code[name]
        gated = [s for s in sites if s[2]]
        gated_total += len(gated)
        if a.gated_prints and not gated:
            continue
        flag = "  [GATED PRINT]" if gated else ""
        print(f"{name}  ({len(sites)} site(s)){flag}")
        for rel, line, g in sites[:6]:
            print(f"    {rel}:{line}{'  <- raw print' if g else ''}")
        if len(sites) > 6:
            print(f"    ... and {len(sites) - 6} more")
    # Split the debt by OWNERSHIP. A gated print inside decomp/sms or extern/aurora is a VENDORED
    # file: converting it forks us from upstream and buys a merge conflict at every rebase, which
    # the project's UPSTREAM SYNC rule explicitly warns against. Quoting one combined number made
    # the debt look ~10x larger than the work anyone should actually do, so the two are separated
    # and only the first is a to-do.
    ours = vendored = 0
    for sites in code.values():
        for rel, _line, g in sites:
            if not g:
                continue
            if rel.startswith("decomp/") or rel.startswith("extern/"):
                vendored += 1
            else:
                ours += 1
    print(f"\n{len(code)} switches, {sum(len(v) for v in code.values())} read sites, "
          f"{gated_total} ad-hoc gated prints:")
    print(f"  {ours} in code we OWN (sms-boot/, sms-recomp/, tools/) — the real debt to convert "
          f"to the logger")
    print(f"  {vendored} in VENDORED trees (decomp/sms, extern/aurora) — NOT a to-do: converting "
          f"them forks upstream and costs a conflict at every rebase")
    return 0


# A dated journal entry is a HISTORICAL record: it is supposed to describe switches that existed at
# the time, and a switch retired since is not a defect there. A LIVE instruction is different — a
# name in CLAUDE.md, docs/ or a run script tells the reader the switch works TODAY. Only the second
# kind is actionable, and conflating them buries the handful that matter under hundreds that do not.
LIVE_DOCS = ("CLAUDE.md", "run.sh", "run-recomp.sh", "run-render.sh")

# RECORDS, not live instructions. A dated journal entry, an RE note, a claim in the ledger (often
# FALSIFIED, where naming the switch is the point) and a do-not-revisit marker all describe what was
# true then, or deliberately name retired machinery so it is not resurrected. Flagging them produces
# a list nobody can act on, which is how a checker gets ignored.
HISTORICAL_PREFIXES = ("debug_journal/", "docs/re_notes/", "docs/info/claims/")


def _is_live_doc(rel):
    if rel.startswith(HISTORICAL_PREFIXES):
        return False
    if os.path.basename(rel).startswith("DO_NOT_REVISIT"):
        return False
    return os.path.basename(rel) in LIVE_DOCS or rel.startswith("docs/")


def cmd_phantoms(a):
    """Named in docs but read NOWHERE. This is the failure that costs a whole session."""
    code = scan_code()
    mentions = scan_mentions()
    macros = macro_names()
    phantoms = {n: s for n, s in mentions.items()
                if n not in code and n not in macros and n not in LIBRARY_READ
                and not n.endswith("_")}
    # A name only ever mentioned in its own registry entry is not evidence of anything.
    phantoms = {n: {p for p in s if os.path.basename(p) != "diagnostics.md"}
                for n, s in phantoms.items()}
    phantoms = {n: s for n, s in phantoms.items() if s}
    live = {n: s for n, s in phantoms.items() if any(_is_live_doc(p) for p in s)}
    historical = len(phantoms) - len(live)
    if not live:
        print(f"no LIVE phantoms: every switch named in CLAUDE.md, docs/ or a run script is "
              f"actually read by the code.")
        if historical:
            print(f"({historical} name(s) appear only in dated debug_journal entries — history "
                  f"describing switches that existed then. Not actionable.)")
        return 0
    print("PHANTOM SWITCHES — named in a LIVE instruction, read by NO code:\n")
    for n in sorted(live):
        print(f"  {n}")
        for p in sorted(x for x in live[n] if _is_live_doc(x))[:4]:
            print(f"      named in {p}")
    print(f"\n{len(live)} live phantom(s). Either the switch was deleted and the mention is a "
          f"tombstone, or it was never implemented. Both send a session chasing nothing.")
    if historical:
        print(f"({historical} further name(s) appear only in dated journal entries — history, "
              f"not actionable.)")
    return 1


def render():
    code = scan_code()
    lines = ["# Diagnostic switches — generated by `tools/diag_registry.py write`",
             "",
             "Every environment switch the code actually READS, and where. Regenerate rather than",
             "hand-edit; `tools/diag_registry.py check` fails if this drifts from the source.",
             "",
             "`[gated print]` marks a switch still guarding a raw `fprintf` instead of the project's",
             "logger — that is debt to convert, not a feature.",
             ""]
    by_prefix = {}
    for name, sites in code.items():
        by_prefix.setdefault(name.split("_")[0], {})[name] = sites
    for prefix in sorted(by_prefix):
        # FILE, NOT FILE:LINE. The line number went stale the moment anything was inserted above
        # the getenv, so every unrelated edit anywhere in a file rewrote this table and failed the
        # commit gate until it was regenerated — a recurring tax that bought nothing, since a reader
        # greps the switch name anyway. The file still narrows it to one place.
        lines += [f"## `{prefix}_*`", "", "| switch | sites | first read in | |", "|---|---|---|---|"]
        for name in sorted(by_prefix[prefix]):
            sites = by_prefix[prefix][name]
            gated = "gated print" if any(s[2] for s in sites) else ""
            lines.append(f"| `{name}` | {len(sites)} | `{sites[0][0]}` | {gated} |")
        lines.append("")
    total_gated = sum(1 for v in code.values() if any(s[2] for s in v))
    lines += [f"**{len(code)} switches; {total_gated} still gate a raw print.**", ""]
    return "\n".join(lines)


def cmd_write(a):
    os.makedirs(os.path.dirname(DOC), exist_ok=True)
    with open(DOC, "w") as f:
        f.write(render())
    print(f"wrote {os.path.relpath(DOC, ROOT)}")
    return 0


def cmd_check(a):
    rc = 0
    if not os.path.exists(DOC):
        print(f"MISSING {os.path.relpath(DOC, ROOT)} — run `diag_registry.py write`")
        rc = 1
    elif read_text(DOC) != render():
        print(f"STALE {os.path.relpath(DOC, ROOT)} — switches changed; run `diag_registry.py write`")
        rc = 1
    if cmd_phantoms(a) != 0:
        rc = 1
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan"); s.add_argument("--gated-prints", action="store_true"); s.set_defaults(fn=cmd_scan)
    s = sub.add_parser("phantoms"); s.set_defaults(fn=cmd_phantoms)
    s = sub.add_parser("write"); s.set_defaults(fn=cmd_write)
    s = sub.add_parser("check"); s.set_defaults(fn=cmd_check)
    a = ap.parse_args()
    sys.exit(a.fn(a))


if __name__ == "__main__":
    main()

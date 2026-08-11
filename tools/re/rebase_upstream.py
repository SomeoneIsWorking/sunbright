#!/usr/bin/env python3
"""rebase_upstream.py — run and audit the twice-weekly rebase of our native port
onto upstream `doldecomp/sms`.

Upstream moves fast (we sync ~2x/week). A rebase is only cheap if our divergence
from upstream is small and *deliberate*. This tool does three things:

    status     how far behind upstream we are + how far our tree has diverged
    rebase     perform the rebase with the strategy that actually works here
    diverge    list diverging files, split into "native-guarded" (legit) vs
               "unmarked" (free-convergence candidates — shrink these!)
    converge   greedily adopt upstream's version of candidate files, building
               after each batch and keeping only what stays green

Background (learned the hard way, 2026-07-17 — see
debug_journal/2026-07-17_upstream_rebase_and_delfino_crash.md):

  * A plain `git rebase -X theirs upstream/main` REPLAYS fine but leaves our and
    upstream's edits interleaved hunk-by-hunk in files both sides touched. That
    produces internally-inconsistent TUs in two recognizable classes:
      1. DUPLICATE DECLS — both sides added the same member to a class at
         different line positions, so git merges BOTH (no textual conflict) and
         the class ends up declaring e.g. load()/perform() twice.
      2. API DRIFT — upstream renamed something (getUnkB4->getViewMtx,
         TLookAtCamera 2-arg->6-arg, TMapCollisionBase unk10->unk20, dropped a
         TOrthoProj overload) while our .cpp still calls the old form.
  * Resolution must be FILE-LEVEL, never hunk-level, and header+cpp must move
    together — a class whose .hpp and .cpp come from different sides will not
    build.
  * Prefer CONVERGING to upstream. Every file we keep our own version of is a
    conflict we pay for again at the next sync. Keep native deltas minimal,
    guarded (`#ifdef SMS_NATIVE_PLATFORM`), and additive so git can auto-merge.

Zero dependencies; safe by default (never force-pushes, never rewrites without
an explicit subcommand).
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SUBMODULE = os.path.abspath(os.path.join(HERE, "..", "..", "decomp", "sms"))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# Markers that indicate a file legitimately carries native-port changes.
NATIVE_MARKERS = (
    "SMS_NATIVE_PLATFORM",
    "SMS_AURORA",
    "sb_log",
    "sb_be",
    "sb_host_alloc",
    "uintptr_t",
    "STOPGAP",
)

SRC_DIRS = ["src", "include"]


def git(*args, cwd=SUBMODULE, check=True):
    r = subprocess.run(
        ["git"] + list(args), cwd=cwd, capture_output=True, text=True
    )
    if check and r.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{r.stderr.strip()}")
    return r.stdout.strip()


def current_branch():
    """Our branch in the submodule, read rather than assumed.

    This was hardcoded to "sunbright" and the branch is now `main` (one branch per repo), so every
    subcommand died on `Not a valid object name sunbright` — a tool that cannot run is a workflow
    defect, not a fact about the rebase. Detached HEAD REFUSES: rebasing a detached head would drop
    our commits with nothing pointing at them.
    """
    b = git("rev-parse", "--abbrev-ref", "HEAD")
    if b == "HEAD":
        sys.exit(f"decomp/sms is on a DETACHED HEAD ({git('rev-parse','--short','HEAD')}). "
                 "Check out the port branch first — rebasing detached loses the commits.")
    return b


TARGET = "sms-boot"


def build_dir():
    """The build directory that actually HAS the decomp target — checked, not assumed.

    This is the instrument's control, and it exists because the instrument lied. The tool assumed
    a `build/` directory; the repo has `build-sms-recomp/` (the RECOMP runtime), which does not
    define `sms-boot` at all. `cmake --build build-sms-recomp --target sms-boot` prints NOTHING and
    exits 0 — so `audit` reported "build GREEN — no post-rebase reconciliation needed" from a build
    that never compiled one file of the tree it was certifying. A green that cannot go red is not
    evidence, and this one certified a 396-commit rebase.

    So: a candidate directory must be configured AND list the target. If none does, REFUSE — an
    unbuildable rebase must read as "not verified", never as "verified fine".
    """
    tried = []
    for d in ("build", "build-sms-recomp"):
        p = os.path.join(REPO, d)
        if not os.path.isdir(os.path.join(p, "CMakeFiles")):
            tried.append(f"{d}: not configured")
            continue
        r = subprocess.run(["cmake", "--build", d, "--target", "help"],
                           cwd=REPO, capture_output=True, text=True)
        if any(line.strip().endswith(f" {TARGET}") or line.strip() == TARGET
               for line in r.stdout.splitlines()):
            return d
        tried.append(f"{d}: configured but defines no '{TARGET}' target")
    sys.exit(f"REFUSES: no build directory defines the '{TARGET}' target ({'; '.join(tried)}), so "
             f"NOTHING can be compiled and this says NOTHING about the rebase. Configure the decomp "
             f"build first:  cmake -B build -DCMAKE_BUILD_TYPE=Release")


def ensure_clean():
    if git("status", "--porcelain"):
        sys.exit(
            "decomp/sms working tree is dirty. Commit or stash first — a rebase "
            "on a dirty tree loses work."
        )


def diverging_files():
    out = git("diff", "--name-only", "upstream/main", "--", *SRC_DIRS)
    return [f for f in out.splitlines() if f.strip()]


def exists_upstream(path):
    """True if upstream/main has this path at all.

    Files we ADDED (our own ports, e.g. include/Animal/Bird.hpp) show up in the
    divergence diff but are not convergence candidates — `git checkout
    upstream/main -- <them>` fails and aborts the whole batch.
    """
    return subprocess.run(
        ["git", "cat-file", "-e", f"upstream/main:{path}"],
        cwd=SUBMODULE, capture_output=True,
    ).returncode == 0


def classify(files):
    """Split into (guarded, unmarked). Unmarked files are convergence candidates."""
    guarded, unmarked = [], []
    for f in files:
        p = os.path.join(SUBMODULE, f)
        try:
            with open(p, "r", encoding="utf-8", errors="ignore") as fh:
                body = fh.read()
        except OSError:
            unmarked.append(f)
            continue
        (guarded if any(m in body for m in NATIVE_MARKERS) else unmarked).append(f)
    return guarded, unmarked


def build(jobs=None):
    """Build the decomp target. Returns (ok, error_count).

    An empty build log is treated as a FAILURE to observe rather than a pass: cmake prints
    nothing and exits 0 for a target it does not have, which is how this tool certified a rebase it
    never compiled. build_dir() now rules that case out up front; this is the second line of
    defence, because "no output" and "everything was already up to date" are also indistinguishable
    after a rebase that touched source files.
    """
    jobs = jobs or str(os.cpu_count() or 4)
    r = subprocess.run(
        ["cmake", "--build", build_dir(), "--target", TARGET, "-j", jobs],
        cwd=REPO, capture_output=True, text=True,
    )
    out = r.stdout + r.stderr
    if not out.strip():
        sys.exit(f"REFUSES: building '{TARGET}' produced NO output at all, so nothing was "
                 f"compiled and the result is not a verdict on the tree.")
    errs = [l for l in out.splitlines() if "error:" in l]
    return r.returncode == 0, len(errs)


def cmd_status(args):
    git("fetch", "upstream", "--quiet", check=False)
    branch = current_branch()
    base = git("merge-base", branch, "upstream/main")
    behind = git("rev-list", "--count", f"{base}..upstream/main")
    ours = git("rev-list", "--count", f"{base}..{branch}")
    on_top = "YES" if subprocess.run(
        ["git", "merge-base", "--is-ancestor", "upstream/main", branch],
        cwd=SUBMODULE).returncode == 0 else "NO"

    files = diverging_files()
    guarded, unmarked = classify(files)

    print(f"fork-point            : {base[:12]} {git('log','--oneline','-1',base)[9:]}")
    print(f"upstream commits ahead: {behind}")
    print(f"our commits on top    : {ours}")
    print(f"already rebased onto upstream? {on_top}")
    print()
    print(f"diverging files       : {len(files)}")
    print(f"  native-guarded      : {len(guarded)}  (legit — our port needs these)")
    print(f"  unmarked            : {len(unmarked)}  <-- convergence candidates")
    print()
    if int(behind) > 0:
        print(f"=> {behind} upstream commits to pick up. Run: rebase_upstream.py rebase")
    else:
        print("=> up to date with upstream.")
    if unmarked:
        print(f"=> shrink divergence: rebase_upstream.py converge --limit N")


def cmd_diverge(args):
    files = diverging_files()
    guarded, unmarked = classify(files)
    which = {"guarded": guarded, "unmarked": unmarked, "all": files}[args.kind]
    for f in which:
        stat = git("diff", "--numstat", "upstream/main", "--", f).split("\t")
        churn = f"+{stat[0]} -{stat[1]}" if len(stat) >= 2 else "?"
        print(f"{churn:>14}  {f}")
    print(f"\n{len(which)} file(s) [{args.kind}]", file=sys.stderr)


def cmd_rebase(args):
    ensure_clean()
    git("fetch", "upstream", "--quiet", check=False)
    branch = current_branch()
    base = git("merge-base", branch, "upstream/main")
    behind = int(git("rev-list", "--count", f"{base}..upstream/main"))
    if behind == 0:
        print("already up to date with upstream — nothing to rebase.")
        return
    tag = f"pre-rebase-backup-{args.tag}" if args.tag else "pre-rebase-backup"
    git("tag", "-f", tag, branch)
    print(f"[safety] tagged current tip as {tag}")
    print(f"[rebase] replaying our commits onto upstream/main ({behind} new upstream commits)")
    r = subprocess.run(
        ["git", "rebase", "-X", "theirs", "upstream/main"],
        cwd=SUBMODULE, capture_output=True, text=True,
    )
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit(
            "rebase stopped. Resolve, `git rebase --continue`, then re-run "
            "`rebase_upstream.py audit`."
        )
    print("[rebase] replay OK. NOW AUDIT — the tree is very likely inconsistent.")
    print("         run: rebase_upstream.py audit")


def cmd_audit(args):
    """Build and classify the failures into the two known post-rebase classes."""
    ok, n = build()
    if ok:
        print("build GREEN — no post-rebase reconciliation needed.")
        return
    r = subprocess.run(
        ["cmake", "--build", build_dir(), "--target", TARGET, "-j",
         str(os.cpu_count() or 4)],
        cwd=REPO, capture_output=True, text=True,
    )
    errs = sorted({l for l in (r.stdout + r.stderr).splitlines() if "error:" in l})
    dup = [e for e in errs if "cannot be overloaded" in e]
    drift = [e for e in errs if "cannot be overloaded" not in e]
    print(f"build RED: {len(errs)} unique errors\n")
    if dup:
        print(f"-- CLASS 1: duplicate decls ({len(dup)}) — both sides added the same")
        print("   member. Fix: delete OUR duplicate, keep upstream's canonical form.")
        for e in dup[:15]:
            print("   " + e.strip()[:150])
    if drift:
        print(f"\n-- CLASS 2: API drift / inconsistency ({len(drift)}) — our .cpp vs")
        print("   upstream's header. Fix FILE-LEVEL, header+cpp together.")
        for e in drift[:25]:
            print("   " + e.strip()[:150])


def cmd_converge(args):
    """Greedily adopt upstream's version of unmarked files; keep only if green."""
    ensure_clean()
    files = diverging_files()
    _, unmarked = classify(files)
    ours_only = [f for f in unmarked if not exists_upstream(f)]
    unmarked = [f for f in unmarked if f not in ours_only]
    if ours_only:
        print(f"[converge] skipping {len(ours_only)} file(s) upstream does not have "
              f"(our own additions, nothing to converge to)")
    if args.limit:
        unmarked = unmarked[: args.limit]
    if not unmarked:
        print("no convergence candidates.")
        return
    ok, _ = build()
    if not ok:
        sys.exit("build is RED before converging — fix that first.")
    print(f"[converge] {len(unmarked)} candidate(s), batch size {args.batch}")
    adopted, reverted = [], []
    for i in range(0, len(unmarked), args.batch):
        batch = unmarked[i : i + args.batch]
        git("checkout", "upstream/main", "--", *batch)
        ok, n = build()
        if ok:
            adopted += batch
            print(f"  [+] adopted {len(batch)} (total {len(adopted)})")
        else:
            git("checkout", "HEAD", "--", *batch)
            reverted += batch
            print(f"  [-] reverted {len(batch)} ({n} errors) — keeps our version")
    print(f"\nadopted {len(adopted)} file(s); kept ours for {len(reverted)}")
    print("REVIEW THE DIFF AND RUNTIME-VERIFY before committing: a file can build")
    print("fine yet drop a native LP64/BE fix that only shows up at runtime.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="how far behind upstream + divergence summary"
                   ).set_defaults(func=cmd_status)

    d = sub.add_parser("diverge", help="list diverging files by class")
    d.add_argument("kind", nargs="?", default="unmarked",
                   choices=["guarded", "unmarked", "all"])
    d.set_defaults(func=cmd_diverge)

    rb = sub.add_parser("rebase", help="tag a backup and replay onto upstream")
    rb.add_argument("--tag", default="", help="suffix for the safety tag")
    rb.set_defaults(func=cmd_rebase)

    sub.add_parser("audit", help="build and classify post-rebase breakage"
                   ).set_defaults(func=cmd_audit)

    cv = sub.add_parser("converge", help="adopt upstream's version where it stays green")
    cv.add_argument("--limit", type=int, default=0, help="max candidates to try")
    cv.add_argument("--batch", type=int, default=10, help="files per build")
    cv.set_defaults(func=cmd_converge)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

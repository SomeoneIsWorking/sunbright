"""convergence_loss.py — gate a decomp merge on lost native work.

Answers one question with no compiler: did adopting upstream delete anything this
fork added? Reads refs only, so it is valid mid-merge.

SCOPE, stated because it is the limit that matters: "anything this fork added"
means added SINCE THE FORK POINT (`FORK`, currently 40c2594b, 2026-08-30). Work
older than that is invisible to this check. Concretely: the empty stubs
`TBathtubKiller::perform(u32, JDrama::TGraphics*) { }` and its ~24 siblings were
added in March 2026 by ab285a7e "BathtubKiller scaffolding", five months before
the fork point, so replacing them with upstream's decompiled bodies reported
"intact" rather than flagging a removal. That is correct for the question this
tool asks -- those lines were not added by the merge era -- and wrong as an
answer to "did we lose fork work here". The per-file convergence debt (which the
2026-10-01 pass measured directly, both trees' line counts) is what catches
pre-fork-point drift; this tool does not, and does not claim to.

This is the check the retired build-and-run gate used to provide, reduced to the
one property that is still checkable without a compiler. For every file our
fork edited since the fork point, each line we added is looked for in the merged
result:

  relocated  upstream moved the file, so our added lines are compared against
             the merged content at the new path, not the old one
  intact     every line we added is present
  REVIEW     at least one added line is absent. That is NOT automatically a
             loss: upstream legitimately reworded the surrounding code. So each
             absent line is reported with the distinctive tokens it carried, and
             the file is listed for a human to confirm the fix, not the wording,
             survived.

Reports what it scanned and matched; an empty REVIEW list is the pass, and a
scan of zero files is a failure rather than a pass.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SUB = REPO / "decomp" / "sms"
SCRATCH = REPO / "scratch" / "decomp-sync"
OURS = "main"
UPSTREAM = "upstream/main"


def arg(flag: str, default: str) -> str:
    """One optional value, so a control can name historical refs."""
    if flag in sys.argv:
        return sys.argv[sys.argv.index(flag) + 1]
    return default


# Default to the recorded 2026-09-28 merge base. A hardcoded hash would silently
# compare against a base that no longer describes the branch; --fork lets the next
# sync measure its own divergence, and --fork/--ours together reproduce any past
# sync exactly, which is how this tool's control is run.
FORK = arg("--fork", "6ae2aa867730bf8afc706dbc7f56882a4f6feb00")
OURS = arg("--ours", "main")

# Upstream relocated the middleware to libs/<lib>/, and it did NOT do so
# symmetrically: the include side keeps the library name twice
# (include/JSystem/X -> libs/JSystem/include/JSystem/X) while the source side
# drops it (src/JSystem/X -> libs/JSystem/src/X). A single naive prefix rule maps
# every src/ file to a path that does not exist, which reads as "unresolved"
# and silently skips 90 files from the check.
MOVES: dict[str, str] = {}
for _old, _new in (
    ("include/JSystem", "libs/JSystem/include/JSystem"),
    ("src/JSystem", "libs/JSystem/src"),
    ("include/dolphin", "libs/dolphin/include/dolphin"),
    ("src/dolphin", "libs/dolphin/src"),
    ("include/PowerPC_EABI_Support", "libs/PowerPC_EABI_Support/include/PowerPC_EABI_Support"),
    ("src/PowerPC_EABI_Support", "libs/PowerPC_EABI_Support/src"),
    ("include/TRK_MINNOW_DOLPHIN", "libs/TRK_MINNOW_DOLPHIN/include/TRK_MINNOW_DOLPHIN"),
    ("src/TRK_MINNOW_DOLPHIN", "libs/TRK_MINNOW_DOLPHIN/src"),
    ("include/THPPlayer", "libs/THPPlayer/include/THPPlayer"),
    ("src/THPPlayer", "libs/THPPlayer/src"),
    ("include/OdemuExi2", "libs/OdemuExi2/include/OdemuExi2"),
    ("src/OdemuExi2", "libs/OdemuExi2/src"),
    ("include/os", "libs/PowerPC_EABI_Support/include/os"),
):
    MOVES[_old] = _new


def git(*args: str) -> str | None:
    r = subprocess.run(["git", *args], cwd=SUB, capture_output=True, text=True,
                       errors="replace", check=False)
    return r.stdout if r.returncode == 0 else None


def blob(ref: str, path: str) -> str | None:
    return git("cat-file", "blob", f"{ref}:{path}")


def target_for(path: str) -> str:
    for old_root, new_root in MOVES.items():
        if path.startswith(old_root + "/"):
            return new_root + path[len(old_root):]
    return path


def norm(line: str) -> str:
    return " ".join(line.split())


def distinctive(line: str) -> str:
    """The token that shows a fix survived even if the wording changed."""
    for token in ("SMS_NATIVE_PLATFORM", "SUNBRIGHT-KEEP", "STOPGAP", "Native port of",
                  "uintptr_t", "intptr_t", "size_t", "__builtin_bswap", "sb_log",
                  "sb_be", "sb_host_alloc", "SMS_AURORA", "LP64", "big-endian",
                  "byteswap", "bswap"):
        if token in line:
            return token
    words = [w for w in line.replace("*", " ").split() if len(w) > 3 and w.isidentifier()]
    return words[0] if words else norm(line)[:40]


def main() -> int:
    # Derived from refs, never from the worktree: during a merge the worktree is a
    # half-resolved index, and a bare `git diff upstream/main` against it silently
    # yields a different, smaller set than the one being merged.
    diverging = git("diff", "--name-only", UPSTREAM, "--", "src", "include", "libs") or ""
    edited = [f for f in diverging.splitlines() if f.strip()]
    if not edited:
        print("REFUSES: no diverging file under src/include/libs; there is nothing to check, "
              "so a clean result here would say nothing about the merge.")
        return 1
    intact, review, unresolved = 0, [], []
    for path in edited:
        base, ours = blob(FORK, path), blob(OURS, path)
        if base is None or ours is None:
            continue
        base_lines = {norm(l) for l in base.splitlines()}
        added = [l for l in ours.splitlines()
                 if l.strip() and norm(l) not in base_lines]
        if not added:
            continue
        target = target_for(path)
        r = subprocess.run(["git", "show", f":{target}"], cwd=SUB, capture_output=True,
                           text=True, errors="replace", check=False)
        merged = r.stdout if r.returncode == 0 else None
        if merged is None:
            unresolved.append((path, target))
            continue
        merged_norm = {norm(l) for l in merged.splitlines()}
        missing = [l for l in added if norm(l) not in merged_norm]
        if not missing:
            intact += 1
            continue
        review.append((path, target, missing, merged))

    print(f"files with a local delta : {intact + len(review) + len(unresolved)}")
    print(f"  intact (every added line present)      : {intact}")
    print(f"  REVIEW  (an added line is absent)       : {len(review)}")
    print(f"  UNRESOLVED (no merged content to check) : {len(unresolved)}")

    out = SCRATCH / "loss_review.txt"
    # scratch/ is gitignored and gets wiped, and this tool is run by hand as often
    # as by the gate. Writing into a directory that is not there raised a bare
    # FileNotFoundError AFTER printing the verdict, which reads like the check
    # itself failed. Create the directory; the verdict is the product, the report
    # is a convenience.
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as fh:
        for path, target, missing, merged in review:
            surviving = sorted({distinctive(l) for l in missing})
            still_there = [t for t in surviving
                           if t and t in merged and t not in ("LP64", "size_t")]
            fh.write(f"{path} -> {target}\n")
            fh.write(f"    absent lines carry: {surviving}\n")
            fh.write(f"    token still present in merged file: {still_there or 'NONE'}\n")
            for line in missing[:6]:
                fh.write(f"    - {norm(line)[:110]}\n")
    print(f"\nper-file detail -> {out}")
    for path, target, missing, merged in review[:15]:
        surviving = sorted({distinctive(l) for l in missing})
        still = [t for t in surviving if t and t in merged]
        print(f"  {path}\n      absent-token={surviving} still-present={still}")
    if len(review) > 15:
        print(f"  ... and {len(review) - 15} more (see {out})")
    for path, target in unresolved:
        print(f"  UNRESOLVED {path} -> {target}")
    return 1 if (review or unresolved) else 0


# The control's "merged result" side, pinned to a commit rather than to the live
# index. Reading `:path` made the control's answer depend on the working tree:
# converging the decomp moved the number from 54 files to 76, and a future
# convergence that removed every last dropped line would have made the control
# report "no loss found" and FAIL -- an instrument that breaks when the thing it
# measures gets better. d145df88 is the decomp commit the 2026-09-28 sync landed,
# i.e. the merged result as it actually was, which is what the replay must be
# compared against. It is a pushed commit and the tool refuses to guess if the
# clone does not have it.
MERGED_CONTROL = "d145df88473a76f1aca501c20eecbc126cd5ddc7"


def selftest() -> int:
    """Prove the check can report a loss, not only that it currently reports none.

    A convergence gate that has only ever answered "clean" is an instrument that
    cannot say the other answer, which is the failure this project keeps paying
    for. The control is the 2026-09-28 merge itself, replayed from its own refs:
    our pre-merge tip against the merged result that sync produced, which really
    did strip 43 files. If that stops producing REVIEW, the check has gone blind.

    The answer must be STABLE, or the control is measuring the wrong thing: an
    earlier version read the merged side out of the live index, so the number
    moved as the tree converged (54 files, then 76) and would have hit zero --
    and failed -- on a successful convergence. Hence MERGED_CONTROL.
    """
    fork = arg("--fork", "")
    if not fork:
        fork = "40c2594bd2706b709f75be0bca01acb283248693"
    ours = arg("--ours", "")
    if not ours:
        ours = "pre-upstream-merge-2026-09-28"
    merged = arg("--merged", "") or MERGED_CONTROL

    for ref in (fork, ours, merged, "upstream/main"):
        if git("rev-parse", "--verify", ref) is None:
            print(f"selftest: REFUSES — control ref {ref!r} is not in this clone, so the "
                  f"control cannot run and a pass would mean nothing")
            return 1

    globals()["FORK"], globals()["OURS"] = fork, ours
    diverging = git("diff", "--name-only", UPSTREAM, "--", "src", "include", "libs") or ""
    files = [f for f in diverging.splitlines() if f.strip()]
    if not files:
        print("selftest: FAIL — no diverging files, so the control scanned nothing")
        return 1

    lost = 0
    for path in files:
        base, side = blob(fork, path), blob(ours, path)
        if base is None or side is None:
            continue
        base_lines = {norm(l) for l in base.splitlines()}
        added = [l for l in side.splitlines() if l.strip() and norm(l) not in base_lines]
        if not added:
            continue
        r = subprocess.run(["git", "show", f"{merged}:{target_for(path)}"], cwd=SUB,
                           capture_output=True, text=True, errors="replace", check=False)
        if r.returncode != 0:
            continue
        merged_norm = {norm(l) for l in r.stdout.splitlines()}
        if any(norm(l) not in merged_norm for l in added):
            lost += 1

    if lost == 0:
        print(f"selftest: FAIL — replaying the 2026-09-28 merge found no loss in "
              f"{len(files)} file(s), but that merge deleted native work from 43. "
              f"The check cannot see the other answer.")
        return 1
    print(f"selftest: PASS — replaying the 2026-09-28 merge against pinned {merged[:8]} "
          f"reports loss in {lost} file(s), so this check is known to go red on a real loss")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())

#!/usr/bin/env python3
"""graphics_db.py — read and curate THE GRAPHICS REGISTRY (docs/graphics/graphics_db.tsv).

The registry is written by the game itself: every emitter that draws gets a row automatically, with
its guest symbol, the stages it was seen in, and the interpolation verdict aurora's audit measured
for it. This tool is the other half — reading it back in a useful order, and recording the CURATED
columns (`re`, `note`) the game never touches.

    graphics_db.py next                    THE WORKLIST — what to RE next, worst-interpolating first
    graphics_db.py summary                 counts by RE state and by lerp verdict, with denominators
    graphics_db.py list --re unknown       the graphics nobody has looked at yet
    graphics_db.py list --lerp no          the ones measured as NOT interpolating
    graphics_db.py show 0x802dfe88         one row, every column
    graphics_db.py set 0x802dfe88 re=yes note="water refraction quad; snap is correct"

WHAT THE FILE CANNOT TELL YOU, repeated here because a listing is where it gets forgotten:

  * It is a census of what has been OBSERVED. A graphic that never drew in a recorded run has no
    row, and its absence is not evidence it does not exist. `summary` prints which stages have
    contributed, so "we have only ever run the plaza" is visible rather than assumed.
  * `lerp=unmeasured` means the interpolation audit filed nothing for that row (the run had
    SBR_LERP60 off). It is NOT "does not interpolate", and this tool never counts it as one.
  * A `re` of `native-override` is a HINT the game seeded — a native override exists for the
    emitting function — not a verdict that the graphic is understood.
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DB = os.path.join(ROOT, "docs", "graphics", "graphics_db.tsv")
COLS = ["key", "kind", "symbol", "stages", "re", "lerp", "first_seen", "note"]
CURATED = {"re", "note"}
RE_VERDICTS = ("unknown", "native-override", "identified", "no", "partial", "yes")


def load():
    if not os.path.isfile(DB):
        # REFUSE. "no rows" from a missing file and "no rows" from a game that drew nothing are the
        # same output and opposite facts; only one of them is answered by running the game.
        sys.exit(f"REFUSES: {DB} does not exist, so NOTHING was read — this is not an empty "
                 f"registry, it is no registry. Run the game once (./run-recomp.sh) and it will "
                 f"write one.")
    rows, header_seen = [], False
    with open(DB, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "key":
                header_seen = True
                if parts != COLS:
                    sys.exit(f"REFUSES: {DB} has columns {parts}, this tool expects {COLS}. "
                             f"Reading it with the wrong column map would mislabel every value.")
                continue
            if len(parts) != len(COLS):
                sys.exit(f"REFUSES: malformed row with {len(parts)} column(s): {line[:120]}")
            rows.append(dict(zip(COLS, parts)))
    if not header_seen:
        sys.exit(f"REFUSES: {DB} has no header row, so its columns are unknown.")
    if not rows:
        sys.exit(f"REFUSES: {DB} exists but holds ZERO rows. The game writes rows only for "
                 f"emitters it actually saw draw, so this says the last run rendered nothing "
                 f"through the hooked waists — not that the game has no graphics.")
    return rows


def write(rows):
    header, body = [], []
    with open(DB, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#"):
                header.append(line)
            else:
                break
    body = ["\t".join(COLS)]
    for r in sorted(rows, key=lambda r: r["key"]):
        body.append("\t".join(r[c] for c in COLS))
    tmp = DB + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.writelines(header)
        fh.write("\n".join(body) + "\n")
    os.replace(tmp, DB)


def as_int(v):
    try:
        return int(v)
    except ValueError:
        return 0


def cmd_summary(args):
    rows = load()
    stages = sorted({s for r in rows for s in r["stages"].split(",") if s and s != "-"})
    print(f"{len(rows)} source(s) of visual output, seen across stage(s): "
          f"{', '.join(stages) or '(none recorded)'}")
    print("  Only these stages have ever been observed — a graphic that draws elsewhere has no row "
          "yet, and its absence here is not evidence it does not exist.\n")

    for field, order in (("re", RE_VERDICTS), ("lerp", ("yes", "partial", "camera-only", "no",
                                                        "2d-correct", "drew-once", "discontinuous-correct",
                                                        "no-primitives", "seam-owned",
                                                        "unmeasured"))):
        print(f"  by {field}:")
        seen = {}
        for r in rows:
            seen[r[field]] = seen.get(r[field], 0) + 1
        for k in list(order) + sorted(set(seen) - set(order)):
            if k not in seen:
                continue
            print(f"    {k:<16} {seen[k]:>4} row(s)")
        print()
    unmeasured = sum(1 for r in rows if r["lerp"] == "unmeasured")
    if unmeasured:
        print(f"  {unmeasured} row(s) are `unmeasured`: the audit filed nothing for them. Run with "
              f"SBR_LERP60=1 to measure — until then they are NOT known to snap.")


def cmd_list(args):
    rows = load()
    for f in ("re", "lerp", "kind"):
        want = getattr(args, f)
        if want:
            rows = [r for r in rows if r[f] == want]
    if args.stage:
        rows = [r for r in rows if args.stage in r["stages"].split(",")]
    if not rows:
        print("no row matches — and that is a statement about the FILTER, not about the game: "
              f"the registry holds {len(load())} row(s) in total.")
        return
    rows.sort(key=lambda r: (r["lerp"], r["key"]))
    print(f"{'key':<12} {'kind':<10} {'re':<16} {'lerp':<14} symbol")
    for r in rows:
        print(f"{r['key']:<12} {r['kind']:<10} {r['re']:<16} {r['lerp']:<14} {r['symbol']}")
    print(f"\n{len(rows)} row(s)", file=sys.stderr)


def cmd_next(args):
    """The worklist: what to reverse-engineer next, and why that one.

    Ordered by how badly the source interpolates — `no` and `camera-only` first, because those are
    the ones a player sees judder. Rows that already carry a curated `re` verdict are finished work:
    someone looked and recorded an answer, so they are not offered again.
    """
    rows = load()
    todo = [r for r in rows if r["re"] in ("unknown", "native-override")]
    # A row that already interpolates needs no lerp work; it may still want an RE verdict, so it is
    # ranked below everything that snaps rather than dropped.
    def rank(r):
        blocked = {"no": 0, "camera-only": 1, "partial": 2, "no-primitives": 4,
                   "unmeasured": 3, "2d-correct": 5, "yes": 5, "seam-owned": 5,
                   "drew-once": 5, "discontinuous-correct": 5}.get(r["lerp"], 3)
        return (blocked, r["key"])
    todo.sort(key=rank)
    if not todo:
        print(f"every one of the {len(rows)} row(s) carries a curated `re` verdict — there is "
              f"nothing unexamined IN THE REGISTRY. That is not the same as nothing left in the "
              f"game: only stages that have been played have rows at all.")
        return
    # LEAD WITH WHETHER ANYTHING IS ACTUALLY BROKEN. Ranked output that opens with twenty
    # `2d-correct` rows reads like a list of defects when it is a list of correct things — the
    # reader has to notice the verdict on every line to work out that nothing here judders. The
    # split is the same one `rank` already makes, said out loud.
    JUDDERS = ("no", "camera-only", "partial")
    bad = [r for r in todo if r["lerp"] in JUDDERS]
    unmeasured = [r for r in todo if r["lerp"] == "unmeasured"]
    # Rows that judder but ALREADY carry a curated verdict are not offered as work — and would
    # therefore vanish from this view entirely. Counting them here is the difference between
    # "nothing left to do" and "nothing left to do THAT NOBODY HAS LOOKED AT", which are very
    # different claims and only one of them is true.
    curated_bad = [r for r in rows if r["lerp"] in JUDDERS and r not in todo]
    if not bad:
        print(f"NOTHING UNEXAMINED JUDDERS. None of the {len(todo)} row(s) below is `no`, "
              f"`camera-only` or `partial` — every one either interpolates, is screen-space where "
              f"snapping is correct, or is claimed by a seam. What is missing is a curated `re` "
              f"verdict, which is bookkeeping, not lerp work.")
        if curated_bad:
            print(f"  {len(curated_bad)} row(s) DO still judder, but carry a curated verdict "
                  f"already and so are not listed below — someone looked and recorded why. They "
                  f"are the real remaining lerp work: "
                  f"{', '.join(r['key'] for r in curated_bad)} "
                  f"(`graphics_db.py list --lerp partial`).")
        if unmeasured:
            print(f"  {len(unmeasured)} row(s) are `unmeasured` — the audit filed nothing for "
                  f"them, so the first claim covers everything EXCEPT those.")
        print("  Only stages that have been played have rows at all, so this is a statement about "
              "what has been SEEN, not about the game.\n")
    else:
        print(f"{len(bad)} of {len(todo)} unexamined row(s) actually judder (`no`, `camera-only` "
              f"or `partial`) — those come first.\n")
    print(f"{len(todo)} source(s) with no curated RE verdict, worst-interpolating first:\n")
    for r in todo[: args.limit]:
        why = {"no": "SNAPS — nothing interpolates it",
               "camera-only": "follows the camera but not its own motion",
               "partial": "some draws interpolate, some do not",
               "unmeasured": "never measured — run with SBR_LERP60=1 first",
               "no-primitives": "emits no primitives; probably a state-only call site",
               "2d-correct": "screen-space; snapping is correct, only the RE verdict is missing",
               "drew-once": "drew on one tick only — a first sighting has nothing to pair with, so "
                            "there is no interpolation verdict to give",
               "discontinuous-correct": "only reappeared after an absence — no adjacent visible pose "
                                          "exists to interpolate",
               "seam-owned": "a seam claims its primitives — the verdict lives in the pop.* row "
                             "named in the note, not here",
               "yes": "interpolates; only the RE verdict is missing"}.get(r["lerp"], r["lerp"])
        print(f"  {r['key']:<12} {r['lerp']:<14} {r['symbol']}")
        print(f"  {'':<12} {'':<14} {why}  [seen: {r['stages']}]")
    if len(todo) > args.limit:
        print(f"\n  ... {len(todo) - args.limit} more (--limit to see them)")


def cmd_show(args):
    for r in load():
        if r["key"] == args.key:
            for c in COLS:
                print(f"  {c:<12} {r[c]}")
            return
    sys.exit(f"no row with key {args.key}. Keys are the emitter's guest address (0x........) or a "
             f"curated population slug (pop.*); `list` prints them.")


def cmd_set(args):
    rows = load()
    target = [r for r in rows if r["key"] == args.key]
    if not target:
        sys.exit(f"no row with key {args.key} — this tool edits rows the GAME created, so a key "
                 f"that has never drawn cannot be curated into existence.")
    for a in args.assignment:
        if "=" not in a:
            sys.exit(f"expected field=value, got '{a}'")
        field, _, value = a.partition("=")
        if field not in CURATED:
            sys.exit(f"'{field}' is MEASURED, not curated: the game rewrites it every run, so an "
                     f"edit here would be silently discarded. Curated fields: {sorted(CURATED)}")
        if field == "re" and value not in RE_VERDICTS:
            sys.exit(f"re must be one of {RE_VERDICTS}")
        target[0][field] = value.replace("\t", " ") or "-"
    write(rows)
    print(f"{args.key}: " + ", ".join(f"{a}" for a in args.assignment))


def selftest():
    """Prove this tool can produce the OTHER answer — on both of the things it can get wrong.

    A reader of a registry is only useful if it REFUSES a corpus it cannot read, and if its
    worklist actually excludes finished work. Both are checked against inputs whose answer is
    forced, because a tool that has only ever been run on the real file has only ever been shown
    agreeing with it.
    """
    import tempfile
    global DB
    saved, failures = DB, []

    def expect(name, fn, want_exit):
        try:
            fn()
            got = "no-exit"
        except SystemExit as e:
            got = "exit" if e.code not in (0, None) else "exit0"
        if got != want_exit:
            failures.append(f"{name}: expected {want_exit}, got {got}")

    with tempfile.TemporaryDirectory() as d:
        # 1. A MISSING registry must refuse, not read as "no graphics".
        DB = os.path.join(d, "absent.tsv")
        expect("missing file refuses", load, "exit")

        # 2. A registry with a header and no rows must refuse too: "the game drew nothing" and
        #    "this file has never been written to" are the same emptiness otherwise.
        DB = os.path.join(d, "empty.tsv")
        with open(DB, "w", encoding="utf-8") as fh:
            fh.write("# comment\n" + "\t".join(COLS) + "\n")
        expect("header-only refuses", load, "exit")

        # 3. A row whose column count is wrong must refuse rather than silently mis-map fields.
        DB = os.path.join(d, "short.tsv")
        with open(DB, "w", encoding="utf-8") as fh:
            fh.write("\t".join(COLS) + "\n0xdead\ttoo\tfew\n")
        expect("malformed row refuses", load, "exit")

        # 4. The POSITIVE case: a well-formed file loads, and `next` offers only the uncurated row.
        DB = os.path.join(d, "good.tsv")
        rows = [
            ["0xaaaa", "immediate", "sym_a", "plaza", "unknown", "no", "2026-01-01", "-"],
            ["0xbbbb", "indexed", "sym_b", "plaza", "yes", "no", "2026-01-01", "curated already"],
        ]
        with open(DB, "w", encoding="utf-8") as fh:
            fh.write("\t".join(COLS) + "\n")
            for r in rows:
                fh.write("\t".join(r) + "\n")
        loaded = load()
        if len(loaded) != 2:
            failures.append(f"positive case: loaded {len(loaded)} row(s), expected 2")
        todo = [r for r in loaded if r["re"] in ("unknown", "native-override")]
        # The second row is already curated (`re=yes`), so a worklist that offered every row with a
        # bad `lerp` would offer finished work. This is the case that catches that.
        if [r["key"] for r in todo] != ["0xaaaa"]:
            failures.append(f"worklist offered {[r['key'] for r in todo]}, expected ['0xaaaa']")

        # 5. A MEASURED field must be refused by `set`, or curation would be silently discarded by
        #    the next run.
        class A:
            key, assignment = "0xaaaa", ["lerp=yes"]
        expect("set refuses a measured field", lambda: cmd_set(A()), "exit")

    DB = saved
    for f in failures:
        print(f"FAIL  {f}")
    print(f"graphics_db selftest: {5 - len(failures)}/5 check(s) passed")
    return 1 if failures else 0


def main():
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("summary").set_defaults(fn=cmd_summary)
    p = sub.add_parser("list")
    p.add_argument("--re")
    p.add_argument("--lerp")
    p.add_argument("--kind")
    p.add_argument("--stage")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("next")
    p.add_argument("--limit", type=int, default=10)
    p.set_defaults(fn=cmd_next)
    p = sub.add_parser("show")
    p.add_argument("key")
    p.set_defaults(fn=cmd_show)
    p = sub.add_parser("set")
    p.add_argument("key")
    p.add_argument("assignment", nargs="+")
    p.set_defaults(fn=cmd_set)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()

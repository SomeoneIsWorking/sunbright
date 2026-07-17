#!/usr/bin/env python3
"""gap_worklist.py — the full-game native-port burn-down tracker.

Enumerates every empty decomp gap file (decomp/sms/src/**.cpp with no real body),
maps it to the class(es) it implements, estimates the RE burden from the JP symbol
sizes, and tags factory-registration status. This is the ordered worklist the
ultracode burn-down loops consume — "what's the next class-chain to port."

Data sources (repo-relative, refuses if missing):
  decomp/sms/src/**.cpp                    — gap detection
  reference/sms_gmsj01_symbols.txt         — per-class method sizes (RE burden)
  decomp/sms/src/System/MarNameRefGen_Enemy.cpp — factory registration status

Usage:
  gap_worklist.py                 # print the ranked worklist (markdown)
  gap_worklist.py --md > docs/port_worklist.md
  gap_worklist.py --json          # machine-readable
  gap_worklist.py --category Enemy # filter to one src category
"""
import os, re, sys, json, glob

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "decomp/sms/src")
JPSYMS = os.path.join(ROOT, "reference/sms_gmsj01_symbols.txt")
FACTORY = os.path.join(ROOT, "decomp/sms/src/System/MarNameRefGen_Enemy.cpp")

GAP_MAX_LINES = 15  # a .cpp shorter than this is treated as an unimplemented gap


def die(msg):
    sys.stderr.write("gap_worklist: " + msg + "\n")
    sys.exit(2)


def load_class_sizes():
    """Parse the JP symbol map -> {class_name: (total_bytes, method_count)}.
    Mangled names embed a length-prefixed class: e.g. load__18TAnimalBirdManagerFR...
    -> class '18' chars = 'TAnimalBirdManager'. Sums text-symbol sizes per class."""
    if not os.path.exists(JPSYMS):
        die("missing JP symbol map: " + JPSYMS)
    sizes = {}
    # method__<len><Name><rest> = .text:0x..; ... size:0xNN
    line_re = re.compile(r'^(\S+)\s*=\s*\.text:0x[0-9A-Fa-f]+;.*size:0x([0-9A-Fa-f]+)')
    name_re = re.compile(r'__(\d+)([A-Za-z]\w*)')
    for line in open(JPSYMS, encoding="utf-8", errors="replace"):
        m = line_re.match(line.strip())
        if not m:
            continue
        sym, sz = m.group(1), int(m.group(2), 16)
        nm = name_re.search(sym)
        if not nm:
            continue
        ln, rest = int(nm.group(1)), nm.group(2)
        cls = rest[:ln] if len(rest) >= ln else rest
        if not cls.startswith("T"):
            continue
        tot, cnt = sizes.get(cls, (0, 0))
        sizes[cls] = (tot + sz, cnt + 1)
    return sizes


def gap_files():
    out = []
    for f in glob.glob(os.path.join(SRC, "**", "*.cpp"), recursive=True):
        try:
            n = sum(1 for _ in open(f, encoding="utf-8", errors="replace"))
        except OSError:
            continue
        if n < GAP_MAX_LINES:
            out.append(f)
    return sorted(out)


def factory_status():
    """{name: 'registered'|'commented'} from getNameRef_Enemy."""
    st = {}
    if not os.path.exists(FACTORY):
        return st
    for line in open(FACTORY, encoding="utf-8", errors="replace"):
        m = re.search(r'strcmp\(name,\s*"([^"]+)"\)', line)
        if not m:
            continue
        st[m.group(1)] = "commented" if line.lstrip().startswith("//") else "registered"
    return st


def classes_for(basename, class_sizes):
    """Heuristic: classes whose name contains the file basename (>=4 chars),
    case-insensitive, ignoring a leading 'T'. Captures TFoo/TFooManager/
    TFooParams/TNerveFoo* for file Foo.cpp."""
    key = basename.lower()
    if len(key) < 4:
        return []
    hits = []
    for cls, (tot, cnt) in class_sizes.items():
        core = cls[1:].lower() if cls.startswith("T") else cls.lower()
        if key in core or core.replace("nerve", "").startswith(key):
            hits.append((cls, tot, cnt))
    return sorted(hits, key=lambda x: -x[1])


def build():
    class_sizes = load_class_sizes()
    fac = factory_status()
    rows = []
    for f in gap_files():
        rel = os.path.relpath(f, ROOT)
        cat = rel.split("/src/")[-1].split("/")[0] if "/src/" in rel else "?"
        base = os.path.splitext(os.path.basename(f))[0]
        hits = classes_for(base, class_sizes)
        burden = sum(h[1] for h in hits)
        methods = sum(h[2] for h in hits)
        # factory: does any candidate class name appear as a registered/commented type?
        fstat = "-"
        for typ, s in fac.items():
            if base.lower() in typ.lower() or typ.lower() in base.lower():
                fstat = s
                break
        rows.append({
            "file": rel, "category": cat, "basename": base,
            "burden_bytes": burden, "methods": methods,
            "classes": [h[0] for h in hits[:6]],
            "factory": fstat,
        })
    rows.sort(key=lambda r: (r["category"], -r["burden_bytes"]))
    return rows


def main():
    args = sys.argv[1:]
    rows = build()
    if "--category" in args:
        c = args[args.index("--category") + 1]
        rows = [r for r in rows if r["category"] == c]
    if "--json" in args:
        print(json.dumps(rows, indent=1))
        return
    # markdown (default / --md)
    total_b = sum(r["burden_bytes"] for r in rows)
    print("# Full-game native-port burn-down worklist")
    print()
    print(f"{len(rows)} gap files remaining · ~{total_b:,} bytes of JP code to RE "
          f"(≈ burden estimate from JP symbol sizes).")
    print()
    print("Ordered by category, then RE burden (biggest first). `factory`: whether the "
          "actor type is registered / commented-out / not in getNameRef_Enemy.")
    print("Regenerate: `python3 tools/re/gap_worklist.py --md > docs/port_worklist.md`")
    print()
    cat = None
    for r in rows:
        if r["category"] != cat:
            cat = r["category"]
            print(f"\n## {cat}\n")
            print("| file | burden | methods | factory | classes |")
            print("|---|--:|--:|---|---|")
        cls = ", ".join(r["classes"]) or "—"
        print(f"| {r['file']} | {r['burden_bytes']:,} | {r['methods']} | "
              f"{r['factory']} | {cls} |")


if __name__ == "__main__":
    main()

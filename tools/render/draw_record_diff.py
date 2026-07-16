#!/usr/bin/env python3
"""Diff the COMPLETE dumped record of one draw between two SB draw-dump logs.

Collects every line belonging to draw #A in log1 and draw #B in log2
([draw-dump] + [tev]/[tevreg]/[tevswap]/[tex]/[dd-*]/[texgen]/[vtxarr]/[pn]),
normalizes volatile fields (draw ids, pose-dependent matrix/translation
values unless --strict), and prints a unified diff. Empty diff = the two
draws are state-identical at every instrumented dimension.

Usage: draw_record_diff.py log1 drawA log2 drawB [--strict]
Refuses records with fewer than 3 lines (wrong id / missing instruments).
"""
import re, sys, difflib

TAGS = re.compile(r"\[draw-dump\]|\[tev\]|\[tevreg\]|\[tevswap\]|\[tex\]|\[dd-ch1\]|\[dd-light\]|\[dd-konst\]|\[texgen\]|\[vtxarr\]|\[pn\]")

def record(path, tid):
    out, cur = [], None
    for line in open(path, errors='replace'):
        m = re.search(r"\[draw-dump\] #(\d+) ", line)
        if m:
            cur = int(m.group(1))
            if cur == tid: out.append(line.rstrip())
            continue
        if cur == tid and TAGS.search(line):
            out.append(line.rstrip())
    return out

def normalize(lines, strict):
    out = []
    for l in lines:
        l = re.sub(r"#\d+", "#N", l)
        if not strict:
            # pose/frame-dependent numeric payloads: matrix rows, translations, light positions
            l = re.sub(r"(posmtx|trans)=\[?\(?[^)\]]*\)?\]?", r"\1=<pose>", l)
            l = re.sub(r"pos=\([^)]*\)", "pos=<pose>", l)
            l = re.sub(r"\[pn\] (pos|nrm) #N \d+ \[[^]]*\]", r"[pn] \1 <pose>", l)
            l = re.sub(r"vp=\(\d+,\d+ ", "vp=(x,y ", l)  # 0,0 vs 2,2 origin quirk
            l = re.sub(r"mark='[^']*'", "mark=<>", l)
        out.append(l)
    return out

def main():
    strict = '--strict' in sys.argv
    args = [a for a in sys.argv[1:] if a != '--strict']
    p1, a, p2, b = args[0], int(args[1]), args[2], int(args[3])
    r1, r2 = record(p1, a), record(p2, b)
    for name, r in ((p1, r1), (p2, r2)):
        if len(r) < 3:
            sys.exit(f"draw_record_diff: record for {name} has {len(r)} lines — wrong draw id or missing instruments")
    n1, n2 = normalize(r1, strict), normalize(r2, strict)
    diff = list(difflib.unified_diff(n1, n2, lineterm='', fromfile=f"{p1}#{a}", tofile=f"{p2}#{b}"))
    if not diff:
        print(f"IDENTICAL ({len(n1)} record lines)")
    else:
        for l in diff: print(l)

if __name__ == '__main__':
    main()

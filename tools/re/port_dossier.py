#!/usr/bin/env python3
"""port_dossier.py — one-command RE dossier for porting a decomp function to native.

Given a function (US symbol name substring OR hex address), auto-assembles everything a
port needs — disasm (auto-bounded to the next symbol), whether the decomp already has a
C++ body, the boot_stub OSPanic site (if any), and bl-callee resolution — into a single
markdown dossier. Turns "hand-write an extraction agent prompt" into a one-liner; the
output is what a porter (agent or main session) reads to transcribe the body.

Usage:
  port_dossier.py <name-substr|0xADDR> [out.md]

Data sources (repo-relative): scratch/bin/sms.dol, reference/sms_gmse01_funcs.txt,
decomp/sms/{src,include}, sms-boot/boot_stubs. Regenerate the DOL with
tools/re/dol_extract.c if scratch/bin/sms.dol is missing.
"""
import os, subprocess, sys, re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOL = os.path.join(ROOT, "scratch/bin/sms.dol")
FUNCS = os.path.join(ROOT, "reference/sms_gmse01_funcs.txt")
DISASM = os.path.join(ROOT, "tools/re/disasm_range.py")


def load_funcs():
    out = []
    for line in open(FUNCS):
        line = line.strip()
        if not line:
            continue
        addr, _, name = line.partition(" ")
        try:
            out.append((int(addr, 16), name))
        except ValueError:
            continue
    out.sort()
    return out


def find(funcs, query):
    if query.lower().startswith("0x"):
        addr = int(query, 16)
        for a, n in funcs:
            if a == addr:
                return a, n
        return addr, "(no exact symbol)"
    hits = [(a, n) for a, n in funcs if query in n]
    if not hits:
        sys.exit(f"[port_dossier] no symbol matches '{query}'")
    if len(hits) > 1:
        # demangled-ish: prefer the shortest name containing the query as a method
        hits.sort(key=lambda an: len(an[1]))
        sys.stderr.write("[port_dossier] multiple matches, using first:\n")
        for a, n in hits[:8]:
            sys.stderr.write(f"    {a:08x} {n}\n")
    return hits[0]


def next_addr(funcs, addr):
    for a, n in funcs:
        if a > addr:
            return a
    return addr + 0x400  # fallback window


def run(*args):
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=120).stdout
    except Exception as e:  # noqa: BLE001
        return f"(command failed: {e})"


def grep_decomp_body(name):
    # name like initMapObj__11TMapObjTree -> method + class; check for a C++ body.
    m = re.match(r"([A-Za-z0-9_]+)__(\d+)([A-Za-z0-9_]+)", name)
    if not m:
        return "(could not parse mangled name to search decomp)"
    meth, nlen, rest = m.group(1), int(m.group(2)), m.group(3)
    cls = rest[:nlen] if len(rest) >= nlen else rest
    pat = rf"\b{re.escape(cls)}::{re.escape(meth)}\b"
    hits = run("grep", "-rn", "-E", pat, os.path.join(ROOT, "decomp/sms/src"))
    return (f"class={cls} method={meth}\n"
            + (hits if hits.strip() else "(NO decomp body found — cold RE / transcribe from disasm)\n"))


def stub_site(query, name):
    keys = [query]
    m = re.match(r"([A-Za-z0-9_]+)__\d+([A-Za-z0-9_]+)", name)
    if m:
        keys.append(m.group(1))
    for k in keys:
        hits = run("grep", "-rn", k, os.path.join(ROOT, "sms-boot/boot_stubs"))
        if hits.strip():
            return hits
    return "(no boot_stub reference found)\n"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    if not os.path.exists(DOL):
        sys.exit(f"[port_dossier] missing {DOL} — regenerate with tools/re/dol_extract.c")
    funcs = load_funcs()
    addr, name = find(funcs, sys.argv[1])
    end = next_addr(funcs, addr)
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        ROOT, "scratch/re", f"dossier_{name.split('__')[0]}_{addr:08x}.md")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    parts = []
    parts.append(f"# Port dossier — `{name}` @ {addr:08x} (end {end:08x})\n")
    parts.append("## 1. Disassembly (auto-bounded to next symbol)\n```")
    parts.append(run("python3", DISASM, DOL, hex(addr), hex(end), FUNCS).rstrip())
    parts.append("```\n")
    parts.append("## 2. Decomp body present?\n```")
    parts.append(grep_decomp_body(name).rstrip())
    parts.append("```\n")
    parts.append("## 3. boot_stub OSPanic / stub site\n```")
    parts.append(stub_site(sys.argv[1], name).rstrip())
    parts.append("```\n")
    parts.append("## 4. Notes for the porter\n"
                 "- Cross-check lui/addiu sign-extension on the listing vs a decompiler (Ghidra).\n"
                 "- Watch LP64: 32-bit offsets stored in pointer-typed fields (narrow to u32);\n"
                 "  BE-swap any raw stream.read of on-disc data; return TVec3 by value.\n"
                 "- If the body calls an unported callee, run this tool on that address too.\n")

    with open(out_path, "w") as f:
        f.write("\n".join(parts))
    sys.stderr.write(f"[port_dossier] wrote {out_path}\n")
    print(out_path)


if __name__ == "__main__":
    main()

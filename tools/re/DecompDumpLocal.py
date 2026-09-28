# Project-local copy of the shared decomp-port DecompDump.py, run under PyGhidra (Python 3).
# -*- coding: utf-8 -*-
# Ghidra headless GhidraScript (Jython) — decompile functions to C. Game-agnostic.
#
# Run against an ALREADY-ANALYZED Ghidra project:
#   analyzeHeadless <projdir> <projname> -process <prog> -noanalysis \
#       -scriptPath <dir-of-this-file> -postScript DecompDump.py
#
# Env vars:
#   DECOMP_OUT      output dir (default ./build/decomp)
#   DECOMP_TARGETS  file of target vaddrs, one hex per line ('#' comments). If unset/missing,
#                   instead dumps a full function inventory to <out>/functions.csv and exits.
#   DECOMP_SLIDE    optional hex image-base slide added to every target address
#
# Output: <out>/<entry-vaddr>.c per target (clean decompiled C), or <out>/functions.csv
# (vaddr,size,name) for the inventory pass — grep it to choose targets.
import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd

OUT = os.environ.get("DECOMP_OUT", os.path.join(os.getcwd(), "build", "decomp"))
if not os.path.isdir(OUT):
    os.makedirs(OUT)

fm = currentProgram.getFunctionManager()
af = currentProgram.getAddressFactory().getDefaultAddressSpace()
di = DecompInterface()
di.openProgram(currentProgram)
mon = ConsoleTaskMonitor()


def decompile_at(vaddr):
    addr = af.getAddress(vaddr)
    fn = fm.getFunctionContaining(addr)
    if fn is None:
        # Overlay/unreached code the auto-analyzer never turned into a function:
        # disassemble at the entry and create a function on demand, then retry.
        DisassembleCommand(addr, None, True).applyTo(currentProgram, mon)
        CreateFunctionCmd(addr).applyTo(currentProgram, mon)
        fn = fm.getFunctionContaining(addr)
        if fn is None:
            return None, None
    res = di.decompileFunction(fn, 60, mon)
    if res is None or not res.decompileCompleted():
        return fn, None
    return fn, res.getDecompiledFunction().getC()


targets_file = os.environ.get("DECOMP_TARGETS", "")
slide = int(os.environ.get("DECOMP_SLIDE", "0"), 0)
if targets_file and os.path.isfile(targets_file):
    addrs = []
    for ln in open(targets_file):
        ln = ln.split("#")[0].strip()
        if ln:
            addrs.append(int(ln, 16) + slide)
    for va in addrs:
        fn, c = decompile_at(va)
        if c is None:
            print("DECOMP FAIL %08x (fn=%s)" % (va, fn))
            continue
        ep = fn.getEntryPoint().getOffset()
        f = open(os.path.join(OUT, "%08x.c" % ep), "w")
        f.write("// decomp @ %08x  name=%s  size=%d\n" %
                (ep, fn.getName(), fn.getBody().getNumAddresses()))
        f.write(c)
        f.close()
        print("WROTE %08x.c (%d bytes C)" % (ep, len(c)))
else:
    f = open(os.path.join(OUT, "functions.csv"), "w")
    f.write("vaddr,size,name\n")
    n = 0
    for fn in fm.getFunctions(True):
        f.write("%08x,%d,%s\n" % (fn.getEntryPoint().getOffset(),
                                  fn.getBody().getNumAddresses(), fn.getName()))
        n += 1
    f.close()
    print("WROTE functions.csv (%d functions)" % n)

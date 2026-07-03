#@runtime Jython
# -*- coding: utf-8 -*-
# DecompDump — decompile function containing a given VA to build/decomp/<VA>.c.
# Env: SMS_DECOMP_VAS=comma-separated hex list.
import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

targets = os.environ.get("SMS_DECOMP_VAS", "").strip()
if not targets:
    print("[decomp] no SMS_DECOMP_VAS set — skipping")
else:
    af = currentProgram.getAddressFactory().getDefaultAddressSpace()
    fm = currentProgram.getFunctionManager()
    di = DecompInterface()
    di.openProgram(currentProgram)
    mon = ConsoleTaskMonitor()
    outdir = "build/decomp"
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    for tok in targets.split(","):
        tok = tok.strip()
        if not tok:
            continue
        va = int(tok, 16)
        addr = af.getAddress(va)
        fn = fm.getFunctionContaining(addr)
        if fn is None:
            print("[decomp] no function at 0x%08x" % va)
            continue
        res = di.decompileFunction(fn, 60, mon)
        if not res.decompileCompleted():
            print("[decomp] decompile failed for 0x%08x (%s)" % (va, fn.getName()))
            continue
        outpath = os.path.join(outdir, "%08x.c" % va)
        with open(outpath, "w") as f:
            f.write("// %s @ 0x%08x\n" % (fn.getName(), fn.getEntryPoint().getOffset()))
            f.write(res.getDecompiledFunction().getC())
        print("[decomp] 0x%08x -> %s (%s)" % (va, outpath, fn.getName()))

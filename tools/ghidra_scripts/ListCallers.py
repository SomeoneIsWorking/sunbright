#@runtime Jython
# -*- coding: utf-8 -*-
# ListCallers — list every function that BLs to SMS_TARGET_VA.
import os
target = int(os.environ.get("SMS_TARGET_VA", "0"), 16)
if target == 0:
    print("[callers] set SMS_TARGET_VA hex")
else:
    af = currentProgram.getAddressFactory().getDefaultAddressSpace()
    fm = currentProgram.getFunctionManager()
    rm = currentProgram.getReferenceManager()
    addr = af.getAddress(target)
    refs = rm.getReferencesTo(addr)
    seen = set()
    for r in refs:
        if not r.getReferenceType().isCall(): continue
        callee = r.getFromAddress()
        fn = fm.getFunctionContaining(callee)
        name = fn.getName() if fn else "??"
        entry = fn.getEntryPoint().getOffset() if fn else 0
        seen.add((entry, name, callee.getOffset()))
    for entry, name, callee in sorted(seen):
        print("  0x%08x call at 0x%08x  (%s)" % (entry, callee, name))

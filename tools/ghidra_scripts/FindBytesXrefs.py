#@runtime Jython
# -*- coding: utf-8 -*-
# FindBytesXrefs — search initialized memory for a hex byte pattern (SMS_FIND_HEX),
# print every match address, then list functions that reference each match.
import os
from ghidra.util.task import ConsoleTaskMonitor

hexstr = os.environ.get("SMS_FIND_HEX", "").strip()
if not hexstr:
    print("[find] set SMS_FIND_HEX hex bytes (no 0x, no spaces)")
else:
    def to_signed_byte(v):
        v = v & 0xff
        return v - 0x100 if v >= 0x80 else v
    pattern = [ to_signed_byte(int(hexstr[i:i+2], 16)) for i in range(0, len(hexstr), 2) ]
    mem = currentProgram.getMemory()
    fm = currentProgram.getFunctionManager()
    rm = currentProgram.getReferenceManager()
    mon = ConsoleTaskMonitor()

    import jarray
    patbytes = jarray.array(pattern, 'b')

    found = []
    addr = mem.getMinAddress()
    while addr is not None:
        m = mem.findBytes(addr, patbytes, None, True, mon)
        if m is None:
            break
        found.append(m)
        try:
            addr = m.add(1)
        except:
            break

    if not found:
        print("[find] no matches for %s" % hexstr)
    for m in found:
        print("[find] match at 0x%08x" % m.getOffset())
        refs = rm.getReferencesTo(m)
        any_ref = False
        for r in refs:
            any_ref = True
            frm = r.getFromAddress()
            fn = fm.getFunctionContaining(frm)
            name = fn.getName() if fn else "??"
            entry = fn.getEntryPoint().getOffset() if fn else 0
            print("    ref from 0x%08x in %s @ 0x%08x  (type=%s)" % (frm.getOffset(), name, entry, r.getReferenceType()))
        if not any_ref:
            print("    (no direct code refs to this exact address -- may be referenced via SDA/offset or as part of a larger load)")

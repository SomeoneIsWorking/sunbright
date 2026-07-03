#@runtime Jython
# Disasm — print instructions in [SMS_DIS_START, SMS_DIS_END).
import os
start = int(os.environ.get("SMS_DIS_START", "0"), 16)
end   = int(os.environ.get("SMS_DIS_END",   "0"), 16)
if start == 0 or end <= start:
    print("[disasm] set SMS_DIS_START/SMS_DIS_END hex")
else:
    af = currentProgram.getAddressFactory().getDefaultAddressSpace()
    listing = currentProgram.getListing()
    a = af.getAddress(start)
    end_a = af.getAddress(end)
    while a.compareTo(end_a) < 0:
        insn = listing.getInstructionAt(a)
        if insn is None:
            print("  0x%08x  (no instruction)" % a.getOffset())
            a = a.add(4); continue
        print("  0x%08x  %-12s %s" % (a.getOffset(), insn.getMnemonicString(),
                                      " ".join(str(op) for op in insn.getOpObjects(0))
                                      + ("  " + str(insn.getDefaultOperandRepresentation(1))
                                         if insn.getNumOperands() > 1 else "")))
        a = a.add(insn.getLength())

#@runtime Jython
# SMS_DISASM: disassemble a VA range in the SMS US DOL. Args via env:
#   SMS_DISASM_START (hex), SMS_DISASM_END (hex)
import os
from ghidra.program.model.address import AddressFactory

start = int(os.environ.get("SMS_DISASM_START", "0x8016c060"), 16)
end   = int(os.environ.get("SMS_DISASM_END",   "0x8016c060"), 16)

af = currentProgram.getAddressFactory()
space = af.getDefaultAddressSpace()
listing = currentProgram.getListing()
addr = space.getAddress(start)
last = space.getAddress(end)
while addr.compareTo(last) <= 0:
    instr = listing.getInstructionAt(addr)
    if instr is not None:
        print("0x%08x  %s" % (addr.getOffset(), instr.toString()))
        addr = addr.add(instr.getLength())
    else:
        d = listing.getDataAt(addr)
        if d is not None:
            print("0x%08x  .data %s" % (addr.getOffset(), d.toString()))
            addr = addr.add(d.getLength())
        else:
            print("0x%08x  ??" % addr.getOffset())
            addr = addr.add(4)

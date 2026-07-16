#@runtime Jython
# ListFuncsInRange — print entry points of all functions in [SMS_LO, SMS_HI).
import os
lo = int(os.environ.get("SMS_LO","0"),16); hi = int(os.environ.get("SMS_HI","0"),16)
af = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager()
f = fm.getFunctionContaining(af.getAddress(lo))
it = fm.getFunctions(af.getAddress(lo), True)
for fn in it:
    a = fn.getEntryPoint().getOffset()
    if a >= hi: break
    print("[range-fn] 0x%08x %s" % (a, fn.getName()))

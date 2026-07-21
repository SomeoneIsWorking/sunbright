#@runtime Jython
import os
target = int(os.environ.get("SMS_CONST","0x8039da98"),16)
hi = (target>>16)&0xffff
lo = target & 0xffff
lo_s = lo if lo<0x8000 else lo-0x10000
# lis loads hi; addi/ori completes. Consider carry: if lo>=0x8000, addi uses hi+1.
listing=currentProgram.getListing()
fm=currentProgram.getFunctionManager()
it=listing.getInstructions(True)
# collect lis results per register
regval={}
count=0
while it.hasNext():
    ins=it.next()
    mn=ins.getMnemonicString()
    if mn=="lis":
        try:
            rd=ins.getRegister(0).getName()
            imm=ins.getScalar(1).getValue()&0xffff
            regval[rd]=(imm, ins.getAddress())
        except: pass
    elif mn in ("addi","ori","addic"):
        try:
            rd=ins.getRegister(0).getName()
            rs=ins.getRegister(1).getName()
            imm=ins.getScalar(2).getValue()
            if rs in regval:
                base=regval[rs][0]<<16
                if mn=="ori":
                    val=base|(imm&0xffff)
                else:
                    val=(base+ (imm if -0x8000<=imm<0x8000 else imm))&0xffffffff
                if val==target:
                    fn=fm.getFunctionContaining(ins.getAddress())
                    print("HIT at 0x%08x fn=%s @0x%08x (lis@0x%08x)"%(ins.getAddress().getOffset(), fn.getName() if fn else "??", fn.getEntryPoint().getOffset() if fn else 0, regval[rs][1].getOffset()))
                    count+=1
                if rd==rs: regval.pop(rs,None)
        except: pass
print("done, hits=%d"%count)

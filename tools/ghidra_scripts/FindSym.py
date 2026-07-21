#@runtime Jython
import os
q=os.environ.get("SMS_SYM","MtxTimeLag")
st=currentProgram.getSymbolTable()
it=st.getAllSymbols(True)
n=0
while it.hasNext():
    s=it.next()
    nm=s.getName()
    if q in nm:
        print("0x%08x %s"%(s.getAddress().getOffset(), nm))
        n+=1
        if n>50: break
print("done n=%d"%n)

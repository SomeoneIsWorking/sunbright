#@runtime Jython
# SMS_LIST: list programs in the current ghidra project (run with -process x -noanalysis)
from ghidra.util.task import ConsoleTaskMonitor
root = currentProgram.getProject().getProjectData().getRootFolder()
print("=== programs ===")
for f in root.getFiles():
    print("PROGRAM:", f.getName())

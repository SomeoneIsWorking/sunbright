"""Ghidra preScript — DOL section blocks for the US GMSE01 image, BSS clipped.

DolLoad.py in the shared decomp-port skill rebuilds a DOL's sections after
BinaryLoader's flat import, which is what makes guest addresses resolve. It
aborts on this image: the header's BSS range (0x803E9700..0x8040EB98) overlaps
DATA6 (0x8040C1C0..0x8040CF00), so createUninitializedBlock raises
MemoryConflictException and the whole load fails.

That overlap is a fact about this DOL, not about the loader, so the fix belongs
here rather than in the shared script: create BSS only over the sub-ranges that
no data section already claims, and report what was skipped. Clipping silently
would be worse — the gap in coverage would read as "the loader covered
everything".

Env:
  DOL_PATH   path to the .dol to reparse (required)
"""

import os
import struct
import sys

from ghidra.program.model.mem import MemoryConflictException
from ghidra.util.task import ConsoleTaskMonitor
from java.io import ByteArrayInputStream

DOL_PATH = os.environ.get("DOL_PATH")
if not DOL_PATH or not os.path.isfile(DOL_PATH):
    print("[DolLoadLocal] FATAL: DOL_PATH unset or missing: %r" % DOL_PATH)
    sys.exit(1)

with open(DOL_PATH, "rb") as fh:
    dol = fh.read()


def u32s(off, n):
    return struct.unpack(">%dI" % n, dol[off:off + 4 * n])


t_off, d_off = u32s(0, 7), u32s(28, 11)
t_addr, d_addr = u32s(72, 7), u32s(100, 11)
t_size, d_size = u32s(144, 7), u32s(172, 11)
bss_addr, bss_size, entry = struct.unpack(">III", dol[216:228])

program = currentProgram
mem = program.getMemory()
af = program.getAddressFactory().getDefaultAddressSpace()
mon = ConsoleTaskMonitor()

for block in list(mem.getBlocks()):
    print("[DolLoadLocal] drop '%s' 0x%x..0x%x" % (
        block.getName(), block.getStart().getOffset(), block.getEnd().getOffset()))
    # removeBlock needs the TaskMonitor; passing only the block has no overload.
    mem.removeBlock(block, mon)


def add_initialized(name, vaddr, foff, size):
    # createInitializedBlock is the API that takes a source stream; createBlock
    # wants a source *block*, and createByteBlock does not exist on MemoryMapDB.
    blk = mem.createInitializedBlock(name, af.getAddress(vaddr),
                                     ByteArrayInputStream(dol[foff:foff + size]),
                                     size, mon, False)
    blk.setRead(True)
    blk.setWrite(True)
    blk.setExecute(name.startswith("text"))


for i in range(7):
    # A DOL may leave a section slot empty; createInitializedBlock rejects a
    # zero-length block, so empty slots are skipped rather than aborting the load.
    if t_size[i]:
        add_initialized("TEXT%d" % i, t_addr[i], t_off[i], t_size[i])
for i in range(11):
    if d_size[i]:
        add_initialized("DATA%d" % i, d_addr[i], d_off[i], d_size[i])

claimed = sorted((d_addr[i], d_addr[i] + d_size[i]) for i in range(11))
bss_end = bss_addr + bss_size
print("[DolLoadLocal] bss 0x%x..0x%x; data claims: %r" % (bss_addr, bss_end, claimed))

cursor, made, skipped = bss_addr, [], []
for start, end in claimed:
    if end <= bss_addr or start >= bss_end:
        continue
    if start > cursor:
        made.append((cursor, start))
    cursor = max(cursor, end)
if cursor < bss_end:
    made.append((cursor, bss_end))
else:
    if cursor == bss_end:
        pass

for start, end in made:
    try:
        mem.createUninitializedBlock("bss_%x" % start, af.getAddress(start),
                                     end - start, False)
        print("[DolLoadLocal] bss block 0x%x..0x%x (%d bytes)"
              % (start, end, end - start))
    except MemoryConflictException as exc:
        skipped.append((start, end, str(exc)))

covered = sum(e - s for s, e in made)
print("[DolLoadLocal] bss: %d of %d bytes mapped, %d block(s); %d clipped"
      % (covered, bss_size, len(made), len(skipped)))
for start, end, why in skipped:
    print("[DolLoadLocal]   clipped 0x%x..0x%x: %s" % (start, end, why))
print("[DolLoadLocal] entry 0x%08x" % entry)

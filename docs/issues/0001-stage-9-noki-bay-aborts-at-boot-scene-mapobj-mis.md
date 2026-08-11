---
id: 1
title: Stage 9 (Noki Bay) aborts at boot: /scene/mapObj missing from the mounted archive
status: investigating
symptom: SBR_STAGE=9 (Noki Bay, mare0.szs) aborts at boot: NULL-pointer r32 at guest address 0x00000000, guest stack MActorAnmData::init <- TObjManager::createAnmData <- TMActorKeeper <- TMapObjBase::makeMActors <- TJumpBase::initMapObj
tags: recomp,assets,jkr,stage9,boot-crash
created: 2026-08-11
updated: 2026-08-11
---

## What is measured

`SBR_FASTBOOT=1 SBR_STAGE=9` dies before rendering a frame. Every scenario does the same
(`SBR_SCENARIO=0,1,2` all abort identically), so it is not an episode-selection artifact.

The crash is three layers from its cause. `MActorAnmData::init` does, with no null check:

    JKRFileFinder* f = JKRFileLoader::findFirstFile(dir);
    do { addFileNum(f->mFileName); } while (f->findNextFile());

The decomp carries an `#ifdef SMS_NATIVE_PLATFORM` guard at exactly this spot
(`decomp/sms/src/M3DUtil/MActorData.cpp:140`) — the recomp runs the game's real PPC code, where
that guard does not exist and cannot.

## The located fact

`sms-recomp/overrides/diag_anmdata.cpp` (SB_LOG=anmdata) logs the directory, the disc paths and
every `findFirstFile` result. On stage 9:

    DVDConvertPathToEntrynum("mare0.szs") -> 113      (mounts, entry found)
    findFirstFile("/common/map")    -> ok
    findFirstFile("/scene/map/map") -> ok
    findFirstFile("/scene/mapObj")  -> NULL

The same lookup succeeds repeatedly on stage 8 (monte0.szs) and stage 7 (delfino0.szs), so it is
this stage's archive, not the lookup.

## What is NOT known, and must not be assumed

Whether `/scene/mapObj` is genuinely absent from mare0.szs or whether our mount of it is incomplete.
Two facts point away from a truncated read: the DVD log shows the whole file streaming in clean
0x8000 chunks with a final partial 0x6500 through the two JKRDecomp ping-pong buffers, and the RARC
directory table sits at the FRONT of the archive, so late corruption could not drop a directory
entry. Against that, the stage's own object list creates a TJumpBase, which is a TMapObjBase and so
needs the mapObj resources — a retail disc whose Noki Bay had no mapObj directory would crash on
console the same way, which cannot be true.

Next probe, not yet run: walk the mounted RARC node table and print its directory names, which
settles "absent from the archive" against "absent from our mount" without inference.

## Do not

Do not "fix" this by null-guarding the recomp side. The guest's own code is what dereferences it,
the archive contents are the input, and a guard would turn a missing-asset bug into a stage that
loads with no map-object animations — a silent success shape.

### Note (2026-08-11)
ROOT CAUSE CHAIN FOUND (2026-08-11). It is not a missing directory and not an archive problem at all
— the system heap is exhausted, and the archive lookup is just the first thing that needed 36 bytes.

The chain, each step measured rather than inferred (SB_LOG=anmdata):

  1. findFirstFile("/scene/mapObj") returns null, and MActorAnmData::init dereferences it unguarded.
  2. findVolume("/scene/...") at that same instant returns the mare0 archive correctly, and the
     archive's tables are intact: its root holds a DIR entry "mapobj" whose stored hash equals the
     one CArcName computes.
  3. findDirectory("mapobj", dirId 0) SUCCEEDS — it returns the directory entry.
  4. getFirstFile then fails anyway, at `new (JKRHeap::sSystemHeap, 0) JKRArcFinder(...)`:
     operator new(36 bytes, heap 0x804178c0) returns NULL.
  5. sSystemHeap is a 130,928-byte heap with a TOTAL of 16 bytes free and a largest free block of 8.
     Exhausted, not fragmented — both numbers had to be read, because getFreeSize() returns the
     LARGEST BLOCK (JKRExpHeap.cpp:570), not the total.

Not a leak in the path that fails: allocations through operator new(size, heap, align) balance
(710 allocations totalling 56,280 bytes against 708 frees). The heap is filled by traffic through
other entry points — JKRHeap::alloc saw 1,011 calls this run, unattributed by heap.

## Three wrong answers on the way, all recorded so nobody repeats them

  * "The directory is missing from the archive." No — it is there, with a correct hash.
  * "The lookup is case-sensitive in our port." No — repeating the identical query in lowercase
    also returns null, and monte0 (stage 8, which works) stores the same lowercase "mapobj".
  * "The allocator is fine, no allocation returns NULL." That came from a hook on JKRHeap::alloc,
    which is NOT the entry point getFirstFile uses. Same trap twice: hooking one of two paths and
    reading the resulting zero as an answer. The frees were miscounted the same way — "0 frees"
    was JKRHeap::free being unhooked while 708 went through JKRExpHeap::free.

## Next probe, named

Attribute the RESIDENT bytes: walk the heap's used-block list (CMemBlock, group id + size) or track
outstanding allocations by size through JKRExpHeap::alloc. Compare against stage 8, which survives
the same load path. Whatever holds ~128 KB of system heap at stage-9 load is the actual bug.

Do NOT fix this by enlarging the system heap or by null-guarding the recomp side. The heap size
matches retail's and the guest's own code is what dereferences the null; a guard would trade a crash
for a stage that loads with no map-object animations.

### Note (2026-08-11)
QUANTIFIED (2026-08-11, same probe): stage 9 consumes ~60 KB MORE system heap than stages that work,
measured at the same point in the load (the first mixed-case archive lookup, which every stage
performs).

    stage 1 (plaza):        largest free block 60,780   total free 60,800   of 130,928
    stage 8 (Pianta):       largest free block 60,780   total free 60,800   of 130,928
    stage 9 (Noki Bay):     largest free block      8   total free      16   of 130,928

Identical numbers for two working stages and near-zero for the failing one: the heap budget is not
marginal, something Noki-Bay-specific takes about 60 KB of it.

What is NOT the cause: JKRThread stacks. Four threads are constructed in every stage (JUTException,
JKRAram, JKRAramStream, JKRDecomp), 16 KB of stack each, 64 KB total — structural, identical in 8
and 9, and confirmed by hooking the constructor rather than assumed.

Where the resident ledger stands: 21 live allocations totalling 69,600 bytes are attributable inside
the heap's address range at the failure, dominated by those four thread stacks. The heap is 130,912
bytes used, so ~61 KB is held by allocations the probe does not see — and that unattributed figure
is suspiciously close to the 60 KB difference between stages. So the next probe is specific:

    JKRExpHeap::alloc (0x802c138c) is hooked, but allocations may reach the heap through the VIRTUAL
    do_alloc instead. Hook that path too, re-run stages 8 and 9, and diff the callers of everything
    above ~1 KB. The ~60 KB Noki Bay holds and Pianta does not is the bug.

Beware the traps this probe has already fallen into, all fixed but all easy to repeat:
  * cache the ADDRESS of JKRHeap::sSystemHeap, never its value — the static is reassigned during
    boot (it starts as the root heap), and caching the value made the ledger report 20.8 MB live in
    a 128 KB heap, including a single 15.7 MB block;
  * filter resident blocks by the heap's [mStart, mEnd) range, not by which heap they were charged
    to, for the same reason;
  * getFreeSize() is the LARGEST FREE BLOCK, getTotalFreeSize() is the sum. Both are needed to tell
    exhaustion from fragmentation (here: both tiny, so exhausted).

### Note (2026-08-11)
REFRAMED (2026-08-11, later): Noki Bay does not need more memory. The system heap LOSES 79 KB.

Walking the heap's own two block lists (JKRExpHeap keeps the free list at +0x74 and the used list at
+0x7C; each CMemBlock carries its size at +0x04 and its next link at +0x0C), at the same point in the
load:

    stage 8 (works):   used 69,988 + free 60,928 = 130,916 of 130,928   <- closes, 12 bytes of rounding
    stage 9 (fails):   used 51,524 + free     96 =  51,620 of 130,928   <- 79,308 bytes in NEITHER list

Both walks terminate normally and every link stays inside the heap's own address range, so this is
not a smashed link that truncates the walk — the walk is complete and the memory is simply absent
from both lists. Memory in neither list is memory the heap has lost: it cannot be allocated and it
will never be freed.

That is why every earlier line of attack came up empty. The allocation ledger built by shadowing all
four JKRExpHeap entry points accounts for only ~70 KB, and it was right to — the missing memory was
never handed out by them.

## What to look at next

Something takes memory out of the free list without putting it on the used list, during Noki Bay's
load specifically. Candidates, in order of how cheaply they can be tested:

  * a CHILD heap created inside the system heap (JKRSolidHeap::create / JKRExpHeap::createSolidHeap
    with the system heap as parent) — a child's memory leaves the parent's free list, and if the
    parent tracks it differently the parent's own accounting looks exactly like this;
  * JKRExpHeap::allocFromTail carving without linking into the used list in our recompilation;
  * a mistranslated block-splitting sequence in the ExpHeap allocator, which would show as the same
    shortfall growing over the load.

The instrument to use is already in diag_anmdata.cpp: `dump_used_blocks` prints both lists with a
validity check. Call it repeatedly during the stage load and find the tick where the shortfall
appears — the allocation that runs immediately before that is the one to read.

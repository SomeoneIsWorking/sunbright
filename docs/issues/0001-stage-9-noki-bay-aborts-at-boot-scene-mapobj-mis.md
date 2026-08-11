---
id: 1
title: Stage 9 (Noki Bay) aborts at boot: /scene/mapObj missing from the mounted archive
status: resolved
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

### Note (2026-08-11)
LOCALISED (2026-08-11, final measurement of the session): the 79 KB does not drain away — it
disappears in a SINGLE step, inside MActorAnmData::init.

Sampling the heap accounting after every allocation, every free, and at every findFirstFile:

    shortfall 0 -> 12 bytes      (a 16,384-byte allocation; alignment rounding)
    shortfall 12 -> 24 bytes     (a 1,024-byte allocation; alignment rounding)
    shortfall 24 -> 12 bytes     (init__13MActorAnmDataFPCcPPCc+0x94)
    shortfall 12 -> 79,308 bytes (init__13MActorAnmDataFPCcPPCc+0x94)   <- one step

Two 12-byte rounding steps, then 79,296 bytes in one jump between two of MActorAnmData::init's own
findFirstFile calls.

Reading: this is USED-LIST CORRUPTION, not allocation. The blocks are still allocated as far as the
game is concerned, but they are no longer reachable from mHeadUsedList — stage 9's used list walks
13 blocks where stage 8's walks 22, and the walk terminates "normally" because the surviving chain
ends in a null link. Memory unreachable from that list can never be coalesced or freed, which is
exactly the shape of the shortfall.

What happens in that window (JKRArchivePri/MActorData.cpp:120-200): the first findFirstFile
enumeration and its addFileNum counting, then six `new MActorAnmDataEach<...>(count)` arrays sized
from those counts, `delete fileFinder`, then a second findFirstFile and the addFileTable pass. A
write past one of those arrays would smash the CMemBlock header of the block after it and unlink
exactly this way.

## Next step, and the tool for it

The runtime already has watchpoints (sb_watch_hit / sb_watch_fire in intrinsics.h). Put one on the
system heap's mHeadUsedList word and on the CMemBlock header of the last block that is still
reachable, then re-run stage 9: the write that breaks the chain will name itself. That is a much
sharper question than "who allocates too much", which is what this issue looked like for three
sessions.

Note the counts are what to compare against stage 8, not the sizes: if our findFirstFile enumerates
a different NUMBER of files than retail (duplicates, or the "." and ".." entries the archive dump
shows in every directory), the six arrays are sized differently and the pass that fills them writes
past the end.

### Note (2026-08-11)
THE CORRUPTING WRITE IS NAMED (2026-08-11): PSMTXCopy, writing into a CMemBlock HEADER.

Arming the watchpoint on the link that gets cut (SBR_WATCH=0x804242fc) catches it:

    [watch] write 0x00000000 (4 bytes) @ 0x804242fc
      #2  func_803499bc+0x152   PSMTXCopy
      #3  func_802d4cf0+0x1e6   (unnamed; symbol list places it inside
                                 initMtxIndexArray__13J3DSkinDeformFP12J3DModelData)

The block at 0x804242f0 has its 0x10-byte header at 0x804242f0..0x80424300, so its content starts at
0x80424300 — and the write lands at 0x804242f8/fc, i.e. EIGHT BYTES BELOW the content it should be
filling. A matrix copy is writing through a pointer that is one 8-byte step short of its buffer, and
what it lands on is the block's next-link, which is why the used list loses its tail.

That also explains why nothing else lined up: the memory is not over-allocated, no allocator
misbehaves, and the archive lookup that appears to fail is simply the first caller to need 36 bytes
after the heap's free list was orphaned.

## Two instrument corrections needed to get here, both worth keeping

  * THE WATCHPOINT ENV VAR IS `SBR_WATCH`, not the `SUNBRIGHT_WATCH_WADDR` that intrinsics.h had
    documented for who knows how long. Three runs were made with the stale name, all reporting
    silence from a watchpoint that was never armed. The comment is fixed.
  * the watchpoint only sees GUEST stores. Two native paths copy straight into guest RAM behind it
    (the DVD DMA in dev_di.cpp and the ARAM DMA in dev_aram.cpp), and both now call the new
    sb_watch_range so a bulk transfer covering the watched address reports itself. Neither was the
    culprit here, but "the watchpoint is silent" could not be trusted until they were covered.

## Next

Read the J3D skin-deform call site at 0x802d4cf0+0x1e6 against the decomp and find which pointer is
short by 8 — a matrix array indexed from the wrong base, or a destination computed as
`content - sizeof(header)` somewhere. This is now an ordinary RE question with a named function and
a known-wrong offset, not a memory mystery.

### Note (2026-08-11)
ROOT CAUSE (2026-08-11): a guest STACK OVERFLOW on the JKRDecomp thread, smashing the heap block
below it. Every earlier layer of this issue was a downstream symptom.

The chain, complete:

  MActor::calc -> J3DModel::calc -> an unnamed recursive J3D function at 0x802d4cf0, which
  RECURSES (measured by counting entries and exits, not by reading the corrupt stack's frames):

      depth  8   1,064 bytes of guest stack
      depth 16   2,280
      depth 32   4,712
      depth 64   9,576          ~152 bytes per level

  It runs on the JKRThread created by JKRDecomp's constructor: a 16,384-byte stack at
  0x804246e0..0x804286e0. The stack pointer is ALREADY at 0x80426488 when the recursion begins —
  8,792 bytes consumed before the first level. 8,792 + 9,576 > 16,384, so the stack runs off the
  bottom of its block and writes into the CMemBlock header of the block below (0x804242f0), zeroing
  its next-link via a PSMTXCopy of a matrix onto stack that is no longer inside the stack.

  That unlinks the tail of the system heap's used list: 79,308 bytes become unreachable, the heap
  cannot satisfy a 36-byte allocation, JKRArcFinder cannot be created, getFirstFile returns null on
  a directory it FOUND, and MActorAnmData::init dereferences it.

## What to check next, before choosing a fix

Do NOT simply enlarge the thread's stack — the size comes from the game's own JKRDecomp constructor
and matches retail. The question is why this call arrives on the decompression thread with 8.8 KB
already spent, and whether retail's path is shallower. Two concrete comparisons:

  * dump the guest call chain from the JKRDecomp thread's entry down to MActor::calc and check it
    against the decomp's own flow — if our port runs model calc inside a decompression callback that
    retail runs elsewhere, that is the defect;
  * measure the same recursion's depth and per-level stack on a stage that WORKS (stage 8 reaches
    the same code with the same 16 KB stack), which says whether Noki Bay's models are simply deeper
    or whether the arrival stack differs.

The instrument is in sms-recomp/overrides/diag_anmdata.cpp: it counts real recursion depth, records
every JKRThread stack's address range, and names which thread a given stack pointer belongs to.

### Note (2026-08-11)
THE DEFECT, NAMED (2026-08-11): stage loading runs on the JKRDecomp thread's 16 KB stack instead of
the setup thread's 64 KB one.

Logging every OSCreateThread with its stack range (r6 = stack top, r7 = size — read BEFORE the
super-call, since the recompiled body clobbers the argument registers; reading them after reported
"1 byte" for every thread and made every thread overlap itself):

    0x8041ba20  16,384 bytes  0x80417a00..0x8041ba00   JKRThread (JUTException)
    0x8041fe60  16,384 bytes  0x8041be40..0x8041fe40   JKRThread (JKRAram)
    0x80424300  16,384 bytes  0x804202e0..0x804242e0   JKRThread (JKRAramStream)
    0x80428700  16,384 bytes  0x804246e0..0x804286e0   JKRThread (JKRDecomp)
    0x803fcbe8  65,536 bytes  0x80569580..0x80579580   the SETUP thread, re-created per job
    ... plus 4 KB helpers

The J3D recursion that overflows runs with its stack pointer at 0x80426488 — inside the JKRDecomp
thread's 16 KB block — and its call path is TMap::load -> TMapModelManager::init ->
TJointModelManager::initJointModel -> TMapModel::initJointModel -> MActor::calc -> J3DModel::calc.
That is stage loading, and the game has a 64 KB thread for exactly this work.

So the recursion is not too deep: 64 levels at ~152 bytes is 9.6 KB, which fits a 64 KB stack four
times over. It runs on the wrong thread, in a 16 KB stack that already had 8.8 KB used.

Stages 1 and 8 reach the same code on the same thread and survive only because their models recurse
16 levels deep instead of 64+ — measured, same instrument, same arrival stack pointer. So this is
latent everywhere and Noki Bay is merely the first model deep enough to fall off the end.

## The fix is a thread-routing question, not a stack-size one

Find why the load continues on the decompression thread: our scheduler runs a JKRDecomp job and the
load work proceeds on that stack, where retail hands it back to the setup thread. Look at
gsched_create / the JKRDecomp work loop in sms-recomp/runtime/guest_sched.cpp and
overrides/native_os_thread.cpp. Enlarging the JKRDecomp stack would hide it and leave the real
routing wrong.

### Note (2026-08-11)
FIXED (2026-08-11), and it was never a stage-9 bug.

The arena — the region the game's heap is built in — was published starting at the end of the DOL
image, 0x80417800. The game's MAIN STACK lives immediately above that: __init_registers loads
r1 = _stack_addr = 0x804277e8 and grows down through 64 KB that belongs to no DOL section, so
nothing in the image accounts for it. We therefore handed the game an arena containing the stack it
was standing on. JKRExpHeap built the 128 KB system heap at 0x804178c0, directly on top, and every
16 KB JKRThread stack allocated out of that heap overlapped the main stack too.

That is why the "wrong thread" reading in the previous note was wrong: the recursion was on the MAIN
thread all along, at SP 0x80426488 on its own stack. That address only LOOKED like the JKRDecomp
thread's stack because JKRDecomp's stack had been allocated on top of it. The scheduler and the
stack-range inference disagreed — scheduler said OSThread 0x80402aa8 (the adopted main thread,
created with no stack of its own), range-match said JKRDecomp — and printing both is what exposed
it. A single answer would have been believed.

## The fix

OSInit already knows the right value and asks for it whenever BootInfo->arenaLo is null:

    OSSetArenaLo(!BootInfo->arenaLo ? &__ArenaLo : BootInfo->arenaLo);
    if (!BootInfo->arenaLo && BI2DebugFlag && *BI2DebugFlag < 2)
        OSSetArenaLo((_stack_addr + 0x1F) & ~0x1F);

__ArenaLo is a linker symbol placed AFTER the stacks (0x80429800 in this DOL). That branch is not a
fallback for odd boots — it is the normal disc-boot path. So `boot_env.cpp` now publishes
BootInfo->arenaLo as ZERO and the game picks its own. No constant on our side, correct for any DOL.
With the debug monitor absent the game takes the second branch and lands on 0x80427800; the system
heap moves to 0x804278c0, clear of the stack.

## Measured, before -> after (SBR_STAGE=9)

  * abort in MActorAnmData::init                  -> boots and RENDERS (scratch/screenshots/arena_stage9.png)
  * system heap largest free block 8 bytes        -> 60,780
  * heap accounting 51,620 of 130,928 (79 KB lost) -> 130,916 of 130,928
  * "USED LIST CUT ... next link now reads 0"     -> gone

Stages 1 and 8 still render (arena_stage{1,8}.png), so nothing regressed to buy it.

## The guard that would have caught it at boot

`sms-recomp/overrides/guard_arena.cpp` overrides OSSetArenaLo and aborts if the new arena lo is at
or below the CALLER'S OWN STACK POINTER — a live address inside a stack by definition, so the check
needs no constant and no threshold. It reports at shutdown when it never ran (silence is not a
pass), and `SBR_ARENA_SELFTEST=1` feeds it a value it MUST reject, verified firing.

## What this says about everything else

The overlap was latent in EVERY stage; Noki Bay is only the first model whose J3D calc recurses 64
levels instead of ~16 and therefore reaches far enough down the main stack to hit the heap block
below it. Any past "missing asset", "null from a lookup that should work" or unexplained corruption
dated before this commit is suspect and worth re-testing rather than trusted.

### Resolution (2026-08-11)
arena lo was published over the game's own main stack; publish 0 and let OSInit use __ArenaLo

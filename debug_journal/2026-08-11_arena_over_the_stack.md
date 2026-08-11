# The heap was built on top of the game's own stack (recomp, fixed 2026-08-11)

**One line:** our boot environment published `BootInfo->arenaLo` as the end of the DOL image, but the
game's main stack lives immediately *above* that image in memory that belongs to no section — so the
game built its heap on the stack it was standing on. Fixed by publishing `arenaLo = 0` and letting
`OSInit` use the DOL's own `__ArenaLo` linker symbol, which is placed above the stacks.

## The symptom, three layers from the cause

`SBR_FASTBOOT=1 SBR_STAGE=9` (Noki Bay) aborted before rendering a frame, on a null dereference
inside `MActorAnmData::init` — the game asking a mounted archive for `/scene/mapObj` and getting
NULL back. It read as a missing-asset bug and was catalogued as one (issue #1). Every intermediate
finding was also true, and none of them was the cause:

  * `/scene/mapObj` genuinely returned NULL — but the directory *was* present in the mounted archive,
    with a correct stored hash, verified entry by entry;
  * the allocation that actually failed was 36 bytes for a `JKRArcFinder`, from a heap whose largest
    free block was **8 bytes**;
  * the heap's used list had been **cut**: the chain agreed for 13 blocks and then a `next` link read
    zero where it had previously read `0x80424610`, so 79,308 bytes were in neither the used nor the
    free list — lost, not in use;
  * a watchpoint on the cut link caught the store: a `PSMTXCopy` writing 8 bytes below a `CMemBlock`
    header, from a `J3DModel::calc` recursion 64 levels deep at ~152 bytes a level.

## The measurement that turned it around

The obvious reading of "a recursion ran off the bottom of a 16 KB stack" is *too deep* or *wrong
thread*. Both were wrong, and the way that surfaced is worth keeping.

The probe named the thread by matching the stack pointer against every JKRThread stack range it had
recorded, and answered: the JKRDecomp thread's 16 KB stack. That reading is what the previous commit
message asserted. It was wrong.

Adding a **second, independently derived answer** to the same question broke it: the cooperative
scheduler *knows* which guest thread holds the token, and it said `OSThread 0x80402aa8` — the adopted
main thread, created with no stack of its own. Two derivations, one from an address range and one
from the scheduler's own bookkeeping, disagreeing:

    [anmdata] the scheduler says the running guest thread is OSThread 0x80402aa8 ... created with
              stack top 0x00000000 (0 = the adopted main thread, which brought its own)
    [anmdata] the recursion runs on the JKRThread created by __ct__9JKRDecompFl+0x28 — a 16384 byte
              stack at 0x804246e0..0x804286e0

Both were reporting honestly. The main thread was running on its own stack, and JKRDecomp's stack had
been allocated *on top of it*. A single answer — either one — would have been believed.

## The layout

    0x80417800   end of the DOL's sections and BSS   <- what we published as arenaLo
    0x804177e8..0x804277e8   the MAIN STACK, 64 KB, in no section at all
    0x804277e8   _stack_addr (r1 at __init_registers)
    0x804297e8   the debug-monitor stack
    0x80429800   __ArenaLo    <- what the game itself uses

`JKRExpHeap` built the 128 KB system heap at `0x804178c0`, covering the whole main stack. The four
16 KB JKRThread stacks allocated out of that heap covered it too. Anything that pushed the main stack
deeper than usual wrote through a heap block.

## Why only Noki Bay

Nothing about stage 9 is special except model depth. Stages 1 and 8 reach the same `J3DModel::calc`
on the same stack and recurse ~16 levels; stage 9 recurses 64+, about 9.6 KB, and that is how far
down it has to go to reach the heap block below. The overlap was latent in **every stage** — Noki Bay
is just the first place it was deep enough to matter. Any older finding of the shape "asset missing"
or "a lookup that should work returned null", dated before this fix, is suspect.

## The fix

`OSInit` already asks for the right value whenever `BootInfo->arenaLo` is null:

```c
OSSetArenaLo(!BootInfo->arenaLo ? &__ArenaLo : BootInfo->arenaLo);
if (!BootInfo->arenaLo && BI2DebugFlag && *(u32*)BI2DebugFlag < 2)
    OSSetArenaLo((void*)(((u32)&_stack_addr + 0x1F) & 0xFFFFFFE0));
```

That branch is not a fallback for unusual boots — it is the normal disc-boot path, which is why every
retail game depends on it. `boot_env.cpp` now publishes zero. With no debug monitor present the game
takes the second branch and lands on `0x80427800`; the system heap moves to `0x804278c0`, clear of the
stack. **No constant of ours is involved**, so it stays correct for any DOL.

## Measured, before → after (`SBR_STAGE=9`)

| | before | after |
|---|---|---|
| boot | abort in `MActorAnmData::init` | renders (`scratch/screenshots/arena_stage9.png`) |
| largest free block, system heap | 8 bytes | 60,780 bytes |
| heap accounted for | 51,620 of 130,928 | 130,916 of 130,928 |
| used-list cut | yes | gone |

Stages 1 and 8 still render, so nothing was traded for it.

## The guard

`sms-recomp/overrides/guard_arena.cpp` overrides `OSSetArenaLo` and aborts when the new arena lo is at
or below **the caller's own stack pointer** — an address that is inside a stack by definition, so the
check needs no constant and no threshold, and every term is measured at the moment of the call. It
would have failed at boot, in one line, instead of surfacing as a missing archive directory in one
stage months later.

It reports at shutdown when it never ran, because "no violation" and "never looked" are otherwise the
same output — and this whole entry is about the second one reading like the first.
`SBR_ARENA_SELFTEST=1` feeds it a value it must reject; verified firing.

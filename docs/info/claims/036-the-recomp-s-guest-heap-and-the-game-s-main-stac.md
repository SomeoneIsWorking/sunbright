---
id: C036
kind: claim
status: holds
created: 2026-08-11
tags: 
---

## Claim

The recomp's guest heap and the game's main stack no longer overlap: BootInfo->arenaLo is published as 0 so OSInit uses the DOL's own __ArenaLo, which sits above the stacks

## Evidence

sms-recomp/runtime/boot_env.cpp publishes 0; measured on SBR_STAGE=9 the system heap moves 0x804178c0 -> 0x804278c0, largest free block 8 -> 60,780 bytes, heap accounting 51,620/130,928 -> 130,916/130,928, and the 'USED LIST CUT' report disappears. Guarded at runtime by overrides/guard_arena.cpp, which aborts on any arena lo at or below the caller's live stack pointer and is self-tested with SBR_ARENA_SELFTEST=1

## What would falsify it

any change to boot_env.cpp's OS_ARENA_LO write, or the guard_arena.cpp override being removed or its shutdown report saying it never ran

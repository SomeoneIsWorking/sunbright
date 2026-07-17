# rand() is libc's, not the GC MSL LCG — game RNG is non-deterministic vs retail (2026-07-17)

## Finding (deferred fix — surfaced for a scope decision)

While porting the `TAnimalBase` flock scatter (every position/heading drawn from `rand()`),
the wide-RE pass flagged that the MSL `rand()` in
`decomp/sms/src/PowerPC_EABI_Support/Msl/MSL_C/MSL_Common/rand.c` never runs. Two compounding
facts:

1. **`rand.c` is not compiled.** `sms-boot/CMakeLists.txt` excludes the whole
   `src/PowerPC_EABI_Support/` tree from `sms-native` (line ~30, "PPC CRT"). So every
   `rand()`/`srand()` in the game binds **libc's** rand at link time.
   - Verified: `grep -c MSL_C/MSL_Common/rand.c build/compile_commands.json` → 0.
   - Verified: a full-link test that calls `rand()` after `srand(1)` returns `1804289383`
     (0x6B8B4567) — glibc's seed-1 output — not the GC LCG's `16838`.

2. **`RAND_MAX` seen by game TUs is host's (2³¹−1), not the MSL 0x7FFF.** The decomp's
   `stdlib.h` (`.../MSL_Common/stdlib.h:9 #define RAND_MAX 32767`) is nested and **not on the
   `-I` path** (only `staged_sms_include/` top-level dirs are). So game TUs get host `<stdlib.h>`.

Currently these two are *internally consistent*: libc `rand()` ∈ [0, 2³¹) scaled by
`1/(RAND_MAX+1)` = `1/2³¹` → [0,1). So the 10 game callsites (all
`rand() * (1.f/(RAND_MAX+1))` or `rand() & mask`) produce plausible [0,1) randomness — but
the sequence is **non-deterministic vs the GC** (glibc LCG ≠ MSL LCG) and the distribution
granularity differs. Faithfulness (not "works") is the bar (CLAUDE.md), so this is a real bug.

## The GC LCG (what faithful rand() must reproduce)

```c
// rand.c — on GC, size_t is 32-bit so `next` wraps at 2^32.
size_t next = 1;
int  rand(void)        { next = 0x41C64E6D*next + 12345; return (next >> 16) & 0x7FFF; } // [0,32767]
void srand(size_t s)   { next = s; }
```
On the LP64 host `size_t` is 64-bit → the state never wraps → even if compiled, the sequence
would still diverge. So the faithful fix is TWO parts, which must land together:

- **Compile `rand.c`** (un-exclude just this one file from the `PowerPC_EABI_Support` filter)
  with the state pinned to 32-bit (`u32 next` / mask the multiply) so it wraps like GC.
- **Force `RAND_MAX == 32767` for game TUs** — else `rand() * (1/(RAND_MAX+1))` with
  rand ∈ [0,32767] but RAND_MAX = 2³¹−1 collapses to ≈0 (a REGRESSION). Cleanest is a small
  forced-include for `sms-native` that `#undef`/`#define RAND_MAX 32767` after `<stdlib.h>`.

## Why deferred

Doing only half regresses; doing both is a **game-wide RNG + CRT-exclusion + RAND_MAX** change
touching all 10 `rand()` callsites and any host code in `sms-native` that calls `rand()`. That
crosses the "name a big/risky fix, let the user decide" bar — it can't be fully verified
without broad testing. Tracked here for a deliberate scope decision.

## Impact on the in-progress animal port (handled, no dependency)

The `TAnimalBase` scatter is ported using the **decomp source idiom** `rand() *
(1.0f/(RAND_MAX+1))` (which the GC compiler folds to `1/32768`), NOT the disasm-folded literal
`1/32768`. Under the current consistent libc regime this yields the correct [0,1) range today,
and becomes bit-deterministic-vs-GC automatically once the global rand fix above lands. So the
animal port is correct now and does not block on this finding.

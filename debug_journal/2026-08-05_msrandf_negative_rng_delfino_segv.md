# `MsRandF()` returned NEGATIVE numbers on the host — an int overflow in the RNG scale

An intermittent SIGSEGV in Delfino Plaza on the decomp runtime (`sms-boot`, `SB_STAGE=1`, ~90% of
runs) traced to the game's random-number helper returning values in **(-1, 0)** instead of **[0, 1)**
on every single call.

## The bug

`decomp/sms/include/MarioUtil/RandomUtil.hpp`:

```c
inline f32 MsRandF() { return rand() * (1.f / (RAND_MAX + 1)); }
```

`RAND_MAX + 1` is **int arithmetic**. The GC SDK's `RAND_MAX` is small, so on the original target
this is an ordinary int and the expression yields the intended `1/(RAND_MAX+1)`. On a host where
`RAND_MAX` is `INT_MAX` (glibc: 2147483647) the addition **overflows to -2147483648**, so the scale
is `-4.66e-10` and every result is negative. Measured directly:

    RAND_MAX          = 2147483647
    RAND_MAX + 1      = -2147483648      (int arithmetic)
    1.f/(RAND_MAX+1)  = -4.65661e-10
    MsRandF() over 2e6 draws: min = -0.999999   max = -5.6345e-07

This is the exact class CLAUDE.md names: a faithful transcription that is benign on PPC and corrupts
on the host.

## How it crashed

`TGraphWeb::getRandomNextIndex` (`src/Enemy/graph.cpp`) does:

```c
int rnd = MsRandF() * num;          // negative
...
if (railNode->mConnections[rnd] == param_2)   // indexes the array FROM BELOW
```

so it reads out of bounds, returns garbage as a graph node index, and `TGraphTracer::moveTo` walks
`unk0->unk0[mCurrIdx].unk0->mPitch` straight into unmapped memory. Backtrace confirmed the fault in
`TGraphTracer::moveTo(int) + 36` (the inlined `setParamFromGraph`).

The caller is `TBoidLeader::updateGoal` → `mTracer->moveToRandomNext()` (`src/Animal/boid.cpp:165`),
which fires when a boid reaches its graph goal — so **the intermittency was the RNG itself**, which
is why the same binary crashed on one run and survived the next with no rebuild in between. That
non-determinism is also what made it look like a stateful or load-related problem at first.

## The blast radius is much wider than the crash

Every randomised quantity in the game was wrong, not merely the graph walk. `MsRandF(l, r)` computes
`rand() * scale * (r - l) + l`, so with a negative scale it returned values **below `l`** — outside
the requested range entirely — for every caller: enemy jump speeds, effect rotations, spawn timers,
particle parameters. `MsRandI(l, r)` the same. The plaza crash was simply the one caller that turned
a wrong value into an out-of-bounds index instead of merely wrong behaviour.

## The fix

Compute the divisor in float, where nothing overflows:

```c
inline f32 MsRandF() { return rand() * (1.f / ((f32)RAND_MAX + 1.f)); }
```

Guarded by `SMS_NATIVE_PLATFORM` so upstream's matching build is untouched. The two forms are
bit-identical wherever the int version was already well-defined (`RAND_MAX` 32767 gives 1/32768
either way), so this changes nothing on the original target — it only stops the host from
corrupting.

## Verified with a control, both directions

    without the fix:  1 survived,  9 SIGSEGV  out of 10 runs
    with the fix:    10 survived,  0 SIGSEGV  out of 10 runs

and both scenes still render: title mean RGB unchanged at (187.2, 217.0, 234.7), Delfino Plaza
renders correctly (buildings, palms, sea, fruit, HUD — `scratch/screenshots/rngfix_delfino.png`).

## Worth checking elsewhere

Any other `X_MAX + 1` or `X + 1` in a scale/divisor expression transcribed from the decomp is the
same latent bug wherever the host's limit is larger than the GC SDK's. This one was found only
because it happened to crash; the silent cases — every out-of-range random value the game has been
using — produced no diagnostic at all.

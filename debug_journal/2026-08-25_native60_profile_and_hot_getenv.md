# 2026-08-25 — native-60 profile on current HEAD; uncached hot-path getenv fixed

## Method

`perf record -F 700` over a bounded native-60 plaza run (SBR_FRAME_RATE=native-60, fastboot,
headless, SBR_QUIT_AFTER=5400), report at --percent-limit 0.5. Whole-process attribution including
dawn/libc; guest-side string compares are the GAME's own code and are labelled as such below.
Re-run after the fix to confirm the delta.

## Where a native-60 tick goes today (Ryzen 7 5700X, current HEAD)

The Aurora FIFO/geometry pipeline dominates — approximately:

| share | symbol |
|---|---|
| 4.9% | fifo BP/opcode `parse` |
| 5.2% | XXH3 retained-array hashing (`XXH3_hashLong` + `XXH3_64bits_update`) |
| 4.0% | `draw_prim` |
| 3.8% | fifo `process` |
| 3.3% | `scan_auto_array_max_indices` |
| 2.8% | `memmove` (buffer copies) |
| 2.3% | `GxFifoInput::appendBigEndian` (guest FIFO appends) |
| ~4.1% | vector/ByteBuffer growth (`_M_range_insert`, `resize` x2) |
| 1.6% | `push_gx_draw` |
| 1.5% | `gxFifoVertexSize` |

The two named leaders from 2026-08-22 remain the leaders: the required FIFO
translation/expansion (~28% across the pipeline above) and the retained-array hash (5.2%).
The rejected page-fingerprint cache (that journal) stays rejected; the named future direction is
an authoritative dirty source that does not add work per guest store — page-protection fault
tracking (mprotect array pages PROT_READ, mark-dirty in the fault handler) is the candidate, at
the cost of signal-safety review against the runtime's existing handlers.

## Fixed: uncached diagnostic getenv on the per-bind hot path

`dev_gxfifo.cpp` read `getenv("SBR_BIND_DECODE_LOG")` on EVERY texture bind: 0.64% getenv +
0.96% strncmp in the first profile. Now read once into a `static const bool`. Post-fix profile:
getenv 0.44%, strncmp 0.73% — and the remaining string-compare cost is the GUEST's own
`strncmp` (recompiled game code doing path/name lookups), i.e. faithful emulation, not runtime
overhead. Audit of every other getenv in linked aurora/sms-recomp code found them all behind
static-once caches.

## Verification

- ctest 18/18 after the fix.
- Two bounded native-60 profiles (before/after), same recipe, exit 0, no GPU incidents.

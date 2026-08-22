# Internal-work profiling and decomp upstream rebase (2026-08-22)

## The measurement correction

Real-world clock performance measurement is retired. Frame/tick duration changes under compiler
work, GPU workers, desktop contention, and pacing while saying nothing about what the game did.
Wall clock remains only as runtime machinery for pacing, watchdogs, and bounded-run safety; its
duration is never optimization evidence, including as an “end-to-end” speed observation.
Optimization candidates come from bounded no-loss CPU sampling plus deterministic work quantities
measured at the owning subsystem. Sampling locates recurring code; only owning-subsystem counters
describe how much game work that code performed.

This correction falsified the previous dormant-diagnostic hypothesis. A no-loss sampling capture
instead placed work in the GX command path: root FIFO parsing/copying, Aurora parsing/draw creation,
auto-array scanning, and per-draw cache hashing. `SB_DRAW_STATS=1` now reports both sides of the same
frame. A settled plaza frame observed approximately:

- 123,500 guest write-gather appends / 357 KB input;
- 1 Aurora replay call / 1.80 MB after display-list expansion;
- 30,400 auto-sized primitives, 169,000 vertices, 506,000 indexed-field visits, and 1.01 MB of index bytes;
- 1,420 finalized Aurora draws and about 18,800 immediate vertices.

The old input buffer called generic `std::vector::insert` for every 1/2/4-byte MMIO store. The new
`GxFifoInput` owns big-endian store encoding and advances a consumed-prefix cursor. Its unit test has
a little-endian known-difference control and exercises compaction/stat reset. The live work control
reports zero compactions, moved bytes, and capacity growths after warmup. A bounded 499 Hz capture
recorded 10,398 samples with zero losses; the old generic insertion hotspot is now confined to the
separate flattened output stream. The next target is repeated per-draw hashing and the duplicate
root/Aurora command parse—not another clock micro-timer.

`dev_gxfifo.cpp` was already a critical legacy monolith. Formatting the touched file expanded its
line count and correctly failed the structure ratchet; the limit was not raised. The independent
VCD/VAT vertex-size rule now lives in `gx_fifo_vertex_layout.{h,cpp}` with direct-layout and NBT3
indexed controls. The FIFO 2D decoder, decline accounting, and report now live in
`gx_fifo_2d.{h,cpp}`, bringing the formatted root device from 1,968 to 1,603 lines while retaining
one authoritative vertex-size formula shared by stream framing and the decoder.

That extraction let clang-tidy see a real direct-vertex cursor defect: position decoding advanced a
shadow pointer, then colour and texture decoding reused the unadvanced payload pointer. Direct
vertices now advance by the declared position width; indexed positions advance only past their
index, since their components live in the external array. The indexed-XF matrix destination check
was also rewritten as `destination < capacity && length <= capacity - destination`, avoiding an
overflow-prone addition before writing the 256-float matrix store.

## Decomp rebase and extension

`decomp/sms` is rebased through upstream `eaa222a0`; its native Clang audit builds green. Rebase
reconciliation had displaced upstream's full `MtxUtil` implementation with five loud stubs, so the
coherent upstream file was restored together with its `TVec3::epsilonEquals` dependency. The native
light-perspective helper still writes a temporary 4×4 matrix and copies the observable 3×4 result,
preserving retail output without reproducing PPC-benign host memory corruption.

Seven typed `MActorAnmData` accessors now name their actual animation tables instead of `getUnk*`.
The native build stages UTF-8 decomp sources through a lexical Shift-JIS literal transformer because
Clang has no `-fexec-charset`; this fixes exact serialized-name searches without corrupting comments
or identifiers. SPC reload no longer remembers pointer identity as an endian proxy: it detects the
blob's current structural state, so an allocator-reused address containing fresh big-endian data is
swapped again. Matching-MWCC verification cannot run without the untracked Japanese Rev-0 disc, but
the native build and focused controls are available and green.

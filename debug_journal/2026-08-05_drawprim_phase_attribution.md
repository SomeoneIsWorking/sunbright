# draw_prim, measured from the inside: the render cost is ~55% per-primitive and ~43% per-DRAW

The previous entry (`b8c48bb`) settled the render lever as *"per-primitive cost, not batching"*, on the
strength of the primitive-size distribution: 46k primitives for 1314 merged draws, 53% of them
4-vertex quads. That reasoning was sound about *primitive shape* and wrong about *where the time
goes*. Phase-probing the body says the split is roughly half and half, and the per-draw half is
concentrated in 1,291 calls rather than spread over 45,913.

## Why the old profiler could not have found this

`SB_PROFILE_DRAWPRIM` timed the body with a `clock_gettime` pair and the max-index scan with a
second pair nested inside it. `clock_gettime` is ~20-25 ns even through the vDSO, against a body of
~340 ns:

* the two outer probes were ~12% of what they measured;
* the two inner probes were charged to `draw_prim`'s own total, so the reported **"scan = 14% of
  draw_prim" was partly the probe measuring itself**;
* splitting the body into seven phases with that probe would have cost more than the body does.

So the probe was replaced with a raw cycle counter (`__rdtsc` on x86-64, `cntvct_el0` on arm64, a
`clock_gettime` fallback elsewhere that the report flags as too coarse), calibrated against
`CLOCK_MONOTONIC` at startup. Measured probe cost: **0.3 ns**, which is what makes a nine-probe
split admissible at all.

## The controls, and what each one can catch

Per the instruments rule — an instrument that cannot report its own failure has the same output for
"no signal" and "broken":

| control | catches | reading |
|---|---|---|
| `unattr` = whole − Σphases | a region with no probe on some path (the phases are built as a partition, so this is *not* trivially zero) | 1.9% |
| `probe cost x probes/call` vs body | the split measuring itself, i.e. the defect that produced the old 14% | 0.5%, prints `TOO HIGH: NOT admissible` above 25% |
| `merged + unmerged + early-return == calls` | an exit path escaping the accounting | 44622 + 1291 + 0 = 45913 ✅ |
| `n=` beside every phase | a phase averaged over calls that never entered it | see below — this one fired |
| `TRUNCATED` on the sample ring | a percentile computed from a clipped distribution | not hit (1291 < 4096) |

**The `n=` control caught a real error in my own first reading.** Dividing each phase by the total
call count made `unmerged` read as *"293 ns/call"*, which looks like an ordinary per-primitive cost.
It runs on 1,291 of 45,913 calls, so its true per-call cost is **35x** that. An average over calls
that never entered the phase is not a per-call cost, and had the column not carried its own
denominator I would have filed the per-draw path as cheap.

## The measurement

Delfino, `SB_STAGE=1 SB_HEADLESS=1 SB_TURBO=1`, steady-state frame. Machine load average was 27.8,
so the **absolute** ms are inflated and only the shares and the distribution shape are used here.

    prologue     0.578ms    2.3%      12.6 ns/call  n=45913 (100%)
    diag-pre     0.645ms    2.6%      14.0 ns/call  n=45913 (100%)
    attr-enum    1.707ms    6.9%      37.2 ns/call  n=45913 (100%)
    idx-scan     6.350ms   25.8%     139.2 ns/call  n=45634  (99%)
    diag-post    0.710ms    2.9%      15.5 ns/call  n=45913 (100%)
    push-verts   0.701ms    2.8%      15.3 ns/call  n=45913 (100%)
    merge-idx    2.860ms   11.6%      64.1 ns/call  n=44622  (97%)
    unmerged    10.601ms   43.1%    8211.7 ns/call  n=1291    (3%)
    unattr       0.463ms    1.9%

**Per-primitive ≈ 55%** (prologue + diag + attr-enum + idx-scan + push-verts + merge-idx, all at
~46k calls) and **per-draw ≈ 43%** (`handle_draw_unmerged`, 1,291 calls).

## The mean was not enough, so the distribution was taken too

A 8.2 µs mean over 1,291 calls has two explanations with opposite fixes: a uniformly expensive build
path worth restructuring, or a few shader/pipeline compiles at ~1 ms each dragging up an otherwise
cheap path — a warm-up cost that must **not** be optimised for. Percentiles separate them:

    unmerged ns: p50=2615  p90=7795  p99=61025  max=1137088   (n=1291)

Both effects are real and both matter:

* **p50 = 2.6 µs is a genuine, uniform per-draw cost.** 1,291 x 2.6 µs ≈ 3.4 ms/frame as a floor.
* **the tail is not warm-up** — this is a steady-state frame, so the ~13 draws above 61 µs and the
  1.1 ms outlier recur *every frame*. That is roughly another 1 ms/frame.

## What this changes

The `2026-08-05` conclusion "the render lever is a cheaper per-primitive path, not better batching"
is **refined, not overturned**: per-primitive is the larger half, but 43% of `draw_prim` is 1,291
draws, so *fewer or cheaper draws* is a lever of comparable size and was written off. The two are
attacked differently — per-primitive is 46k calls where only constant-factor work matters,
per-draw is 1,291 calls where a 2.6 µs body is worth reading line by line.

Ranked by size, with what each one actually is:

1. **`unmerged` 43%** — per-draw state build. p50 2.6 µs, heavy recurring tail.
2. **`idx-scan` 26%** — per-vertex max-referenced-index scan. Exists *only because array sizes are
   unknown*: fields are collected under `arrays[a].size == 0 && data != nullptr`. The root fix is
   upstream, at whoever leaves `size` at 0, not a faster scan.
3. **`merge-idx` 12%** — `prepare_idx_buffer` + `push_indices`. Per quad this is six separate
   two-byte `ByteBuffer::append` calls, each doing its own capacity check, then a copy of the
   12-byte result into the frame packet, then a `clear()`.
4. **`attr-enum` 7%** — the 26-iteration loop over `GX_VA_PNMTXIDX..GX_VA_TEX7` rebuilding the
   indexed-attribute field list, recomputed for every primitive from state that changes rarely.
   Purely redundant recomputation.

## First fix landed: index generation (12% -> ~6%)

`merge-idx` was taken first because it is the one item on the list that is *pure overhead* rather
than work: every index went into the buffer through its own capacity-checked `ByteBuffer::append`,
so a quad paid six of them to emit twelve bytes. Each branch now sizes its output, takes the tail
pointer once, and writes straight through — N capacity checks per primitive become 1.

Measured with within-run ratios against phases that were NOT touched, because machine load moved
from 27.8 to 12.0 between the two runs and every absolute number fell with it:

| yardstick | before | after |
|---|---|---|
| merge-idx / push-verts | 4.19x | 2.02x |
| merge-idx / prologue   | 5.09x | 2.32x |

Both agree: roughly halved, so ~6% of `draw_prim` (~2.7% of the drain). Modest, and stated as
modest.

### The test found a heap overflow in the optimisation, in the first minute

`prepare_idx_buffer` was `static` inside a 4,700-line TU that pulls in the whole WebGPU renderer,
so the code producing every index in the frame had no unit test. Moving it to
`lib/gx/prim_index.hpp` (templated on the buffer, so the test drives the shipping code rather than
a copy) made one possible, asserting byte-for-byte agreement with the previous implementation
across all seven primitive types, vertex counts 0..64, four start offsets, and an odd buffer
offset. It carries a positive control that perturbs one index and requires the comparison to
notice, so a harness wired to compare nothing cannot pass.

It failed immediately. The QUADS branch sized its reservation with `vtxCount / 4` while the loop
steps by 4 and therefore runs `ceil(vtxCount / 4)` times, emitting a full six indices for a
trailing partial group — so any count not a multiple of 4 wrote **12 bytes past the end of the
allocation**.

The part worth keeping: **the rendered frame could not have caught this.** This game only emits
quads in multiples of 4, so with the overflow present the Delfino mean RGB still matched the
baseline exactly — (135.2, 144.6, 145.8), to the digit. A whole-frame statistic was the wrong
instrument for a change whose correctness claim is "the same bytes", and it would have signed off
on a latent heap corruption.

It is also the characteristic failure of this *kind* of optimisation. The old code reserved as a
hint and re-checked capacity on every append, so a wrong size expression was harmless. Sizing up
front makes the size expression load-bearing. That trade is the reason the test had to come with
the change rather than after it.

## Second fix landed: the per-draw path was formatting an error message it never printed

Splitting the 43% further, `push_gx_draw` opened with **two unconditional `snprintf` calls on every
draw** — one building a 160-char description (including `%s` of the `std::string` frame marker), a
second copying it into a 16-deep ring. The text exists solely so the staging-overflow fatal in
`gfx/common.hpp` can name the runaway draw instead of leaving it anonymous. In a healthy run it is
never read.

Measured with a dedicated sub-probe inside the phase: **~500 ns per draw, 10.5% of the whole
per-draw path.**

The fix is to record, not format: the fields go into a POD ring and `sb_last_draw_desc()` formats
them on demand. The diagnostic is unchanged — same fields, same ring depth, same `OVERFLOWED`
marker. Two details that are not incidental:

* the marker string is **copied, bounded**, not pointed at. `g_sbLastMarker` is reassigned as the
  frame proceeds, so a stored pointer would print whatever the marker was at *fatal* time rather
  than at *draw* time — a diagnostic that quietly attributes the overflow to the wrong pass.
* the empty case prints `(no draws recorded before the overflow)` rather than nothing, because an
  empty report is otherwise indistinguishable from "the recorder never ran", and an overflow
  before the first draw is exactly when that distinction matters.

Result: **~500 ns/draw -> ~74 ns/draw**, and the machine was *busier* for the after-run (load 36.8
vs 26.0), so the gain is understated rather than flattered.

### Proving a rewritten diagnostic still works when its consumer is an abort path

Rewriting the recorder created the classic problem: the only consumer is a fatal that a healthy run
never reaches, so a broken formatter would sit undetected until the day it was needed — the "prove
it fires" rule, in its most literal form.

`LUCENT_DEBUG=drawdesc` now prints the output of **the same function the fatal calls**, once per
frame. Verified on a real run: correct fields, correct ring ordering, the `<- OVERFLOWED` marker on
the final entry, and non-empty markers surviving the bounded copy
(`mark='DrawBuf ChrXlu' drawIdx=3181789`).

## Third fix, and the big one: 37 MB/frame of array uploads for 20 MB of distinct data

Splitting the per-draw path further with the *existing* `SB_PROFILE_GFX` seven-slot breakdown —
which nobody had run against this question — gave a very stable answer:

    per-draw-build us/frame: arrayUpload=2845 shaderinfo+cfg=479 bindgroups=254
                             pipeline_ref=416 build_uniform=301 push_cmd=86 resolve_tex=100

**`arrayUpload` is 63% of the per-draw build and ~28% of the whole drain.** But a time figure does
not say *why*, and "uploads a lot" has two explanations with completely different fixes. So the
instrument was extended to report **volume and distinctness**, not just calls:

    arrays: uploads=968 (37.09 MB)  distinct=492 (20.44 MB)  redundancy=1.8x  cache-hits=2401

Half the traffic is the *same bytes uploaded twice in one frame*.

### The cause

`AttrArray::cachedRange` is a one-entry cache **per attribute slot**, and `GXSetArray` drops it
whenever the registration changes. The game walks its scene graph re-pointing `GX_VA_POS` at object
A, then B, then back at A — and A is uploaded again, having already been uploaded this frame. The
cache is keyed by *which slot currently points at the data*; the uploaded range is a property of
*the data*.

### The precondition, measured before the change rather than assumed

Keying the cache on data identity is sound only if a given `(pointer, size)` holds the same bytes
for the whole frame. If the game ever rewrites an array in place between two draws, a data-keyed
cache serves the stale upload — and the failure would be silent, intermittent geometry corruption,
the worst kind to debug. There is even a diagnostic in the tree (`SB_NO_ARRCACHE`) that exists
because that exact worry was live once.

So it was measured, not argued: hash the uploaded bytes, and count any key whose content changes
within a frame.

    arrays: in-frame content changes under an unchanged (ptr,size): 0  <- a data-keyed upload cache is SAFE

Zero, on every frame. **The counter stays in the build**, so a future scene that violates the
precondition reports it instead of quietly rendering stale geometry.

### Result

    arrays: uploads=492 (20.44 MB)  distinct=492 (20.44 MB)  redundancy=1.0x
    arrays: data-keyed cache hits (uploads avoided)=476

**37.09 MB -> 20.44 MB per frame**, redundancy exactly 1.0x — every byte uploaded once. In time,
against three untouched yardsticks in the same runs (load moved 14.6 -> 9.8, so absolutes are again
not comparable):

| yardstick | before | after |
|---|---|---|
| arrayUpload / shaderinfo+cfg | 5.94x | 3.66x |
| arrayUpload / pipeline_ref   | 6.84x | 5.21x |
| arrayUpload / build_uniform  | 9.45x | 6.72x |

Roughly a 30% cut in array-upload time — less than the 45% byte reduction, because `push_storage`
has per-call cost beyond the copy. That is ~8% of the drain, the largest single win in this arc.

Cache lifetime is one frame, cleared where `AttrArray::cachedRange` is cleared: the ranges index
into the frame packet's storage buffer, and outliving it would hand out offsets into a buffer that
has since been rewound.

## The next lever, sized: 100% of the remaining 20.4 MB/frame is unchanged from the previous frame

With the redundancy gone, 20.44 MB/frame remains — and it is re-uploaded every frame because the
frame packet's storage buffer is rewound each frame, not because anything changed. Since every
upload is already hashed, the ceiling on a persistent-buffer change was cheap to measure:

    arrays vs PREVIOUS frame: unchanged=20.44 MB (100%)  changed=0.00 MB  new=0.00 MB

**Every byte, every frame.** Nothing changed, nothing new. So a persistent cross-frame geometry
buffer would remove essentially the whole `arrayUpload` cost — the largest single item in the
drain — and it would remove it twice over, since the same 20 MB is also written staging->GPU each
frame.

### The obvious shortcut is unsound, and the reason matters

The tempting version is: offsets are deterministic (measured — all 492 arrays land at the same
offset every frame, `stable=492 moved=0`), and `g_storageBuffer` is a single persistent GPU buffer,
so just skip the staging write and let last frame's bytes stand.

**That is wrong.** `lib/gfx/common.cpp` `Unmap()`s the staging buffer each frame and re-`MapAsync`s
it, and the contents of a re-mapped `MapWrite` buffer are undefined per the WebGPU mapping
contract. It would appear to work — right up until a driver or Dawn version chose to hand back
different memory.

### What was built instead

A persistent arena inside `g_storageBuffer`, sized `StorageBufferSize + PersistentStorageSize`
(48 MB + 32 MB). The arena lives **past** the staging-mirrored region, so the staging->GPU copy,
which maps staging offsets 1:1 onto storage offsets, cannot reach it. The static bind group already
binds the whole buffer with no dynamic offset and shaders index it by byte offset from a uniform,
so nothing about the binding, layout or shaders changes.

Stable arrays are written once with `queue.WriteBuffer` and re-written only when their content hash
differs. The hash is the change detector, and there is no substitute: nothing else can observe the
game rewriting an array, and a stale binding is silent geometry corruption.

    arena: reused=492 (20.44 MB, NOT uploaded)  uploaded=0 (0.00 MB)  full-fallbacks=0
           arena used=20.53 MB in 574 entries

**Zero uploads per steady-state frame.** Against untouched yardsticks in the same runs (load ran
14.6 -> 9.8 -> 4.1 across the three measurements, so absolutes are again not comparable):

| yardstick | original | after redundancy fix | after arena |
|---|---|---|---|
| arrayUpload / shaderinfo+cfg | 5.94x | 3.66x | **1.66x** |
| arrayUpload / pipeline_ref   | 6.84x | 5.21x | **2.54x** |
| arrayUpload / build_uniform  | 9.45x | 6.72x | **3.09x** |

About a 3x reduction overall; `arrayUpload` falls from ~63% of the per-draw build to ~34%. The
~775 us that remains is the hash itself — a 20.44 MB read per frame — which is the price of
detecting change honestly.

### It broke the game, and the fix was not the one I would have guessed

The first working build FATAL'd with `invalid wrap mode 3` at ~frame 400. Bisecting by forcing the
arena to always fall back produced a clean run, which named the arena and nothing else.

Reading my own new code turned up two genuine defects:

1. **A lossy key.** `(ptr << 8) ^ size` folds size's high bits onto pointer bits, so two distinct
   `(ptr, size)` pairs can collide — the cache then serves one array's bytes for another.
2. **A 3-byte tail spill.** `WriteBuffer` needs a 4-byte multiple, so a non-multiple length
   finished with a padded 4-byte write, while the allocator advanced by exactly `length`. Up to 3
   bytes landed in whatever was allocated next.

Fixing both cleared it — but "changed two things, it works now" is not a root cause, so both were
measured:

    allocations=500  non-4-multiple=74  old-lossy-key collisions=0

**The tail spill was the trigger** (74 of 500 allocations hit it; vertex strides like 6 bytes for
S16 XYZ are routinely not 4-aligned). The lossy key never collided in this scene — a real latent
defect, fixed pre-emptively, but not the cause. Worth separating: had I stopped at "both fixed", I
would have carried a wrong belief about which class of bug this was.

Also wired `persistent_storage_reset()` into device teardown — the arena's offsets are meaningful
only for the `g_storageBuffer` being destroyed there, and keeping them would hand out ranges into
a dead buffer on the next bring-up.

## The transferable part

Two attributions in this arc were wrong before this one, and the reason is the same each time: a
number was read without its denominator. "scan = 14%" had probe cost inside it; "unmerged =
293 ns/call" was divided by calls that never ran it. Both looked like ordinary results. The fix
that generalises is to make the instrument *print the denominator next to every quantity* — `n=`,
`% of calls`, probe cost, unattributed — so that a misreading has to survive contradicting evidence
printed on the same line.

## A rejected optimisation: collapsing the diagnostic gates (measured, ~1%, reverted)

With the array upload fixed, the phase table at low load showed `prologue + diag-pre + diag-post`
at **13.4% of draw_prim** — roughly 1.2 ms/frame spent walking ~15 gated diagnostic blocks with
every one of them switched off. The obvious fix: one master gate (the OR of all 15 switch names)
so the hot path does one cached load instead of fifteen.

It was built, and it worked correctly — verified in both directions on two independent
diagnostics: `SB_POS_PROBE` 40 lines, `SB_NDC_PROBE`+`SB_NDC_DRAW` 2,185,607 lines, and 0 lines
with nothing set.

**Then it was measured properly, and reverted.** A single before/after was useless here (load moved
3.5 -> 16.4 between runs, and two yardsticks disagreed 22% vs 0%), so both binaries were built and
run ALTERNATELY so each saw the same machine load, medians taken over ~2,000 frames:

| | ratio vs push-verts | ratio vs prologue |
|---|---|---|
| no gate | 2.008, 1.903 | 2.742, 2.227 |
| master gate | 1.678, 1.837 | 2.121, 2.045 |

~10-16% off the diag phases. Those are ~9% of `draw_prim`, so the whole change is worth **~1% of
draw_prim, ~0.3% of the drain**.

**The theory was wrong.** Fifteen scattered cached-static loads are not where the ~19.5 ns/call
goes — collapsing them recovered only a tenth of it. Whatever costs that time is still in those
regions (work done before a gate, or code-size effects that an outer branch does not fix, since the
blocks stay inline either way). That remains unattributed and is the honest state of it.

Reverted because the trade is bad: ~0.3% of a frame in exchange for coupling a hot function to a
hand-maintained list of switch names, where forgetting one entry does not slow anything down — it
makes that diagnostic **silently dead**. This codebase has paid for that failure mode repeatedly,
and a 0.3% gain does not buy it back. Recorded rather than deleted so the next session does not
re-derive the same idea and reach the same dead end.

Also worth keeping: the first attempt to verify the gate used `./run.sh 2>&1 | grep -c` under a
SIGKILL timeout and reported **0 matching lines for a diagnostic that was in fact emitting
2.18 million of them**. The pipeline was dropping the count when the process was killed. I nearly
concluded I had broken every diagnostic in draw_prim on the strength of it. Redirect to a file and
count the file.

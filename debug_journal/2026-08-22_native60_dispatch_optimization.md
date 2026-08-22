# Native-60 dispatcher optimization and rejected geometry-hash cache

## Symptom and baseline

Native 60 fps still slows down on a Ryzen 7 5700X / RX 6700 XT, while interpolated 60 fps has
visible coverage gaps. The machine is not the limiting specification: a stage-1 profile put the
cost in the recomp/runtime path. The already-landed sparse dispatch table had reduced
`call_ppc + override_lookup` from 9.65% to 3.96% of sampled cycles, but every statically known PPC
`bl` and out-of-function `b` still repeated address folding, override lookup, and recompiled-table
lookup at runtime.

## Earlier milestone: sparse address dispatch and MMIO routing

The first controlled run measured about 14.6 ms in the guest/FIFO phase and 7.7 ms in render, or
22.3 ms for a complete tick. Profiling initially attributed 5.26% of samples to `call_ppc` and
4.39% to `override_lookup` (9.65% combined). The override registry's min/max reject was ineffective
for ordinary game calls because registered overrides span most of the code window, so each call
usually paid an `unordered_map` miss and then a binary search of the generated-function table.

Both registries answer the same exact-address question over the aligned MEM1 code window.
`GuestAddressTable` became their single lookup mechanism: a sparse two-level page table with two
indexed loads on the hot path and allocation only for pages that contain functions. It rejects
misaligned, out-of-window, null, and duplicate entries. Override entries live in a `deque`, keeping
the table's stored pointers stable. A focused test covers boundary addresses, misses, aliases,
misalignment, out-of-range addresses, and duplicate ownership.

The MMIO router was another repeated general scan because every write-gather FIFO write reaches it.
It retains the authoritative non-overlapping device registry but caches the last matched device per
guest thread; a `deque` likewise keeps cached device addresses stable. After these changes,
`call_ppc` was 2.71% and `override_lookup` 1.25% (3.96% combined), and MMIO routing was 0.63% in
`mmio_write` plus 0.61% in its remaining search. Follow-up full ticks were roughly 20.5--21.2 ms.

The `SB_PROFILE_DRAWPRIM=1` instrument also needed a root-cause repair during that milestone. The
recomp submits through `aurora_fifo_replay()`, leaving the live FIFO empty at the later
end-of-frame `fifo::drain()`. The drain returned before reporting or resetting the replay counters.
It now skips only empty live-buffer processing and still reports/resets at frame end. Its path
control (`merged + unmerged + early-return == calls`) passes over roughly 31,300--32,000 calls per
frame, so its mutually exclusive path accounting covers the replay path; that control does not
claim coverage of uninstrumented internal costs.

## Direct branch resolution

The C emitter now distinguishes the two quantities it previously conflated:

- A direct PPC branch has a compile-time target. It emits `call_ppc_direct<Target>`, resolves the
  final override-aware function pointer on first execution, and caches it in the program-wide slot
  for that target address.
- A true indirect transfer (`bctrl`, `blrl`, `bctr`) still uses the general runtime dispatcher,
  because its target is data and may differ on every execution.

Regeneration produced 84,148 direct sites using the target cache and 7,409 general-dispatch sites. The emitter
unit tests assert both the direct-call form and the indirect fallback. A 700-present `perf` run
with zero lost samples measured `resolve_ppc_target` 0.67%, `override_lookup` 0.49%, and
`call_ppc` 0.06%: 1.22% combined, down from 3.96% before the change. The run exited 0 and
`run-safe.sh` observed zero amdgpu timeout/reset/fault events.

## Dead RAM-read bookkeeping

Every fast 8/16/32-bit MEM1 read called `sb_poll_note`, which updated shared globals and called
`sb_poll_fire` after 24 repeated addresses. The latter only logged: it did not yield, advance
CoreTiming, or deliver an interrupt, and its re-entry guard was never set anywhere. That made the
bookkeeping a dead Dolphin-era diagnostic in the hottest guest-memory path. It is removed rather
than optimized. Runtime behavior and the bounded stage run remain green; its isolated wall-clock
effect is inside run-to-run noise, so no speedup is claimed for it.

## Rejected: page-dirty fingerprints for persistent geometry

The next profile attributed 4.14% to XXH3. Its caller was Aurora's persistent indexed-array arena:
it hashes arrays every frame so an unchanged `(pointer,size)` can reuse GPU storage without serving
stale bytes. `SB_PROFILE_DRAWPRIM=1` measured the actual input on repeated stage-1 frames:

- 20.74 MiB previously seen arrays per frame were 100% byte-identical;
- 0.05 MiB was new;
- 0 in-frame content changes occurred under an unchanged `(pointer,size)`;
- 746 prior storage offsets were stable and 0 moved.

A page-version fingerprint prototype reduced XXH3 from 4.14% to about 0.64%. The first version
incremented a counter on every guest store and was plainly bad: guest logic rose from about 13 ms
to about 20 ms. A once-per-page-per-frame epoch avoided that cliff, but a same-binary enabled /
disabled control still measured 13.7 + 7.7 ms versus 13.0 + 8.3 ms (guest + render): 21.4 ms with
the cache and 21.3 ms without it. It merely moved cost across the seam. The entire prototype and
its Aurora changes were removed. A future replacement needs a cheaper authoritative dirty source,
not another store on every guest write.

Earlier candidates were also measured and removed rather than retained speculatively:

- Replacing the FIFO scalar append path with `vector::push_back` regressed the guest phase to
  35--40 ms.
- A custom raw gather buffer with a focused test regressed the guest phase to about 16.9 ms versus
  12.5 ms in its paired baseline.
- Avoiding command-processor dirty flags for writes whose values had not changed produced no
  measurable improvement (about 12.5 ms guest + 8.0 ms render either way).
- Moving the MMIO cache into the header as an external thread-local cache regressed the guest phase
  to about 13.7 ms versus 12.5 ms. The retained function-local cache measured better.

## Remaining cost

This is a measured improvement, not completion of native 60 fps. Heavy Delfino ticks still exceed
the 16.67 ms budget. The leading remaining CPU costs are Aurora GX primitive decoding/building,
Sunbright's required FIFO translation/display-list expansion, the unchanged-array hash, buffer
copies, and rendering. The FIFO parser cannot simply be deleted: it expands guest display lists
and translates GameCube addresses into host pointers before Aurora can consume the stream.

## Verification

- Clang builds passed for the recompiler and `sms-recomp`; generated code was regenerated.
- Recompiler CTest passed 1/1 and runtime CTest passed 7/7.
- `tools/cpp_quality.py` passed clang-format and clang-tidy against both real compile databases.
- A 700-present Native-60 stage-1 `run-safe.sh` run completed with no GPU timeout, reset, or fault.
- The direct-cache profile had zero lost samples; the page-version A/B used an explicit same-binary
  disabled control rather than comparing separately built programs.

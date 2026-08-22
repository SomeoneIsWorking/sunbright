# Native-60 guest-dispatch optimization

## Symptom and budget

Native 60 FPS was functionally correct but missed the 16.67 ms frame budget in the settled heavy
part of Delfino Plaza. The preceding controlled run measured about 14.6 ms in the guest/FIFO phase
and 7.7 ms in render, or 22.3 ms for a complete tick. This change targets work that Native 60 does
twice as often; it does not change game timing, omit draws, or skip a present.

## The profiler was dropping the recomp report

`SB_PROFILE_DRAWPRIM=1` originally emitted no per-frame report in the recomp. The counters were
incremented, but the recomp submits its stream through `aurora_fifo_replay()`. At the later
end-of-frame `fifo::drain()` call the live FIFO is empty, and `drain()` returned before reporting or
resetting the replay counters. Aurora now skips only the empty live-buffer processing and still
performs the end-of-frame report/reset.

The repaired instrument reports about 31,300--32,000 `draw_prim` calls per frame. Its path control
(`merged + unmerged + early-return == calls`) passes, about 921--928 persistent arrays / 20.95--20.96
MiB are reused with zero uploads, and the unchanged-identity content-change control remains zero.
That proves the reporter sees the replay path and that its mutually exclusive path accounting is
complete; it does not prove that every internal cost has a probe.

## Root cause: two general-purpose lookups on every guest call

Every `call_ppc` first queried the native-override registry and then binary-searched the generated
recompiled-function table on a miss. The override registry's min/max reject looked cheap, but the
registered overrides span most of the game's code window, so most ordinary calls fell inside that
range and paid an `unordered_map` miss. The generated function lookup then paid a binary search.

A before profile attributed 5.26% of samples to `call_ppc` and 4.39% to `override_lookup` (9.65%
combined). Both registries answer the same exact-address question over the 24 MiB aligned MEM1 code
window. `GuestAddressTable` is now their single lookup mechanism: a sparse two-level page table with
two indexed loads on the hot path and allocation only for pages that contain functions. It rejects
misaligned, out-of-window, null, and duplicate entries. The override entries live in a `deque`, so
the table's stored pointers remain stable. A focused test covers the first and last valid addresses,
misses, aliases, misalignment, out-of-range addresses, and duplicate ownership.

The MMIO router was another repeated general scan because every write-gather FIFO write reaches it.
It retains the authoritative non-overlapping device registry, but caches the last matched device per
guest thread. A `deque` keeps those cached device addresses stable if later devices register.

In the after profile, `call_ppc` was 2.71% and `override_lookup` 1.25% (3.96% combined), with zero
lost samples. The MMIO router fell to 0.63% in `mmio_write` plus 0.61% in its remaining search.
Follow-up full runs finished around 20.5--21.2 ms per tick, roughly 5--8% faster than the 22.3 ms
baseline. Machine load makes individual runs noisy, so the profile attribution is the stronger
evidence for the dispatch change. Native 60 is still over budget and remains an investigating issue.

## Rejected candidates

These were measured and removed rather than retained as speculative complexity:

- Replacing the FIFO scalar append path with `push_back` regressed the guest phase to 35--40 ms.
- A custom raw gather buffer with a focused test regressed the guest phase to about 16.9 ms versus
  12.5 ms in its paired baseline.
- Avoiding redundant command-processor dirty flags for writes whose values had not changed produced
  no measurable improvement (about 12.5 ms guest + 8.0 ms render either way).
- Moving the MMIO fast cache into the header as an external thread-local cache regressed the guest
  phase to about 13.7 ms versus 12.5 ms. The retained function-local cache is smaller and measured
  better.

## Remaining cost

The after profile is led by `draw_prim`, XXH3 hashing of the roughly 21 MiB retained array set, the
Sunbright FIFO parser, and Aurora command processing. The persistent-array report shows the bytes
are already resident and unchanged, but both consumers still parse the stream and identify the
arrays. Removing that work correctly requires authoritative dirty provenance/versioning or a shared
parsed draw plan. Skipping hashing or parsing solely because this scene happened not to change would
serve stale geometry when the game mutates an array and would be a timing-dependent workaround, not
an optimization.

## Verification

- Clang build of `sms-recomp` and `guest_address_table_test`.
- CTest: 7/7 tests passed.
- `tools/cpp_quality.py`: clang-format and clang-tidy passed for the changed first-party C++.
- `tools/structure_check.py`: 246 files, zero violations.
- `tools/selftest_all.py`: 12/12 tool self-tests and the changed-C++ quality gate passed.
- A 700-present Native-60 stage-1 `run-safe.sh` run completed with no GPU timeout/reset.
- A 180-present profiled `run-safe.sh` run emitted per-frame draw controls and completed with no GPU
  timeout/reset.

# 2026-07-07 — Stage-15 crash chain: JPA TEX1 BE header + stale swap-set recycling

Follow-up to `2026-07-07_one_runtime_consolidation.md`'s "next arc". Two suspected defects,
three actual findings.

## 1. SolidHeap "OUT OF MEMORY" at stage-15 setup — NOT A DEFECT

`SB_JKR_BT=1` backtrace: `TMarDirector::loadParticle → JKRMemArchive::mountFixed →
sb_rarc_swap_to_host` allocating particle.arc's LP64 file-entry side array
(270 entries × 0x18 = 0x1950). This is the DESIGNED fallback path — the comment in
JKRMemArchive.cpp documents that GC-sized archive heaps can't fit the host-stride side
array, and the alloc falls back current→root; JKRSolidHeap::alloc additionally overflows
to host malloc. Never fatal. The report line was reworded from "OUT OF MEMORY" to
"full, overflowing to host" so no future session chases it as a crash cause again.

## 2. SEGV in aurora convert_texture — JPA TEX1 ResTIMG never swapped (real bug #1)

Added a permanent GC-contract ASSERT in aurora `init_texobj_common` (dimensions ≤1024,
message prints the byte-swapped interpretation) — it moved the crash from a far-away
unbounded read in `convert_texture` (`resolve_static_texture` passes
`{obj.data, UINT32_MAX}`, so the size CHECK can never fire) to the creator with a clean
backtrace: `JPATextureResource::registration → JUTTexture::initTexObj → GXInitTexObj`
with width 16384 = BE 64.

Root cause: `sb_jpa_swap_to_host` (JPASwap.h) deliberately leaves TEX1 block bodies
untouched — "TIMG handled by texture path" — but that texture path was the DELETED
Path-B capture renderer with its own BE-aware decoding. Under Aurora, `JPATexture`'s
ctor wraps the raw ResTIMG at `mRawData+0x20` straight into JUTTexture. Fix: the JPA
TEX1 load site now swaps its header like every other ResTIMG consumer
(`sb_jpa_timg_to_host` in JPATexture.cpp → `restimg_swap_to_host`), matching the
JKRFileLoader/.bti and JUTResource/'TIMG' pattern. `SB_JPA_TEXDBG=1` traces each JPA
texture (name, pre-swap width bytes, ptr).

## 3. Pointer-keyed swap-set breaks on heap recycling (real bug #2, systemic)

With fix #2 in place the 77th JPA texture (`P_msm_kemuball3_ia`) STILL arrived BE at
GXInitTexObj: its fresh JPADataBlock copy landed on an address already present in
timg_swap's idempotency set (stale entry — the game freeAll's whole heaps between scenes
and the allocator reuses addresses), so `restimg_swap_to_host` skipped the swap. This
stale-set hazard applies to EVERY swap site (.bti by-path loads, 'TIMG' resources), not
just JPA: any BE header loaded onto a recycled address after a scene teardown silently
skipped its swap.

Fix: content-verified idempotency (timg_swap.cpp). The set became a map
ptr → 0x20-byte post-swap header snapshot. Repeat pointer + bytes == snapshot → still
the swapped occupant, skip. Bytes != snapshot → address recycled by a new occupant →
swap as new and re-snapshot. No JKR heap hooks needed; double-swap remains impossible
because the snapshot is ground truth for "this memory is already host-endian".

## Debugging traps hit (recorded so they aren't re-derived)

- Release-build backtraces attribute static functions to the nearest exported symbol —
  "JPATextureResource::registration" in the bt was really the inlined
  JPATexture/JUTTexture ctor chain. Don't over-trust frame names; verify with data.
- A `cmake --build | grep -E "error|Built target"` health check is NOT proof the changed
  TU compiled: one intermediate build reported "Built target sms-boot" while
  JPATexture.cpp.o was stale (verify with `strings <obj> | grep <new-literal>` when a
  fix "doesn't take"). CCACHE_DISABLE=1 + touch forces truth.
- The dimensions in the corrupt texobj decode arithmetically: BE u16 64 → LE 0x4000 =
  16384; BE u32 0x20 (imageDataOffset) → LE 0x20000000, which is why the faulting image
  pointer sat exactly +0x20000000 from the header.

## Verification

Boot to stage-15 with all fixes: no FATAL, no SEGV; run reaches steady scene rendering
(see `2026-07-07` full-run log + `scratch/screenshots/title_full.png`). The GXInitTexObj
dimension ASSERT stays in aurora as a permanent fail-fast for this whole defect class.

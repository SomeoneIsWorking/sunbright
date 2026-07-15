# 2026-07-15 — Delfino storage-buffer overflow (NOTE: ghost-pass theory FALSIFIED)

> ⛔ CORRECTION (2026-07-15, later same day): **`SB_SKIP_GHOST` DOES NOT EXIST in the aurora
> code** — verified by grep; only `SB_SKIP_ORTHO`/`SB_SKIP_PERSP` exist. Every run below that
> passed `SB_SKIP_GHOST=1` was a NO-OP (unrecognized env, ignored). So the "ghost pass ~doubles
> storage, SB_SKIP_GHOST removes the overflow" conclusion is FALSIFIED — the "ghost off" runs
> were identical to "ghost on". The two 32MB runs differing (one overflowed at 33MB, one reached
> the fifo desync) is FRAME-CONTENT NON-DETERMINISM, not a ghost pass: the storage overflow and
> the `0x70` fifo desync are TWO INDEPENDENT failures in the same Delfino frame, and which one the
> fifo drain reaches first varies per run (the game has RNG/timing-dependent emission). The
> phantom env came from a stale codemap note; I compounded it by instructing an agent to use it.
> WHAT REMAINS TRUE: (a) single-pass Delfino storage genuinely exceeds the 8MB title-era cap
> (real capacity — sized to 32MB); (b) the array cache works (SB_ARR_DBG real=0); (c) the
> `0x70` fifo desync at the StaticMapObj ShadowOpa block boundary is a real, separate bug
> (see below — that part stands). Whether a "phase-1 double-draw ghost pass" exists AT ALL is
> now UNVERIFIED (its only toggle was the phantom env); re-establish with real instruments
> (SB_DRAW_STATS per-drain bytes/draws, SB_SKIP_ORTHO) before claiming it.

## (below: original ghost-pass writeup — the ghost-cause claims are FALSIFIED per above)

> RESOLVED framing (read this first — the sections below record the messy path there):
> The per-frame storage staging overflow has TWO independent causes, established by a 2x2
> (cap 8MB/32MB × ghost on/off):
>   - 8MB, ghost off  → overflow @8.0MB  ⇒ a SINGLE Delfino pass already needs >8MB
>   - 32MB, ghost off → NO overflow (reaches the desync) ⇒ single pass fits in 32MB
>   - 8MB/32MB, ghost on → overflow @8.7 / 33MB ⇒ the ghost pass ~DOUBLES it
> So: (a) gameplay geometry legitimately needs >8MB (the 8MB cap was title-era) — fixed by
> sizing StorageBufferSize to 32MB (measurement-backed, NOT a blind bump); (b) the redundant
> phase-1 ghost pass ~doubles storage — a SEPARATE wart that still overflows 32MB when ON.
> The earlier "it's the ghost pass, not capacity" claim below was CONFOUNDED (the ghost-off
> run had died early at the fifo desync) and is FALSIFIED — it is both.

## (historical, partially-falsified) first pass: "not capacity, it's the ghost pass"

After the GXEnd fixes, the Delfino boot advanced into fifo drain and aborted inside
`aurora::gfx::push_storage` → `ByteBuffer::resize` → `abort()` (common.hpp:155): the
per-frame **storage** staging region (8MB, holds all indexed GXSetArray vertex-array
uploads) overflowed. Made the bare abort() LOUD first (prints have/capacity/need — kept,
it's a real fail-fast improvement).

## Not a capacity issue, not a re-upload storm — it's the ghost pass

Measured, don't guess:
- Bumping StorageBufferSize 8MB→32MB did NOT fix it — it accumulated to 33.4MB and
  overflowed again. So the 8.71MB first-abort was just where the small cap tripped; the
  true single-frame demand is larger. (Bump reverted — it's not the fix.)
- `SB_ARR_DBG=1` at n=2000: **real=0, cached=1684, zero=316** — the array cache WORKS
  (zero genuine size>0 re-uploads). So the blowup is not cache thrash.
- `SB_SKIP_GHOST=1`: **the storage overflow DISAPPEARS** (0 overflow lines; fifo total
  ~1.1MB). 

The phase-1 "ghost pass" (double-draws every DrawBuf under stale ortho — the known
structural wart, previously verified `SB_SKIP_GHOST=1` = bit-identical output at the title)
roughly DOUBLES all storage uploads. Single-pass Delfino is ~16MB of indexed-array data
(genuinely large gameplay geometry, already > the 8MB cap); the ghost pass pushes it to
~33MB. So the overflow has two compounding causes: (a) the redundant ghost-pass doubling,
(b) Delfino's real single-pass geometry already exceeding the title-era 8MB storage cap.

## Proper fix (follow-up, not landed)

Two coupled steps, in order:
1. **Kill the ghost pass** (make SB_SKIP_GHOST the default / remove the phase-1 double-draw)
   — it's redundant by design. MUST re-verify bit-identical on the scenes that use it
   (title was verified; Delfino/file-select were not) before defaulting it off. This halves
   storage (and all draw work).
2. **Right-size the storage cap** to single-pass gameplay demand (~16MB measured → 24–32MB
   with headroom) once the ghost pass no longer doubles it. THEN the bump is honest.

Do NOT just bump the cap — with the ghost pass present it overflows any cap that a fuller
scene reaches, and it papers over the redundant-draw wart.

## CAVEAT (2026-07-15, self-correction): the ghost-pass cause is CONFOUNDED

The `SB_SKIP_GHOST=1` run did NOT prove the ghost pass causes the storage overflow — that
run DIED EARLY at a different bug (a fifo desync at fifo pos ~946KB, see below), so it may
simply never have reached the storage-heavy draws. "Overflow disappeared" is confounded by
early death. Do NOT treat "ghost pass = the storage cause" as established. The clean
experiment still owed: measure the actual per-frame storage high-water with vs without the
ghost pass, both runs reaching the same point (needs the desync fixed first, or a storage
high-water instrument that logs at frame end). What IS solid: the array cache works
(SB_ARR_DBG real=0), and a blind cap bump is wrong (32MB still overflowed at 33MB).

## True next frontier: a fifo desync (host pointer read as opcode)

The no-ghost run dies on a REAL fifo-stream bug:

    [aurora FATAL aurora::gx::fifo] command_processor: unknown opcode 0x70 at pos 946176

The hex dump around the desync contains 64-bit HOST POINTERS (`.. 3b 39 12 7f 00 00 ..` =
0x00007f12393b..) embedded in the stream. Those pointers are LEGITIMATE (ARRAYBASE / TEXOBJ
/ TLUT carry u64 host pointers); the parser desynced UPSTREAM and then landed inside a
pointer payload, reading its low bytes (0x70, 0x60) as opcodes. Last draw-identity marker:
`'DrawBuf StaticMapObj ShadowOpa'`.

### Pinned precisely (2026-07-15, marker-filtered trace SB_FIFO_TRACE_MARK=ShadowOpa)

Every command up to pos 946088 parses CORRECTLY (verified by the per-command trace): the
ShadowOpa quad draw (0x80, vtxCount=4, vtxSize=6 = POS-only XYZ s16, span 27), 3 BP loads,
a GX_AURORA ARRAYBASE (0x50 + u16 subcmd + u64 ptr + u32 + u8 = span 16, correct), XF loads
(span 53 = 12 regs, span 9 = 1 reg, both correct), CP loads (span 6 each) — a complete
VAT-fmt0 setup ending at 946088. THEN 88 zero-bytes (read as NOPs) up to pos 946176, then
`0x70` (invalid) followed by host pointers (`60 3b 71 00 7f 00 00` = 0x00007f00713b60-ish).

Facts: pos 946176 is 32-byte aligned; GX_WRITE_AURORA = `0x50` + BE-u16 subcmd (verified
against the enum in GXAurora.h, ARRAYBASE=0x0010 etc.); the fifo DL mechanism pads display
lists to 32 bytes with ZEROS (fifo.cpp end_display_list). So the 88-zero gap looks like a
block/DL boundary, and the block starting at 946176 (0x70 + pointers) is where valid parsing
should resume but the opcode is wrong. REMAINING HYPOTHESES (next focused session): (a) a
display-list / DRAW_SIZED region whose declared size vs written bytes disagree, leaving the
parser at the wrong offset for the next block; (b) a stubbed shadow-emission path that left
the fifo region partially written; (c) the drain `size` includes a region the frame didn't
actually fill contiguously. Instruments in place: SB_FIFO_TRACE_MARK (per-command trace),
span= in the recent-opcode dump, 128-deep ring, wide hex.

Ruled OUT this session (dead ends — do not re-chase):
- vtxSize miscalc for matrix-index attrs (PNMTXIDX/TEXnMTXIDX): REFUTED — `comp_type_size`
  and `comp_cnt_count` (attr_fmt.cpp) both special-case those attrs to return 1, so
  `calculate_last_vtx_size` sizes them correctly (matches the c0off/t0off `a<GX_VA_POS?1:`).
- pointer-width (LP64) mismatch in the u64-carrying AURORA sub-ops: RULED OUT — emit vs
  parse byte counts match for ARRAYBASE (u64+u32+u8=13), LOAD_TEXOBJ (34), LOAD_TLUT (23),
  LOAD_COPY_DEST (8), DESTROY_COPY_TEX (8).

So the desync ORIGIN is some OTHER command mis-advancing (before the pointer bytes). Added a
per-command SPAN to the fifo error dump (recent opcodes now print `span=` = bytes consumed)
so the mis-advancing command is mechanically identifiable: rerun with SB_SKIP_GHOST=1 and
find the recent-opcode entry whose span ≠ its opcode's correct encoding length.

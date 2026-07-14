# 2026-07-15 — Delfino storage-buffer overflow = the phase-1 ghost pass

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

## True next frontier, revealed underneath (with SB_SKIP_GHOST=1)

The no-ghost run does NOT complete either — it dies on a REAL fifo-stream bug:

    [aurora FATAL aurora::gx::fifo] command_processor: unknown opcode 0x70 at pos 946176

0x70 is not a valid GX opcode → the fifo parser DESYNCED: a preceding draw wrote the wrong
byte count, so a data byte is read as an opcode. Last draw-identity marker before it:
`'DrawBuf StaticMapObj ShadowOpa'` (a shadow draw). Recent draws around the desync are
cmd=0x80 (quads, vtxSize 6 and 12) and 0xA0 (trifan, vtxSize 16). This is a vertex-size /
vtx-descriptor mismatch in some StaticMapObj/shadow draw — the next thing to chase once the
ghost pass is handled (the ghost pass currently masks it by aborting on storage first).

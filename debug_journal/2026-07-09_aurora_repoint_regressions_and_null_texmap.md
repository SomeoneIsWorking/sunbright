# 2026-07-09 — aurora repointed to divergent lineage; re-ported 4 regressions; NULL-texMap setter = fill_rect

## Discovery: aurora submodule on a divergent lineage

`extern/aurora` is checked out at `26d5a7b` (branch `sunbright`, remote `fork/sunbright`).
The top-level records `1787edda`, which **does not exist** in the current repo
(`git merge-base --is-ancestor` → not an ancestor; `git log 1787edda` → unknown revision).
So aurora was repointed to a different fork, and that fork had **regressed every one of the
journal's hard-won fixes**. Consequence: `cmake --build` failed at link/configure, and the
pre-existing 13:23 binary (old aurora baked in statically) crashed at title on the NULL-texMap
FATAL.

## Re-ported onto 26d5a7b (all verified — build green)

1. **`aurora::audio` link removed** (`sms-boot/CMakeLists.txt`). Target gone upstream;
   `sb_audio_frame` is already a no-op (JAS mixer arc not started). Re-add when audio lands.
2. **`GXLoadPosMtxIndx` / `GXLoadNrmMtxIndx3x3`** (`extern/aurora/lib/dolphin/gx/GXTransform.cpp`)
   — were `// TODO` stubs. Now emit the GC CP LOAD_INDX command per decomp GXTransform.c:
   opcode 0x20/0x28 + u32 `(mtx_indx<<16)|((len-1)<<12)|dstAddr` (len=12 pos / 9 nrm).
3. **CP LOAD_INDX parser fixed** (`extern/aurora/lib/gx/command_processor.cpp:453`). The old
   decode had TWO bugs the 2026-07-07 journal named: (a) operator precedence —
   `arrayType = GX_POS_MTX_ARRAY + (opcode - (CP_CMD_LOAD_INDX_A / 0x08))` computed
   `opcode-4` → indexed `arrays[28+]` (OOB); (b) read `srcArrayIdx` as 1 byte + misaligned
   `addrLen`. Fixed: `arrayType = GX_POS_MTX_ARRAY + ((opcode - CP_CMD_LOAD_INDX_A) / 8)`
   → {15,16,17,18}; read full u32 word; `mtxIdx=word>>16`, `len=((word>>12)&0xF)+1`,
   `dstAddr=word&0xFFF`; ASSERT on null array base; honor `array.le`.
4. **3-arg `GXSetArray` shim** (`extern/aurora/include/dolphin/gx/GXGeometry.h`) — was passing
   `le=false` always. Now tags matrix/light arrays (`GX_POS_MTX_ARRAY..GX_LIGHT_ARRAY`)
   `le=true`: J3D's runtime matrix/light pools are host-endian by construction; file-origin
   vertex arrays stay big-endian. (arrays[] has MaxVtxAttr=20 entries, covers indices 15-18.)
5. **`GXGetTexObjAll`** (`extern/aurora/lib/dolphin/gx/GXGet.cpp`) — was `// TODO`. Composed
   from the existing accessors (data/width()/height()/format()/wrap_s()/wrap_t()/has_mips()).
   Header already declared it (`GXGet.h:29`, note `GXBool* mipmap` not `u8*`).
6. **`aurora_discard_frame` restored** (`extern/aurora/lib/{aurora.cpp,include/aurora/aurora.h}`)
   — removed upstream; `frame_seam.cpp` references it for the minimized-surface path. Implemented
   as `gx::fifo::clear_buffer()` (drop fifo unprocessed — no render target; games re-emit state
   every frame, so dropping is stateless+safe). Also stripped per-frame `fprintf` debug spam
   in `begin_frame`/`end_frame`/`fifo::drain` (was flooding logs).

## NULL-texMap setter identified — it's fill_rect (legitimate), emit-0 is faithful

The user's uncommitted WIP had **reverted** `26d5a7b`'s emit-0 (skip `.set()` for NULL texMap)
back to `5421500`'s hard-fail ASSERT in BOTH `shader_info.cpp` and `shader.cpp`. Restored both
to HEAD (`26d5a7b`) — that is the user's committed, documented final decision, corroborated by
CLAUDE.md ("aurora 26d5a7b emits 0 per GC HW"). GC's SU_TREF enable bit is cleared for
`GX_TEXMAP_NULL` → HW-disabled sampling → stage contributes 0; SMS ships such materials.

The user's GXTev.cpp WIP diagnostic (`GXSetTevOrder` backtrace when `tmid==NULL` on stages 0-7)
was LEFT IN PLACE. Run captured the setter:
```
[GXSetTevOrder] STAGE0 tmid=NULL tcid=255 cid=4
  ← (anon)::fill_rect(JDrama::TRect const&, JUtility::TColor)  [ScrnFader.cpp]
  ← TApplication::gameLoop
```
**The NULL-texMap setter is `fill_rect`** — the screen-fade solid-color quad. A solid fill
legitimately uses a TEV stage with no texture (color-only). So the NULL texMap is NOT a game
defect; emit-0 is the correct, faithful behavior. This **closes** CLAUDE.md's "uncommitted
investigation into WHO sets NULL on stages 0-7": the answer is the fader, and it's benign.
(Other setters likely exist for pure-color UI quads; all legitimate.)

## Next blocker: CARD subsystem deadlock (distinct arc)

With emit-0 restored, the title no longer FATALs on the shader. It now dies at:
```
[aurora ERROR aurora::card] Failed to open file: super_mario_sunshine
OSPanic sdk_stubs.cpp:84: OSSendMessage: blocking send on full queue 0x1a32560 (count=0)
```
`count=0` is `mq->msgCount` (capacity) — the game's CARD OSMessageQueue is **0-capacity**
(uninitialized or zero-size). The card open fails (save-file lookup) and the error-completion
path does a blocking `OSSendMessage` on the 0-capacity queue → single-thread deadlock → OSPanic
(fail-fast, correct per CLAUDE.md). This is the CARD subsystem — CLAUDE.md's "known un-gated
remainder." It is a SEPARATE arc from the title-backdrop rendering probes (tasks 2/3) and blocks
them (need a running title).

A `MemoryCardA.USA.raw` (16 MB) exists at `<home>/.local/share/dolphin-emu/GC/`; the failure is a
save-file lookup inside the card, not the card image. The real fix is the 0-capacity queue /
CARD completion protocol, not card provisioning.

## Standing edits in the working tree (uncommitted)
- `extern/aurora`: the 6 ports above + shader_info.cpp/shader.cpp restored to HEAD 26d5a7b
  (discarding the WIP hard-fail revert). GXTev.cpp diagnostic WIP preserved.
- `reference/sms`: `J3DDrawBuffer.cpp` — added buffer NAME to the `[dbhead]` flush line
  (uses existing `sb_boot_drawbuf_name`) for the task-2 census (not yet captured — CARD blocks).
- `sms-boot/CMakeLists.txt`: removed `aurora::audio` link.

## Update: the "CARD deadlock" was a red herring — real blocker was the inline-threading chain

Re-ran under gdb: the OSPanic backtrace was NOT card — it was
`JKRAramStream::write_StreamToAram_Async` → the **ARAM/DVD subsystem**. The
`[aurora::card]` line was a coincidental non-fatal log. Traced and fixed a chain
of identical defects (all the same pattern: a JKRThread worker's command queue is
`OSInitMessageQueue`'d inside `run()`, which never executes under native because
`OSResumeThread` is a no-op → the queue stays 0-capacity → the blocking
`OSSendMessage` at the enqueue site OSPanics; CLAUDE.md's model is "every GC-thread
body runs inline at its enqueue site"). Fixed instances:

1. **JKRAramStream** (`write_StreamToAram_Async`): inline `writeToAram`.
2. **aurora DVD** (`dvd.cpp`): the divergent lineage added a `DvdWorker`
   `std::thread` — structurally incompatible with the panic-on-empty-block
   `OSReceiveMessage` (async completion races the immediate panic). Don't start
   the worker → `enqueue` takes its `executeNow` branch → inline
   `execute(block)` (process_command + callback) → completion fires before
   `DVDReadAsync` returns. This is the documented "No DVD worker thread... every
   DVDRead*/Async completes inline" design.
3. **JKRAramPiece** (`orderAsync`): inline `sendCommand` → `startDMA` →
   `ARQPostRequest`. Confirmed aurora's `AR.cpp` already emulates ARQPostRequest
   as inline memcpy + synchronous `doneDMA` callback (and the decomp `ar.c`/
   `arq.c` are excluded from the native build — no `__ARQInterruptServiceRoutine`
   symbol). So the ARAM DMA completes inline once dispatch reaches it.
4. **vtx attr 14** (aurora `shader.cpp:vtx_attr`): a texcoord referenced by
   TexGen/TEV but `GX_NONE` in the VAT hit FATAL; now defaults to `vec2f(0.0)`,
   matching the existing NRM/CLR defaults + GC HW.
5. **JKRDecomp** (`sendCommand`): inline `decode` + ARAM-chain/callback/completion
   dispatch (was deadlocking `TMarDirector::loadParticle` → `JKRDecompressFromDVD`).

**Result:** the title now boots from a first-frame ARAM deadlock all the way
through `TMarDirector::setup` → `loadResource`/`loadParticle` → `genObject`
(scene-object construction, e.g. "MapObjFlagManager"). This is a large milestone.

**Current blocker (next session):** a SIGSEGV ~around scene construction
(after genObject). Hard to pin — gdb keeps catching parked aurora gpu worker
threads (pipeline_cache_writer / WSI swapchain), not the faulting frame. With
`SB_WATCHDOG_SECS=0` the process still exits 139 (real segfault, not the
watchdog). Next step: `thread apply all bt` under gdb to find the faulting
thread, or run AddressSanitizer. Tasks 2/3 (the `[dbhead]` census + NDC probe)
still need a title that renders; the census diagnostic itself is wired and ready.

NOTE: the earlier "task 4 (oracle diff) infeasible" conclusion stands — the
Dolphin submodule + oracle tiers remain retired.

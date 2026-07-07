# 2026-07-07 — One-runtime consolidation: single thread, sync I/O, Aurora render+audio

User directive: "one runtime, PC native single thread, no OSThread emulation, sync direct
I/O, no emulating disk speed, Aurora for Render and Audio."

## What changed

**Aurora (extern/aurora):**
- `lib/dolphin/dvd/dvd.cpp`: deleted the `DvdWorker` std::thread + command queue. Every
  DVD command (`DVDReadAbsAsyncPrio`, seeks, BS reads) executes inline and fires its
  callback before the API returns; the cancel/retire/wait surface is trivially satisfied
  because a block is never observable BUSY/WAITING after the call. nod
  `preloader_threads = 0`. There was never a simulated transfer rate — the queue itself
  was the only asynchrony.
- New `aurora::audio` module (`include/aurora/audio.h`, `lib/audio.cpp`,
  `cmake/aurora_audio.cmake`): `aurora_audio_open/close/push/queued_frames` over an SDL3
  `SDL_AudioStream` on the default playback device (interleaved S16).
- New `aurora_discard_frame()` C API: drops the queued GX fifo for frames produced while
  the surface was unpresentable (minimized) — without it the fifo grows unbounded because
  `end_frame` must not run when `begin_frame` failed.
- Weak `aurora_host_alloc_push/pop` hooks (same weak-override pattern as OSReport), called
  from the allocating DVD entry points (`DVDFastOpen`, `DVDClose`, `DVDChangeDir`); sms-boot
  overrides them onto the JKR host-alloc gate. **CARD is the known un-gated remainder.**

**sms-boot:**
- `main.cpp` (was `render_gc/aurora_bridge.cpp`): the game runs on the PROCESS MAIN THREAD.
  No game std::thread. `PADSetKeyboardActive(0, TRUE)` so keyboard drives pad 0 out of the
  box (previously nothing enabled it — input was gamepad-only by accident).
- `runtime/frame_seam.cpp`: `sb_frame_present(retraces)` = aurora_end_frame → event pump
  (AURORA_EXIT → aurora_shutdown + `_exit(0)`) → aurora_begin_frame, all under
  `sb_host_alloc_push/pop`, then wall-clock pacing to `retraces` NTSC fields
  (1001/60000 s each; SB_TURBO=1 disables; >4 fields behind resyncs instead of sprinting).
  Kicks the watchdog and pumps `sb_audio_frame()` once per frame.
- Seam placement: `JDrama::TVideo::waitForRetrace` (reference/sms, SMS_NATIVE_PLATFORM
  branch), NOT the `VIWaitForRetrace` SDK call. The game spins on VIWaitForRetrace from
  load-polling and TV-mode settle loops where presenting a half-built fifo would render
  garbage; TVideo::waitForRetrace is the real once-per-frame scan-out point (reached from
  TDisplay::endRendering) and its `param_1` carries the intended field count (2 at 30 fps),
  which is exactly the pacing input the seam needs. VIWaitForRetrace stays a pure counter.
- `runtime/sdk_stubs.cpp` (was `boot_stubs/sdk_gap_stubs.cpp`): OSMessageQueue is now a
  REAL single-threaded ring over the caller's buffer; blocking receive on empty / send on
  full OSPanics (single-thread deadlock, fail fast). GXSetDrawSyncCallback stores the
  callback and GXSetDrawSync dispatches it inline (pipe latency is zero on PC), so
  TDrawSyncManager's retire logic actually runs — the old no-op silently dropped it.
- `runtime/watchdog.cpp`: SIGALRM watchdog now kicked per frame by the seam. The old bridge
  armed it once and never re-kicked — any run past SB_WATCHDOG_SECS survived only because
  the alarm got lost across threads; single-threaded it would have killed every run.
- Layout: `sms-boot/{main.cpp, runtime/, shims/, assets/, boot_stubs/}`. Deleted the whole
  retired Path-B/parity era: `common/` (GX-seam capture layer, cooperative scheduler
  os_impl.cpp, dvd/card/vi/pad seams — none of it compiled since the Aurora refactor),
  `render_pc/` (SDL3-GPU renderer + ngx + shaders), `src/` (boot.cpp two-thread Path-B
  boot, scene_drive, pin_state parity harness, boot_heap_bringup — glslang is gone, and
  JKRHeap's unmarked-thread→host-malloc fallback covers pre-main static allocs),
  `engine.h`, `pin_state_schema.h`. The 25 RE'd pure-spec `sms_boot_*.h` headers moved to
  `shims/` (14 actively included by reference/sms; the rest are porting-worklist specs
  referenced from code comments).

**reference/sms:**
- `JDRVideo.cpp`: TVideo::waitForRetrace native branch calls `sb_frame_present` (above).
- `JKRDecomp.cpp` sendCommand + `JKRAramPiece.cpp` orderAsync: run the dead worker's loop
  body INLINE. Root cause worth remembering: the old no-op OSMessageQueue stubs meant
  `sendCommand` posted to a queue nobody drained and the caller's `sync()` "succeeded"
  without the decode/DMA ever running — silent no-op of two whole subsystems, exactly the
  fail-fast violation class. The title screen worked anyway because the hot boot path uses
  `JKRDecomp::decode` directly (via JKRDvdRipper) rather than the queued path; the queued
  paths were cold, waiting to bite. With the real ring + inline dispatch, the completion
  protocol (doneDMA → command->mMessageQueue → sync()) is coherent and synchronous.

## Confounds / dead ends / notes

- Aurora's GX fifo is UNSYNCHRONIZED globals by design; the previous two-thread bridge
  (game thread writing GX while main thread drained in end_frame every 8 ms) was a live
  data race. Single-threading is the fix, not a preference.
- Do NOT hook the present at VIWaitForRetrace — first attempt did; see seam-placement note.
- Frame pacing cannot ride host vsync (present-blocking): host refresh is arbitrary
  (60/144/240 Hz) while the game wants N NTSC fields per frame. Wall-clock pacing in the
  seam is the correct mechanism.
- Uncommitted work left in the USER's aurora checkout (not shipped here): debug printfs in
  begin/end_frame/fifo, an SB_DUMP_FRAME framebuffer-dump diagnostic (worth landing
  separately), and a re-revert of the NULL-texMap emit-0 fix back to a hard ASSERT plus a
  GXSetTevOrder backtrace probe — an open investigation into which SMS callsite programs
  stages 0-7 with a NULL texmap. That question is still open; committed aurora HEAD
  (26d5a7b) keeps the faithful emit-0 behavior.
- Known open gaps after this consolidation: JAS mixer port (audio pump exists, mixer
  doesn't — game silent by omission); aurora CARD entry points not host-alloc gated;
  GXLoadPosMtxIndx/GXLoadNrmMtxIndx3x3 are silent no-ops (J3D indexed skinning matrices —
  should be implemented or made to panic); THP movies excluded.
- tools/ still contains recomp/oracle-era scripts referencing deleted machinery; pruning
  them is separate cleanup (they don't affect the build).

## Verification (2026-07-07, this branch)

- Build green (`cmake --build build --target sms-boot`).
- Boot run (SB_STAGE=15, SB_TURBO=1): the SUPER MARIO SUNSHINE title logo RENDERS through
  the single-threaded runtime (SB_DUMP_FRAME at frame 150 —
  `scratch/screenshots/title_oneruntime.png`). Game logic runs entirely on thread 1; the
  crash backtrace below doubles as proof of the frame path:
  gameLoop → TDisplay::endRendering → TVideo::waitForRetrace → sb_frame_present →
  aurora_end_frame → fifo::drain, one thread.
- The real OSMessageQueue immediately caught JKRAramStream::write_StreamToAram_Async
  blocking-sending to the never-initialized, never-drained sMessageQueue from
  SMSLoadArchiveARAM at logo time — i.e. streamed ARAM archive loads were being SILENTLY
  SKIPPED under the old no-op stubs on every boot. Fixed inline (writeToAram at the
  enqueue site), same pattern as JKRDecomp/JKRAramPiece.
- Run ends in a PRE-EXISTING failure during stage-15 scene setup: a 0x2a00-byte
  JKRSolidHeap OOMs (requiredSize=0x1950), then SIGSEGV in aurora
  gfx::convert_texture during the next frame's draw (texture image pointer from the
  OOM'd allocation). BASELINE-CONFIRMED pre-existing: the user's pre-consolidation build
  hits the IDENTICAL OOM (same span, same size) and then dies even earlier at its
  re-reverted NULL-texMap ASSERT. The consolidation branch gets strictly further than
  baseline. NEXT ARC: find which SolidHeap this is (0x2a00 span created during stage-15
  setup) and why its sizing underflows — plus make SolidHeap OOM fail fast instead of
  returning null into a texture pointer.

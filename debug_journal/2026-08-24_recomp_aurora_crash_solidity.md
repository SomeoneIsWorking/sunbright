# 2026-08-24 — recomp + Aurora crash-solidity pass

## Scope and evidence standard

The priority lane is the default `recomp + Aurora` runtime. “Crash-solid” here means the concrete
host/FIFO/GPU lifetime defects found in this pass are closed at their owners, malformed contracts
fail before unsafe access, waits are bounded, and representative real runs leave the kernel's
amdgpu timeout/reset/fault count unchanged. It does not mean every possible stage or driver failure
has been exhaustively proven absent.

The historical GPU reset still has no surviving submit timeline. Every source mechanism below is
real and fixed, but none is retroactively declared *the* cause of that old reset.

## Root causes removed

1. **Frame activity was invented instead of observed.** The recomp host ignored the first
   `aurora_begin_frame()` result and initialized its local state to active. A later failed begin also
   allowed guest execution to continue. The next FIFO replay could therefore reach Aurora without
   a matching frame packet. The initial and every subsequent begin result are now authoritative;
   the guest pauses at the seam while WSI is unavailable, and replay without an active packet
   aborts at that boundary. The camera/view extension is emitted before the frame is closed, so
   camera and geometry cannot come from different ticks.
2. **WebGPU startup and WSI teardown were partial-state operations.** A failed backend attempt left
   adapter/device/surface/pipeline flags behind for the next attempt. Present could unconfigure a
   surface while the worker still owned its acquired texture/view. Initialization is now an RAII
   transaction; presentation only atomically invalidates a lost surface, and the main thread
   synchronizes and releases it after in-flight ownership ends. Zero-size/minimized windows are an
   explicit unavailable state rather than input to aspect math.
3. **Asynchronous callbacks outlived the resources they referenced.** Depth-peek and staging map
   callbacks could still arrive during shutdown after their buffers/slots were cleared. Admission
   is now disabled under the callback mutex, every callback is generation/count tracked, shutdown
   waits at most five seconds for quiescence, and only then releases the resources.
4. **Worker backpressure could wait forever.** Queue-full push, synchronization, frame-slot
   acquisition, staging mapping, and staging/GPU slot acquisition now share one five-second fatal
   wait policy. Focused tests force queue and slot exhaustion and require the timeout to fire.
5. **FIFO trust crossed allocation boundaries.** Recomp accepted overflowing or partially consumed
   display lists, incomplete frame tails, unknown commands/VATs, and texture/TLUT spans without
   validating all bytes. Aurora accepted unknown extension commands, indexed draw overruns, invalid
   channel/texgen counts, and indexed-XF reads without a proven source extent. Producer and consumer
   now validate both sides. Array commands carry `upload extent` separately from `backing capacity`,
   allowing auto-sizing while proving the derived read stays inside MEM1. The captured-FIFO player
   was migrated to the same 17-byte protocol and rejects array bases outside its 24 MiB shadow;
   leaving its old 13-byte encoder in place would have desynchronized Aurora's decoder.
6. **Shutdown reported its own destruction as device loss.** Intentional Dawn
   `DeviceLostReason::Destroyed` is now distinguished from a runtime loss, while any validation or
   device-loss callback during initialization makes that attempt fail transactionally.

## Controls and real runs

- `ctest --test-dir build-sms-recomp --output-on-failure`: **17/17 passed** after the renderer and
  scene extractions added their focused shipping-code tests.
- Aurora changed-set gate, excluding three CTest placeholders for targets not built and two
  pre-existing unrelated GX expectation failures: **189/189 passed**. The included death tests
  deliberately exercise unknown commands, indexed overruns, missing capacities, queue saturation,
  and frame-slot timeout; valid controls still decode and preserve ordering.
- Full Clang builds completed for both `sms-recomp` and `sms-boot` after the shared array protocol
  changed.
- Guarded real `recomp + Aurora` runs, each process exit 0 and kernel amdgpu anomaly count
  **42 → 42 (delta 0)**:
  - stage 1, headless, 400 presents;
  - stage 13, headless, 200 presents;
  - stage 24 with interpolated 60 FPS, headless, 400 presents;
  - stage 1, windowed swapchain, 120 presents;
  - stage 1, windowed swapchain, 240 presents.

After final integration, a fresh stage-1 Aurora run completed another 120 headless presents with
process exit 0 and the same validated kernel total, **42 → 42**. A separate gated Native ownership
smoke completed 60 presents and reported no device fault; the exact same boot-wide kernel metric
remained **42** afterward. This is lifecycle coverage for the secondary lane, not a parity claim.

The Wayland desktop did not expose the SDL window to `xdotool`, so an automated real-window
minimize/restore action was not performed. The ordinary visible swapchain path is covered by the
two windowed runs; minimize/restore remains source- and contract-tested rather than claimed as a
completed desktop automation control.

`run-safe.sh` itself had a reporting defect discovered by that control: it printed “headless” as a
literal even for `SB_HEADLESS=0`. The status now derives from the actual selected mode, so a
windowed run cannot be mislabeled as headless evidence.

## Remaining boundaries

Native SDL3-GPU remains a separate, explicit development renderer with full WSI ownership; Aurora
is its offscreen oracle. It is not the default/shipping lane and this entry does not claim Native
visual parity. Decomp + Aurora receives only the shared array-capacity protocol adaptation here;
its standing work remains upstream rebase, expansion, and renaming known `unk*` semantics.

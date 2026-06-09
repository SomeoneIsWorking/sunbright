# Handoff — native VI frame-sync port (2026-06-09)

Pick-up notes for a fresh session. Repo is clean at commit `7c838b0`. Read this, then
`docs/native_threading.md` and the memory index, before re-deriving anything.

## TL;DR state
Boot no longer freezes. The render thread now **wakes via a real VI interrupt** and advances into
the JDrama render path, where it hits a single **wild guest read** (`ea=0x32323502`) inside
`TVideo::waitForRetrace`. Active task: **own the VI frame-sync (the 1/60s frame heartbeat) natively**
so we stop emulating the GC VI hardware interrupt + JDrama retrace bookkeeping.

## What this session landed (committed + pushed)
- `ecd63ac` — *superseded*; was a yield-time-advance hack that deferred the IRQ and starved the render thread.
- `7c838b0` — the real fixes:
  1. **Corrected diagnosis**: the post-audio boot freeze is the **TCardManager** memory-card thread
     (OSCreateThread entry `0x802b3264` → card-mgr loop `0x802b17c8`), NOT audio. Its handler
     busy-loops `CARDProbeEx → __EXIProbe`, gated on a **time-based EXI insertion debounce**. 25b84bd
     misidentified `0x802b3264` as audio.
  2. **Recompiled the real JASystem AudioThread** entry `0x80311170` (discovery missed it — the THIRD
     OSCreateThread code-pointer; added to `kForceEntry`/`kForceCFG` in `tools/recompiler/main.cpp`).
     Under the interpreter the `ttrack_tick_native` override is skipped → the tick spun; recompiled it
     runs on the native stack and the override fires. Killed the audio interp-spin.
  3. **Fixed the native idle/IRQ driver**. ROOT CAUSE: `ct.Idle()+ct.Advance()` does **not** advance
     CoreTiming's global timer outside the JIT loop (verified: ticks delta = 0). Only
     `interp.SingleStep()` does (it consumes downcount). Rewrote `idle_run()` in
     `runtime/dolphin_hook.cpp` to spin an idle `b .` under SingleStep → time advances → VI/DSP/DVD
     events fire → with EE on the interrupt is delivered and vectors into the ISR → run it to
     completion (`interp_run_until`, whose `native_os` intercept routes `OSWakeupThread →
     nthr::make_ready`). `OSYieldThread` (`nthrt_yield_current`) uses this when nothing else is
     runnable ("yield → GC idle thread runs with interrupts on"). Note: `call_ppc` does NOT sync the
     recomp `cpu` into global `ppc` before a native_os override, so the yield path must
     `cpu_to_dolphin_state(*yielder, ppc)` first or the ISR runs on a stale `r1` → wild write.

## Active task: own the VI frame-sync natively (user-approved scope)
The frame heartbeat is standard Nintendo SDK code (documented semantics), not game logic. Replace the
emulated VI retrace interrupt with a native frame clock: native `VIWaitForRetrace` bumps our retrace
counter, runs the post-retrace callback, paces, returns — no HW interrupt, no `OSSleepThread`-on-
retrace, no scheduler IRQ delivery, no JDrama retrace bookkeeping. Keep Dolphin for GPU/present.

### VI globals already mapped (game-specific addresses for SMS GMSE01)
- `retraceCount`  @ **`0x8040E8D0`**  (`r13-0x58f0`; r13 SDA base = `0x804141C0`)
- retrace wait-queue @ **`0x8040E8D8`** (`r13-0x58e8`)
- `VIGetRetraceCount` = `0x803504EC` (just `lwz r3,[0x8040E8D0]; blr`)
- `VIWaitForRetrace`  = `0x8034F684`:
  `old=retraceCount; OSDisableInterrupts(); do OSSleepThread(&queue) while(retraceCount==old); OSRestoreInterrupts()`
- `__VIRetraceHandler` ≈ **`0x8034EF84`** (ISR: bumps count, wakes queue, runs callbacks; **opens with a
  bctr jump table** — the post-retrace **callback global is NOT yet extracted**; this is the one
  missing piece). Crash `ctr=0x8034efac` = `0x8034EF84+0x28`, consistent.
- `TVideo::waitForRetrace` (JDrama wrapper) = `0x802FC9A4`; its loop:
  `do { VIWaitForRetrace(); } while(VIGetRetraceCount()==start)` → (if render mode changed) VIConfigure/
  VISetBlack/VIFlush/VIWaitForRetrace → `VISetNextFrameBuffer` (the swap) → `VISetBlack`.

### Concrete next steps
1. Finish extracting the **post-retrace callback global** from `__VIRetraceHandler` (`0x8034EF84`) —
   find the `bctrl` through the callback pointer and the `OSWakeupThread(&queue@0x8040E8D8)`.
2. Write `runtime/overrides/sms_vi_native.cpp`: native `VIWaitForRetrace` (0x8034F684) +
   `VIGetRetraceCount` (0x803504EC) using a native counter that writes `0x8040E8D0`, invoke the
   post-retrace callback, native frame pacing (immediate under turbo/headless). Register in
   `runtime/overrides/sms_overrides.cpp`. New file → **`cmake -B build` reconfigure** before building
   (CMake GLOB is evaluated at configure time; see memory `build-glob-reconfigure`).
3. Test. **If the `0x32323502` wild read clears** → VI port fixed it, done. **If it persists** → the
   bug is a recomp issue in the JDrama float/framebuffer code, so extend the override up to own
   `TVideo::waitForRetrace` (`0x802FC9A4`) entirely (the "own the render loop" scope).

### ⚠️ Important caveat (don't get fooled)
The wild read is one layer **above** the VI SDK — inside `TVideo::waitForRetrace`'s body
(`0x802FCB28`, doing `OSGetTick`/framebuffer math; `r28=0x43300000` = int→double magic). Owning the VI
SDK heartbeat may or may not fix it (depends on whether the wild pointer comes from bad VI data vs a
recomp FP/framebuffer bug). Verify step 3 honestly; be ready to extend scope to JDrama.

## How to run / measure (always headless)
```
set -a; source .env; set +a   # sets $SUNBRIGHT_ROM
SUNBRIGHT_HEADLESS=1 SUNBRIGHT_TURBO=1 SUNBRIGHT_PROBE=1 SUNBRIGHT_AUTOSTART=1 \
  SUNBRIGHT_RUN_SECONDS=25 timeout -s KILL 40 ./build/sunbright >scratch/logs/run.log 2>&1 &
```
- `SUNBRIGHT_WATCHDOG=0` to disable the freeze-killer while debugging; freeze dumps land in
  `scratch/watchdog/`. `SUNBRIGHT_DBG_IDLE=1` traces the idle driver.
- vps/fps = 0 in headless is a **measurement artifact** (the pure-Dolphin baseline shows it too); use
  the **VI field heartbeat** in the watchdog dump and frame dumps as the real render signal, NOT vps.
- A/B baseline: `SUNBRIGHT_DISABLE_RECOMP=1` (renders the logo cleanly — that's the target boot path).
- gdb attach to the spinning process often fails; prefer the watchdog dump + probe REPL (`/cur`,
  `/r?a=HEX`, `/fn?a=HEX`, `/stack?sp=HEX`).

## Tools built this session (gitignored `scratch/bin/`)
- `scratch/bin/ppcdis.py <addr> [n]` — PPC disasm with operand resolution: `bl`/`b` targets named,
  `lis/addi/ori` and `lwz/stw/lhz/...` resolved to absolute addresses (SDA bases r13=0x804141C0,
  r2=0x80416BA0 seeded). Use this to find globals.
- `scratch/bin/bltargets.py <addr> [n]` — lighter branch-target-only decoder.
- Symbol resolution source: `reference/sms_gmse01_funcs.txt` (sparse — names with large `+0x` offsets
  are unreliable; e.g. it mislabels VI/EXI funcs as `OSGetFontTexture+...`/`DVDSetAutoInvalidation+...`).

## Memory notes added
`idle-coretiming-singlestep`, `card-thread-boot-blocker` (+ the existing `no-spin-detector-bandaid`,
`blocking-call-interp-spin`, `port-not-emulate`, `debug-path-only` all apply).

# Sunbright — PC-Native Port of Super Mario Sunshine (reference/sms + Aurora)

This file holds **hard rules and pointers only**. Session commentary, "FIXED" narratives, and
architecture history live in `debug_journal/` and `docs/`. Every rule below is standing —
don't propose alternatives, don't argue around them.

---

## 🏛️ ARCHITECTURE — ONE RUNTIME (2026-07-07; supersedes every Tier/seam-oracle/Path-B doctrine)

**One binary (`sms-boot`), one runtime, one thread.** The `reference/sms` decomp compiles
native (`SMS_NATIVE_PLATFORM=1` + `SMS_AURORA=1`) and runs on the **process main thread**.
Aurora (`extern/aurora`, SDL3 + WebGPU/Dawn) provides the entire GC platform surface: GX
render, DVD, CARD, PAD/SI, VI, MTX, and host audio out. Aurora is an IO library called from
inside the game's own frame — it never drives game logic.

- **Single thread.** No game `std::thread`, no OSThread emulation, no cooperative scheduler.
  OSThread/mutex/cond SDK calls are no-ops (`sms-boot/runtime/sdk_stubs.cpp`); every
  GC-thread body runs inline at its enqueue site under `SMS_NATIVE_PLATFORM` (setup threads,
  JKRDecomp decode, JKRAramPiece DMA, JAS loaders, card worker). OSMessageQueue is a REAL
  single-threaded ring — a blocking receive on an empty queue OSPanics (it's a guaranteed
  deadlock), it must never silently return. Library-internal threads (SDL audio device,
  Dawn/driver workers) are fine; none may execute game or runtime logic.
- **Frame seam.** The once-per-frame present point is `sb_frame_present(retraces)`
  (`sms-boot/runtime/frame_seam.cpp`), reached from `JDrama::TVideo::waitForRetrace`:
  aurora_end_frame (GX fifo drain + render + present) → event pump → aurora_begin_frame →
  wall-clock pacing to `retraces` NTSC fields (SB_TURBO=1 disables). `VIWaitForRetrace` is a
  pure counter — the game spins on it from load loops; never present there. GX emission and
  fifo drain are same-thread by construction (Aurora's fifo is unsynchronized on purpose).
- **Synchronous unthrottled I/O.** No DVD worker thread, no command queue, no simulated disk
  latency: every DVDRead*/Async completes inline and fires its callback before returning
  (aurora `lib/dolphin/dvd`). SZS decode and ARAM copies run at the enqueue site. Do not
  reintroduce latency-hiding threads or queues — delete the need for them instead.
- **Heap routing at seams.** The main thread is marked as THE game thread (plain `new` →
  JKR heap, `JKRHeap.cpp`). Host-side work inside seams runs under
  `sb_host_alloc_push/pop`: the frame seam gates the whole Aurora frame cycle; aurora DVD
  entry points gate via the weak `aurora_host_alloc_push/pop` hooks. Any NEW seam that
  allocates host C++ memory on the game thread MUST gate the same way (aurora CARD is the
  known un-gated remainder).
- **Audio.** Output = `aurora::audio` (SDL3 audio stream; `extern/aurora/include/aurora/audio.h`),
  pumped once per frame from `sb_audio_frame()` (`sms-boot/runtime/audio_out.cpp`). The JAS
  native mixer (DSP-frame synth) is NOT ported yet — the game is silent by omission; that
  port is the named audio arc and it plugs into this pump.

**Layout:** `sms-boot/{main.cpp, runtime/, shims/, assets/, boot_stubs/}` — see the header
of `sms-boot/CMakeLists.txt`. `shims/` holds RE'd game-logic spec headers included by
`reference/sms`; `boot_stubs/` holds scaffold stubs for unported classes (each one is
porting worklist, replaced as the boot path exercises it).

⛔ Retired and deleted — do not resurrect: the recomp/JIT stack, the Dolphin submodule and
both oracle tiers, the SDL3-GPU Path-B renderer (`render_pc`), the GX-seam capture layer
(`sms-boot/common`), the cooperative scheduler, the "flip" host-layout engine
(`docs/DO_NOT_REVISIT_FLIP.md`). History: `debug_journal/`, memory
`[[aurora-two-path-refactor-2026-07-04]]`, `debug_journal/2026-07-07_one_runtime_consolidation.md`.

## 🏛️ RENDERER DOCTRINE (2026-07-10, decided at user's delegation): Aurora GX-replay stays

Faithfulness/parity via the easier path = the GX-replay architecture, because parity is
only debuggable when intermediate states are comparable to the oracle (FIFO diffs,
per-draw matrix diffs — the tooling that repeatedly localized defects this project could
never have found from pixels alone). A native renderer reimplementation converges to
per-pixel GX emulation anyway at the parity bar while destroying comparability; Path B
and the flip engine already proved the rewrite path harder (both retired). Do not
re-propose it. Narrow license: at genuinely opaque HARDWARE seams (EFB copy mechanics,
XFB present), an understood modern equivalent is acceptable — only if its output matches
the oracle ("faithful outputs over faithful plumbing", seams only, never the GX state
machine).

## 🚫 NO BANDAIDS — RE the intent, port it

For any bug/crash/hang: find WHERE, name the ROOT CAUSE, then reverse-engineer the behavior
and port it properly. No magic constants/offsets to make output line up, no special-casing
the failing input, no swallow/retry/sleep, no commenting-out failing checks, no hardcoded
expected values, no endpoint-colour hand-tuning. If the proper fix is genuinely too big,
name it and let the user decide — never slip a hack in as the fix. Faithful byte-exact
reproduction of GC behavior in Aurora's GX layer is legitimate and encouraged; hand-tuned
output that papers over an RE gap is not.

## 💥 FAIL FAST — parse/contract failures CRASH at the root cause, never return nil

Bad magic, unparseable header, precondition violated, unexpected null, out-of-range value →
CRASH RIGHT THERE (OSPanic/assert) with a message naming the location and dumping the
offending values. **Silent success-shaped stubs are BANNED** (2026-07-10, the JRenderer
incident: 20 no-op stubs + a stale CMake exclusion left every TEV texmap NULL → black
screen mimicking legitimate render defects for days): every stub is either a DOCUMENTED
intentional seam or LOUD — one-time `[STUB-CALLED]` OSReport on first call, OSPanic where
a wrong result corrupts state. CMake `list(FILTER ... EXCLUDE)` entries on reference/sms
sources are part of the same audit surface — an exclusion can keep a later fix dead. Never return nil/0/empty and let it propagate; never no-op an operation
that callers assume happened (the stubbed OSMessageQueue silently skipping ARAM DMA/SZS
decode is the canonical local example — and shader/pipeline compile failures MUST panic,
not skip batches). Guard with `SMS_NATIVE_PLATFORM` so original decomp behavior is
preserved off-platform.

## 🔧 TOOLING / VERIFICATION FIRST

If the harness that would verify a change is missing or broken, FIX/BUILD THE HARNESS FIRST.
Tools must refuse degenerate/empty/stale input loudly. Two verification layers, in order:
1. **Unit test from RE** (spec-derived expected values, hand-derived from disassembly) —
   ships WITH every ported function.
2. **Whole-system checks** (screenshot at a pinned state, boot-log health) — only once the
   subsystem is complete enough to run end-to-end; diffing an incomplete subsystem
   manufactures ambiguous "divergences".
TDD per defect: failing close-test authored from the NAMED defect, red on HEAD, fix, green,
commit test+fix together. `SB_DUMP_FRAME=/path.rgba` (aurora `end_frame`) is the current
frame-capture diagnostic.

## 🧭 Ghidra is the default RE tool

Ghidra 12.0.4 bare from `$PATH` (`analyzeHeadless`, `pyghidra`); GameCubeLoader handles DOLs
natively. Never invoke GUI variants. Prefer the decompiler over hand-disasm; hand-disasm
only to spot-verify a falsified Ghidra result, and say why. Cross-check listing vs
decompiler on lui/addiu sign-extension. Comment-code parity ≠ verification — check against
the decomp source, not your own header comment.

## Build, run, environment

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target sms-boot -j$(nproc)
./run.sh [rom.rvz]        # ROM via $SUNBRIGHT_ROM / .env / rom.rvz drop-in
```

Env vars: `SB_W`/`SB_H` (window), `SB_HEADLESS` (never show the window — REQUIRED for all
automated/diagnostic runs, agents included), `SB_TURBO` (unpaced), `SB_WATCHDOG_SECS`,
`SB_NO_FASTBOOT` / `SB_STAGE` / `SB_SCENARIO` (boot destination, `Application.cpp`),
`SB_DUMP_FRAME` / `SB_DUMP_FRAME_AFTER` (framebuffer dump), `SB_DBG_AUDIO`.
Diagnostic env vars route through these tracked names — prune dead ones on sight.
Keyboard drives pad 0 (`PADSetKeyboardActive`). Kill a stuck run with `timeout -s KILL N`;
the in-process SIGALRM watchdog dumps all-thread backtraces on a stalled frame.
Scratch output → gitignored `scratch/` (never `/tmp`).

## Active target

Boot-order fidelity: GC logo → title (stage 15) → file-select → gameplay, rendered through
Aurora GX. Fix defects in boot order; finish one port end-to-end before switching.

**Title screen status (2026-07-11): the title RENDERS FAITHFULLY.** The multi-session
"washed / orange / blue-diluted / oversized logo" defect was a **diagnostic dump-path
mislabel**, NOT a render bug — `SB_DUMP_FRAME` wrote raw BGRA8 surface bytes but logged them
as "RGBA", so every consumer that read them as RGBA saw red/blue swapped (blue sky → orange,
blue logo → orange). Now FIXED in aurora: the dump normalizes to true RGBA8 on output, so the
file matches its label (see `debug_journal/2026-07-11_dump_bgra_mislabel.md`). **Do NOT
re-open title-color/logo investigations** (J2D bounds, duotone overlays, EFB-snapshot
composites, mBlack bisects, titleDraw state-machine diffs, the "293-vs-1258 sparse scene"
theory) — all were measured off misread dumps and are falsified there. Any future title work
must use the now-default (RGBA8, color-correct) dumps. Minor real residuals (cosmetic, not
blocking): native whole-frame ~15-25 levels brighter than oracle; PRESS START prompt and
"SUNSHINE" word at different animation phase. One outstanding structural note (perf wart, not
parity-blocking): a phase-1 ghost pass double-draws each DrawBuf under stale ortho
(`SB_SKIP_GHOST=1` drops it with bit-identical output).

`GXLoadPosMtxIndx`/`GXLoadNrmMtxIndx3x3` are IMPLEMENTED (2026-07-09) and verified
non-degenerate — do not re-suspect them. The `…Draw SnapTime` nodes in the title are
`TTimeRec` profiling timers, NOT the EFB-copy trigger. NULL-texMap TEV callsite question
(aurora `26d5a7b` emits 0 per GC HW; an uncommitted investigation into WHO sets NULL on
stages 0-7 lives in the user's aurora checkout). Named audio arc: port the JAS DSP-frame
mixer into `sb_audio_frame` (game is silent by omission).

## Skills

`/analyze-rom` · `/build` · `/update-docs` · `/patch-func ADDR` (recomp-era skills are stale
pending cleanup).

## Findings registry

Learned a non-obvious fact worth keeping? Add it to `debug_journal/<date>_<topic>.md` and
commit alongside the code. Dead ends too, not just wins. Fix stale notes when a later
diagnosis supersedes them. Do NOT append session commentary to this file.

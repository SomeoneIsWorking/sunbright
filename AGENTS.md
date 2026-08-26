# Sunbright — PC-Native Port of Super Mario Sunshine (decomp/sms + Aurora)

This file holds **hard rules and pointers only**. Session commentary, "FIXED" narratives, and
architecture history live in `debug_journal/` and `docs/`. Every rule below is standing —
don't propose alternatives, don't argue around them.

---

## 🏛️ ARCHITECTURE — ONE RUNTIME (2026-07-07; supersedes every Tier/seam-oracle/Path-B doctrine)

**One binary (`sms-boot`), one runtime, one thread.** The `decomp/sms` decomp compiles
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
`decomp/sms`; `boot_stubs/` holds scaffold stubs for unported classes (each one is
porting worklist, replaced as the boot path exercises it).

⛔ Retired and deleted: both oracle tiers, the SDL3-GPU Path-B renderer (`render_pc`), the
GX-seam capture layer (`sms-boot/common`), the cooperative scheduler. History:
`debug_journal/`, memory `[[aurora-two-path-refactor-2026-07-04]]`,
`debug_journal/2026-07-07_one_runtime_consolidation.md`.

**Dolphin is NOT retired** — this line used to say it was, and the tree disagreed.
`extern/dolphin_fork` (SomeoneIsWorking/dolphin@sunbright) is live and is the pixel/FIFO
oracle: it carries a draw-log hook in `VertexManagerBase::Flush` and the `--fifo-record`
NoGUI flag, used by `tools/oracle/record_fifo.sh` and `capture.sh`. (That hook's env switch
lives in the FORK's source, not this repo, so it is deliberately not named here — the commit
gate scans only our tree and would report any such name as a phantom, correctly.) What was
retired in the 2026-07-07 consolidation was the in-process oracle TIERS, not the emulator.
`extern/dolphin` (pinned upstream, never initialised, 0 bytes) is registered in `.gitmodules`
and used by nothing — it is only a trap: `capture.sh` looked for its binary there, never found
it, and silently fell through to a stock system Dolphin that has none of the hooks.

## 🏛️ TWO RUNTIMES (2026-07-21, decided at user's delegation; SUPERSEDES "NATIVE-ONLY, NO RECOMP")

The 2026-07-15 "no recomp, none will be reintroduced" directive is **REMOVED**. Two runtimes
are now first-class, side by side:

1. **decomp + Aurora** (`decomp/sms` + `extern/aurora`) — the existing native runtime. Still the
   moddable end-state and the **verification oracle**: it renders title, file-select and Delfino
   correctly, so recomp output can be diffed against it in-process, per function.
2. **recomp + native overrides** — the game's real PPC code, statically recompiled, with native
   overrides at the HW/OS seams. Runs the WHOLE game without hand-porting every actor.

**What was actually intractable was the FLIP ENGINE, not recompilation.** The retired hybrid had
recomp'd PPC calling into *native decomp objects*, needing per-field marshaling between guest
layout (32-bit BE, guest vtables) and host layout (LP64, LE, native vtables). That boundary is
structurally impossible and stays banned — **do not reintroduce recomp↔decomp interop.** A
STANDALONE recomp has no such boundary: guest layout end-to-end, overrides only at narrow
HW/OS APIs (GX, DVD, PAD, VI, audio). That is the N64Recomp / Zelda64Recompiled model.

**The old rule's supporting claims were wrong and must not be repeated:**
- It said recomp was retired "because emitting flip-backed code was intractable". The RECOMPILER
  worked — retired 2026-06-18 by directive ("I don't want a recomp") while live in Delfino at
  ~2.16M recomp calls/sec. Only the flip engine failed.
- It said native-only is "REQUIRED for interpolated 60fps". Overstated: the recomp era
  interpolated via `runtime/interp60.h` (DELETED; recoverable from git at `9283f44^`) by capturing `J3DModel::viewCalc` matrices.

**Any recomp work that relies on guest code must also go through the decomp path.** A recomp-only
result is not complete when it depends on game-owned guest behavior: carry the corresponding
behavior, naming, evidence, or implementation through `decomp/sms` in the same work. Recomp-only
ownership remains appropriate for the recompiler, guest runtime substrate, host application,
hardware/OS seams, and renderer machinery that does not reimplement game logic. This does not
permit recomp↔decomp object interop; the two runtimes keep their layouts separate.

**Resurrect, do not rebuild.** In git at `9283f44^`: `tools/recompiler/` (188 `case PPCOp::`
emitter cases, 41 `ps_*` mnemonics, `PSQ_L/LU/ST/LX/STX` with GQR dequantization),
`runtime/native_threads.cpp` (DELETED; recoverable from git at `9283f44^`) (431 lines; interrupt delivery already fully PC-native, a behaviour
port of OSInterrupt.c), and 74 files of `runtime/overrides/` (ngx renderer, native JAS audio, EFB,
matrix, widescreen, the interp60 stack). The genuinely NEW work is swapping **Dolphin's** substrate
(JIT dispatch, MMIO, interrupt sources) for **aurora's**, which did not exist maturely in June.
`DolRecomp` (GPL-3.0, GameCube-native, 236 opcodes) is a **cross-check / gap-filler**, not the base.

## Host application and UI structure

Follow Dusklight's ownership split in the primary recomp runtime: `sms-recomp/app/` owns typed,
persisted application policy; `sms-recomp/ui/` owns RmlUi documents and event lifetimes; engine and
override seams consume the typed policy and never query UI elements. `res/rml/` owns styles and
`res/LICENSES/` owns third-party asset notices. `sms-recomp/host/main.cpp` only composes those
modules. The in-game settings window follows Dusklight's `Document → Window → SettingsMenu` split;
Escape is routed once from Aurora's SDL event array, and `ui::Runtime` owns the modal pause loop.
Keep renderer and frame-rate semantics authoritative in `app/`, not duplicated between the menu,
launcher scripts, and frame seam. Current behavior and gaps: `docs/app/settings.md`.

The decomp side is unchanged: gaps I hand-port are **decomp gaps** — finite, not infinite. The
accelerator is **syncing upstream `doldecomp/sms`** so community-filled bodies land for free (see
UPSTREAM SYNC), plus RE tooling (`tools/re/port_dossier.py`). Rendering-affecting decomp code is
always native; when a native port faithfully reproduces a retail overflow/UB that is benign on PPC
but corrupts on host (e.g. a 4x4 write into a 3x4 buffer), adapt to produce the same OBSERVABLE
result without the host corruption, documented as such.

## 🏛️ RENDERER DOCTRINE (2026-07-23, USER-DIRECTED reversal): the recomp gets its OWN native SDL3-GPU renderer

The 2026-07-10 "Aurora GX-replay stays, do NOT re-propose a native renderer" doctrine is
**REMOVED by the user** (its author). The recomp render is to be **fully under our
control — no Aurora, no Dolphin, a pure PC render on the SDL3 GPU API** — for the same
anti-black-box reason that runs through the port: not debugging rendering through a
third-party GX interpreter.

**Resurrect, don't rebuild.** The retired SDL3-GPU Path-B renderer is in git at
`9283f44^:native/render/gx_sdlgpu.cpp` (reached P3 = real per-material TEV combiners). Its
BACKEND (batches → GPU) resurrects; its FRONTEND (GX state → transformed verts + TEV
shaders) was built for the decomp runtime's GX-call capture and must instead be driven from
the recomp's FIFO parse (`sms-recomp/runtime/devices/dev_gxfifo.cpp`) — the job aurora's
`command_processor.cpp` does today.

**Aurora is the PARITY ORACLE during the build, not deleted early.** Build the native path
ALONGSIDE aurora (both consume the parsed stream), diff every frame, delete aurora only at
parity. The old doctrine's ONE correct point stands: parity is only debuggable when
intermediate state is comparable to a known-good — so keep the oracle until the native path
matches it. This IS the Path-B direction the old doctrine retired as harder; it is large
(reimplementing the GX fixed-function pipeline), accepted with eyes open. Milestone ladder:
device+clear+present → pass-through geom → vertex transform → TEV → textures → EFB, each
A/B'd against aurora. Details: memory `[[native-sdl3gpu-render-pivot-2026-07-23]]`.

## 🔄 UPSTREAM SYNC — rebase ~2x/week, and CONVERGE (don't fork)

Upstream `doldecomp/sms` moves fast and is actively implementing the same actors we
need (DebuTelesa, AreaCylinder, MapObjTree, GateKeeper, Butterfly… were all done
upstream while we hand-ported them). **Check upstream before hand-porting a gap.**

Use `python3 tools/re/rebase_upstream.py` — `status` → `rebase` → `audit` (loop until
green) → `converge`. Hard-won rules:

- **The decomp development loop is rebase → rename known unknowns → expand.** Rebase first so
  upstream implementations are not hand-ported twice; then replace `unk*` names where the
  field/function semantics are already established by use sites or binary evidence; then extend
  the remaining real gaps from binary evidence. A green rebase is synchronization, not completion
  of the decomp lane.

- **Resolve FILE-level, never hunk-level, and move header+cpp TOGETHER.** A class whose
  `.hpp` and `.cpp` come from different sides will not build. Hunk-merging produces
  internally-inconsistent TUs.
- Two breakage classes only: **duplicate decls** (both sides added the same member →
  delete OUR dup, keep upstream's) and **API drift** (upstream renamed something our
  `.cpp` still calls → adapt our call site, or adopt upstream's pair).
- **Prefer CONVERGING to upstream.** Every file we keep our own version of is a conflict
  we pay for again in 3 days. Keep native deltas minimal, `#ifdef SMS_NATIVE_PLATFORM`,
  and additive so git auto-merges them. Adopt upstream's version whenever it is
  equal-or-better (it usually is — it's a matched decomp).
- Keep ours ONLY where a real native fix lives (LP64/BE/guards). `rebase_upstream.py
  diverge unmarked` lists the files with no native marker — that count is the debt to
  drive down (currently ~386 of 566).
- **Re-run convergence AFTER every rebase.** Adopting a file by content in a commit means
  the next rebase replays that stale snapshot over newer upstream (this silently reverted
  upstream's newer `Particles.hpp` once).
- A build-green convergence is NOT proof: a file can compile yet drop a native LP64/BE fix
  that only shows at runtime. Runtime-verify before trusting a big convergence batch.

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
a wrong result corrupts state. CMake `list(FILTER ... EXCLUDE)` entries on decomp/sms
sources are part of the same audit surface — an exclusion can keep a later fix dead. Never return nil/0/empty and let it propagate; never no-op an operation
that callers assume happened (the stubbed OSMessageQueue silently skipping ARAM DMA/SZS
decode is the canonical local example — and shader/pipeline compile failures MUST panic,
not skip batches). Guard with `SMS_NATIVE_PLATFORM` so original decomp behavior is
preserved off-platform.

## 🔬 EVERY COMPARISON INSTRUMENT NEEDS A CONTROL THAT CAN FAIL

The single most expensive failure mode in this project is not a wrong fix — it is an instrument
that compares two things which are not the same quantity, and reports the difference as a finding.
It has happened SIX times (see `debug_journal/2026-07-23_native_texgen_and_texmap_bisect.md`):

- draw ORDINALS joined across two instruments that count different populations (twice);
- aurora's SDK `texObj` slot compared against our BP image base — 133 "disagreements", really 0;
- an 8192-entry event ring reporting its own WRAP as "no writes found";
- a 512-byte prefix sampled from an 8192-byte texture, reported as "never written";
- the capture-list ordinal used to order EFB copies, collapsing them all to end-of-frame.

Each looked like a result. Each cost hours to days. So, before trusting ANY instrument that
compares, diffs, attributes or scores:

1. **State what BOTH sides actually measure**, in the code, at the point of comparison. If one side
   reads an SDK slot and the other a hardware register, they are not comparable no matter how well
   the numbers correlate.
2. **Give it a control that MUST fail visibly.** A no-op variant that has to reproduce the baseline
   exactly; an empty input that has to read as empty; a known-positive that has to be detected. If
   the instrument cannot tell you it is broken, its "no signal" and its "broken" are the same
   output. `SBR_BLACK_OWNER` asserts both ends before bisecting; the ablation sweep carries a
   `control:no-op` that must score +0.0 — copy that shape.
3. **Pair on something both sides genuinely share.** Stream offsets, addresses, stable keys — never
   an ordinal that each side counts for itself.
4. **Never compare aggregates taken at different sample counts.** The A/B mean drifts several
   points with frame COUNT alone; use the `COMPARABLE @ N=` line, not the running mean.

5. **Make the instrument declare what it does NOT cover.** A control proves the instrument works on
   the fields it compares; it says nothing about a field that was never wired. The cull defect hid
   for this whole arc behind exactly that — `state_oracle.h` documented cull bits, neither side ever
   wrote them, and "raster 0 of 29283 disagree" read as coverage while covering nothing. The state
   oracle now self-reports (`coverage:` line): any field CONSTANT across every draw on either side
   is flagged `CONSTANT-ON-OURS` / `CONSTANT-ON-AURORA` / `CONSTANT-ON-BOTH(unwired?)`, because a
   constant field is either genuinely constant in the scene or not plumbed, and the instrument
   cannot tell you which. Validated by deliberately breaking one field and confirming it is flagged.

A finding from an instrument with no control is a hypothesis, and must be labelled one. An
instrument that cannot say what it fails to cover will eventually be trusted for something it never
measured.

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
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target sms-boot -j$(nproc)
./run.sh [--rom rom.rvz]  # ROM via $SUNBRIGHT_ROM / .env / rom.rvz drop-in
./run-safe.sh SBR_STAGE=1 SBR_QUIT_AFTER=400   # PREFER THIS for any automated/diagnostic run
```

**Debug is a playable configuration, not an `-O0` diagnostic mode.**
`cmake/SunbrightBuildPolicy.cmake` keeps assertions and symbols while compiling Debug at `-O2`;
the generated guest retains line tables and function symbols without materializing debug records
for millions of machine-generated locals. Both launchers configure Debug by default. Aurora GPU
validation, robustness, and labels are selected explicitly by `AURORA_GPU_DIAGNOSTICS`, never by
`NDEBUG`, so Release remains an optional packaging profile rather than the only fast path.

**Use `./run-safe.sh` rather than assembling a command line.** Every run that made this machine
unusable was hand-assembled, and the common ingredient was `SB_TURBO=1` with nothing bounding the
present rate. It picks a 60 Hz submission ceiling, headless, no native renderer and a wall-clock
cap, and then reads the KERNEL's opinion of the run: it counts amdgpu ring timeouts and resets
across the run and exits non-zero if the run disturbed the card, even when the game exits cleanly.
That check is external to the process being checked, and it is validated against both classes
(instrument I022) — 108 on the session that reset the card, 0 on an idle window, UNKNOWN (never 0)
when the log cannot be read.

Env vars: `SB_W`/`SB_H` (window), `SB_HEADLESS` (never show the window — REQUIRED for all
automated/diagnostic runs, agents included), `SB_TURBO` (unpaced), `SB_WATCHDOG_SECS`,
`SB_NO_FASTBOOT` / `SB_STAGE` / `SB_SCENARIO` (boot destination, `Application.cpp`),
`SB_DUMP_FRAME` / `SB_DUMP_FRAME_AFTER` (framebuffer dump), `SB_DBG_AUDIO`.
**Enable the repo's commit gate once per clone: `git config core.hooksPath .githooks`.** It runs
`tools/diag_registry.py check` (~0.6s), which fails a commit when the generated switch registry is
stale or when a switch named in AGENTS.md / `docs/` / a run script is read by NO code — the defect
that had `SB_SKIP_GHOST` cited in these instructions while absent from the source. A deliberate
always-loud stderr write is marked `LOGGER-EXEMPT` in a comment BESIDE the print.

**Reproducing a native-renderer frame: use `./run-render.sh`.** The renderer needs six env vars set
together and omitting any one fails silently and plausibly (no native render, or 0 drawables, or an
untextured frame, or an empty scene because plain fastboot derives a non-rendering episode from the
save). Compare runs ONLY on the harness's `=== COMPARABLE @ N=... ===` line — the running mean
drifts several points with frame COUNT alone, so means taken at different N are not comparable.

**Diagnostic logging goes through `SB_LOG=<chan>[,...]` (`all`, `list` to discover) — the ONE
tracked channel registry (`sms-boot/shims/sb_log.h`, `SB_LOGC`/`SB_LOG_ONCE`/`SB_LOG_EVERY`).
Never add ad-hoc `getenv("SB_DBG_*")+fprintf` diagnostics; convert stragglers on sight and
prune dead channels.** Non-logging behavior toggles (dump paths, pins like `SB_PIN_STATE`)
stay as their own tracked env vars — prune dead ones on sight.
Keyboard drives pad 0 (`PADSetKeyboardActive`). Kill a stuck run with `timeout -s KILL N`;
the in-process SIGALRM watchdog dumps all-thread backtraces on a stalled frame.
Scratch output → gitignored `scratch/` (never `/tmp`). To clear a scratch dir, use
`python3 tools/scratch_clean.py <scratch-dir> [--glob '*.png']` (never `rm -rf` — the
harness gates it; the tool refuses paths outside `scratch/`). Use it in subagent prompts too.

## Active target

Boot-order fidelity: GC logo → title (stage 15) → file-select → gameplay, rendered through
Aurora GX. Fix defects in boot order; finish one port end-to-end before switching.

**Recomp status (2026-07-22): title, file-select AND Delfino Plaza all render.** File-select
renders correctly — do NOT re-open "sea wash" investigations (the measurement rect sat on the
white SURF LINE and the "oracle" was the TITLE SCREEN; see
`debug_journal/2026-07-22_wash_attribution_harness.md`). The residual there was FIXED:
`GXTexObj_::has_mips()` gated on aurora's `flags` bit 0, which only the SDK path sets, so every
mipmapped texture bound single-level; it now derives from TexMode0's min-filter field, the
hardware's own rule.

**Loading a save reaching the plaza is FIXED (2026-07-22).** "Back to the title screen" is the
stage-load failure path (`TApplication::proc` turns setup failure into `APP_STATE_DONE` =
`mNextArea.set(15,0,0)`). Root cause: SPRs were per-`CPUState`, so `LCEnable()`'s HID2 bit was
invisible to the THP decode thread and the codec returned "locked cache not enabled" — failing
`loadResource`'s `if (mMap == 1) thpInit()`, which is Delfino-only. SPRs are now machine-wide.
**Any machine-scope state modelled per-CPUState is invisible across threads** and will look like
an unrelated subsystem "not being ported". THP decoding WORKS; only session teardown+reopen does
not (`SBR_THP=stage|all|none`, default stage). `SBR_FASTBOOT=1` / `SBR_STAGE=<n>` boot straight
into a stage — use it, it makes gameplay bugs reproducible without driving the menus. Details:
`debug_journal/2026-07-22_recomp_delfino_spr_locked_cache.md`.

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
blocking): PRESS START prompt and "SUNSHINE" word at different animation phase; seagulls
missing at settle. The old "~15-25 levels brighter" residual is RESOLVED (2026-07-14): it
was the phantom mBlack decomp constant (debug_journal/2026-07-14_blocky_letters_mblack.md);
settled whole-frame means now within 3 levels of the boot oracle (143,178,204 vs 146,178,201).
**The "phase-1 ghost pass" double-draw does NOT exist in the recomp** (measured 2026-08-05:
per-pass command counts on Delfino are `2 10 75 976 331` — one dominant scene pass, nothing
drawn twice). Do not blame it for frame cost or for the 22 MB/tick storage. `SB_SKIP_GHOST` is
LIVE in the decomp (`MarDirectorDirect.cpp`, skips `mPerformListDrawBufGroup`) — the earlier
"phantom, read by no code" note was wrong and is corrected here.

`GXLoadPosMtxIndx`/`GXLoadNrmMtxIndx3x3` are IMPLEMENTED (2026-07-09) and verified
non-degenerate — do not re-suspect them. The `…Draw SnapTime` nodes in the title are
`TTimeRec` profiling timers, NOT the EFB-copy trigger. NULL-texMap TEV callsite question
(aurora `26d5a7b` emits 0 per GC HW; an uncommitted investigation into WHO sets NULL on
stages 0-7 lives in the user's aurora checkout). Named audio arc: port the JAS DSP-frame
mixer into `sb_audio_frame` (game is silent by omission).

## Skills

`/analyze-rom` · `/build` · `/update-docs` · `/patch-func ADDR` (recomp-era skills are stale
pending cleanup).

## Graphics registry

`docs/graphics/graphics_db.tsv` — every graphic the port has been OBSERVED to draw, whether it has
been RE'd, and whether it interpolates. The GAME writes it: any emitter that draws gets a row
automatically. Playing populates it; `tools/gfx/graphics_db.py next` is the worklist ("work on the
DB entries"). `re`/`note` are curated, everything else is measured — read `docs/graphics/README.md`
before trusting a column, especially `unmeasured` (audit was off) vs `no`.

## Codemap

`docs/codemap.md` — what is where, what's done, what's missing. Consult at task START;
update in the SAME commit that lands/changes a subsystem.

## Findings registry

Learned a non-obvious fact worth keeping? Add it to `debug_journal/<date>_<topic>.md` and
commit alongside the code. Dead ends too, not just wins. Fix stale notes when a later
diagnosis supersedes them. Do NOT append session commentary to this file.

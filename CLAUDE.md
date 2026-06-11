# Sunbright — GameCube Static Recompiler

## 🛑 THE ONLY ALLOWED DEBUGGING PATH (user directive — never deviate, never propose alternatives)
When you face a **bug / crash / hang**, follow this exact path. The user is tired of
reiterating it — do NOT propose any other kind of solution (no env-gated fallback "to keep it
working", no force_jit-as-fix, no skipping the function, no magic constant, no bandaid):

1. **Find where** the bug/crash/hang happens.
2. **Find its root cause** (name it — symptom gone ≠ fixed).
3. Then, depending on what the root cause is:
   - **Cause = behavior that wasn't recompiled and is running under Dolphin fallback** →
     **Fix the recompiler so it stops missing that function** (recompile it).
   - **Cause = a CONCRETELY IDENTIFIED mistranslation in the recompiler** → **Fix the
     recompiler, AND add a test** so that exact problem can't regress. This branch requires
     pointing at the specific emitted code and saying "this instruction/pattern was translated
     wrong; it should be X instead." A specific, named defect — not a deduction.
   - **Cause = some other behavior, OR a recomp misbehavior you canNOT pin to a specific
     mistranslation** → **Reverse-engineer that behavior, port it to a PC-native override, and
     fix it there (own the path).**

**"JIT works but recomp doesn't" is NOT a recompiler-fix trigger.** Neither is "the generated C
reads as a faithful translation yet behaves differently / the mechanism is unexplained." Those
are *deductions* that a recomp bug exists, not an *identified* defect — do NOT go hunting the
recompiler on that basis. They fall to the **own-it-natively** path. Only touch the recompiler
when you've DIRECTLY found the wrong translation (e.g. "this opcode emits `a` but PPC semantics
are `b`"). When in doubt, own it natively.

`force_jit` / `SUNBRIGHT_FORCE_JIT` / interpreter fallback are **diagnostics for bisection
only**, NEVER the fix. The fix is always one of the branches above. This rule overrides
any instinct to stabilize by routing around a problem.

**NO BANDAIDS — ALWAYS REVERSE-ENGINEER, PORT, AND FIX IN THE PORT.** No magic constant/offset
to make output line up, no special-casing the failing input, no try/except-swallow, no
retry/sleep-to-fix-a-race, no commenting-out a failing check, no hardcoded expected value, no
"temporary" workaround. When the root cause is a game behavior, the fix lives in a faithful
PC-native port of that behavior — understand what the original does, port it, fix it there.
Symptom-gone ≠ fixed. If a real fix is genuinely too big right now, do NOT silently patch —
name the proper fix and let the user decide; never slip a hack in as if it were a fix.

## What this project is
Static recompiler for Super Mario Sunshine (GameCube/PowerPC) → native PC binary.
ROM: provided via `$SUNBRIGHT_ROM` (set it in a gitignored `.env` next to `run.sh`, or drop a
`rom.rvz` in the repo dir). No machine-specific ROM path is committed.

Pipeline: RVZ → extract DOL → decode PowerPC → emit C → compile → native .so
Runtime: Dolphin subsystems (GFX/DSP/Memory/Input) drive the native code via hook layer.

## Architecture overview

```
tools/recompiler/    Offline tool: ROM → generated/functions.cpp + jump_table.cpp
runtime/             Dolphin integration layer (JIT hook, memory bridge, OS HLE)
runtime/overrides/   Hand-written native overrides + manual JIT routing
generated/           Recompiler output — gitignored, regenerate with /recompile
                     functions.h (decls) + functions_<addr>.cpp ×N (bucketed by
                     address, compiled in parallel) + jump_table.cpp
externals/dolphin/   Dolphin git submodule — do NOT modify
docs/                Living docs — update whenever architecture changes
```

## How to self-update this file
Self-evolving workflow is global (`<home>/.claude/CLAUDE.md`). Here specifically: update the
relevant section below + `docs/`, then run `/update-docs` to sync.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON
cmake --build build -j$(nproc)
```

Or use `/build` skill.

## Running the game

```bash
./run.sh [rom.rvz]          # ← use this; sets the working video env
```
`run.sh` pins `SDL_VIDEODRIVER=x11` and defaults to **Vulkan**, 3× internal
resolution, 16:9 widescreen. The window opens at the output aspect so the game fills
it (F11 toggles fullscreen). Running `./build/sunbright` directly on a
**native Wayland** session fails with "failed to initialize video backend" — Dolphin's
GL/Vulkan can't make a surface on a native Wayland window; XWayland (x11 driver) works
for **both OGL and Vulkan**. ROM comes from `$SUNBRIGHT_ROM` / `.env` / a drop-in `rom.rvz`.

Keyboard → GC pad (window focused): Enter=Start, Z=A(jump), X=B, C=X, V=Y, Q=Z, A=L,
S=R(spray), arrows=control stick, F11=fullscreen.

`sunbright` is a **single self-contained binary** — launcher, runtime bridge,
hand-written overrides, and the statically-recompiled game code are all linked in.
No shared library, no dlopen. (An optional 2nd arg is accepted but ignored; the
recomp table is linked directly via `generated/jump_table.cpp`.)

Useful env vars:
- `SUNBRIGHT_BACKEND=OGL|Vulkan|Software` — GFX backend (both OGL & Vulkan work under XWayland; Vulkan is the default)
- `SUNBRIGHT_RES_SCALE=N` — internal resolution multiplier (default 3 = 3× native EFB)
- `SUNBRIGHT_WIDESCREEN=0` — disable 16:9 (default on: `GFX_ASPECT_RATIO=ForceWide` + `GFX_WIDESCREEN_HACK`; the hack widens the 3D projection so 4:3 SMS isn't stretched)
- `SUNBRIGHT_DISABLE_RECOMP=1` — force everything through Dolphin's JIT (A/B control)
- `SUNBRIGHT_TRACE=1` — log every recompiled function entry (very verbose)
- `SUNBRIGHT_PROBE=1` — built-in HTTP/JSON perf probe + **interactive REPL** at
  `http://127.0.0.1:17654` (`SUNBRIGHT_PROBE_PORT` overrides). `curl` it to read live emulation speed
  (Dolphin `GetSpeed/VPS/FPS`), the recomp-vs-interpreter call mix, interpreter step rate + its share
  of wall time (`interp_wall_frac`), poll-yields. This is how you measure **headlessly** — run the
  game in the background and probe it (`runtime/probe_server.cpp`).
  **REPL endpoints (prefer these over adding env-gated `fprintf` logs + rebuilds):**
  `/metrics` (perf JSON) · `/r?a=HEX&n=N` (read N guest words) · `/fn?a=HEX` (addr→nearest function
  name, from `reference/sms_gmse01_funcs.txt`) · `/stack?sp=HEX` (walk a guest back-chain, each LR
  named) · `/cur` (current OSThread + saved srr0/lr/sp/prio) · `/help`. Walk the GC active-thread
  queue with `/r?a=800000d8` (head@DC, tail@E0, current@E4; OSThread.link.next @+0x2FC), then inspect
  each thread's context (`+0x198`=srr0, `+0x84`=lr, `+0x4`=sp, `+0x2c8`=state, `+0x2d0`=prio).
Graphics config is applied in `main_sdl.cpp` via `Config::SetBase` (GFX_EFB_SCALE,
GFX_ASPECT_RATIO, GFX_WIDESCREEN_HACK) before boot.

### Performance gotcha — stale persisted frame-dump config (was THE "too slow" cause)
Dolphin **persists** the frame-dump flags to `<home>/.config/dolphin-emu/` (`DumpFrames` in
`Dolphin.ini`, `DumpFramesAsImages` in `GFX.ini`). One past `SUNBRIGHT_DUMP=1` run leaves
`DumpFrames=True` on disk, so EVERY later run (incl. `run.sh`) silently re-enables the FrameDumper
— it PNG-encodes every frame on its own thread at ~95% CPU and throttles the whole emulator to
~0.15× real-time, recomp thread idle at ~5%. `main_sdl.cpp` now sets both flags **explicitly each
run** from `SUNBRIGHT_DUMP` (off by default). If a run is mysteriously slow, check per-thread CPU
(`top -H -p <pid>`): a hot `FrameDumper` = this. Dump-off headless speed is ~3.7× real-time
(uncapped); the memory-bridge RAM path is inlined (`intrinsics.h` `sb_r*/sb_w*` — host load+bswap,
not a function call), so the recomp thread (not the dumper) is the active worker.

F11 toggles fullscreen. X11 and Wayland both work (SDL2 auto-detects).
Kill a stuck run with `timeout -s KILL N` — our clean-shutdown path can hang.

## JIT hook (no Dolphin patches required)

`runtime/jit_hook.cpp` intercepts via linker `--wrap` on `_Z13JitTrampolineR7JitBasej`:
```
JitAsm → __wrap_JitTrampoline (our hook)
           ├─ IsRecompiled? → SunbrightBridge::Run() → recompiled func (in-binary)
           └─ else         → __real_JitTrampoline  → Dolphin JIT
```

## Call model (single path — recomp→recomp stays on the native C stack)
There is **one** call model (the old flag-gated "classic" return-bounce model is gone).
The emitter renders `bl`/`bctrl` as an inline C call that *continues* after the callee
returns, `blr` as a C `return`, and `b`/`bctr` that leave a function as `tail_ppc`. So
recomp→recomp calls and returns stay on the native C stack — no JIT dispatcher
round-trip, no per-call register-file copy. Implemented in `runtime/dolphin_hook.cpp`:
  - `call_ppc` (bl/bctrl): recomp target → nested C call; non-recomp target →
    run it under the Dolphin **interpreter** (`SingleStep`, NOT `SingleStepInner`, so
    CoreTiming advances and HW-wait/spin loops progress) until control returns to
    `cpu.lr`, then continue the caller inline. **Return detection is stack-aware**
    (`interp_run_until(ret, budget, sp_floor)`): it stops only when `pc == ret` AND the
    guest stack has unwound to the caller's SP (`gpr[1] >= sp_floor`). A bare `pc == ret`
    match is ambiguous when the interpreted callee **re-enters that same address** — e.g. a
    recursive callee whose own post-call continuation IS `ret` (`__construct_array`
    `0x80337f78`): it would stop EARLY mid-call with a half-unwound register file, leaking
    the callee's loop registers back to the caller (the `TBeamManager`-ctor crash: a valid
    `this` came back as ≈5 → wild write). The `sp_floor` floor rejects the deeper nested
    re-entries. ISR/syscall/idle/thread-body callers pass `sp_floor=0` (bare-PC, unchanged).
  - `tail_ppc` (b/bctr that leave the function): recomp target → nested call then the
    caller returns; non-recomp target → commit state and `siglongjmp` back to
    `SunbrightBridge::Run`, unwinding the recomp C stack and letting the CPU loop take
    over. This is what makes the **boot→main-loop handoff** (a never-returning tail
    branch into OS/runtime code) work — running it synchronously under the interpreter
    instead would try to run the whole game inside `SingleStep` and spin. `Run` installs
    the longjmp target via `sunbright_set_tail_jmp`.
  - `cpu_to_dolphin_state` writes back XER[SO]/OV too (needed for the C return).
  - **Context-switch handoff (`OS_LOAD_CONTEXT` 0x80343fe4):** the GC scheduler's `OSLoadContext`
    (JIT-only) rfi's into another thread and never returns to its caller. `call_ppc` detects this
    target and does the same `siglongjmp` handoff as `tail_ppc` — letting **Dolphin's CPU loop run
    the GC threading** (the hybrid: Dolphin owns OS/threading, recomp owns game logic) instead of
    spinning it under `run_jit_sync`. This cleared the audio-init boot stall. (Reimplementing the
    GC scheduler natively was tried and abandoned — wrong layer; see `docs/native_threading.md`.)

Status: boots reliably (0/16 runs crash) and renders correctly at **full coverage**
(13460 funcs, CFG + pointer-discovery). The throughput win is in CPU-bound scenes (the
title screen is vsync-capped). `/recompile` always emits this model.

### Interrupt-delivery hazard (fixed) — do not regress
Recomp runs on the native C stack and **never consults `ppc.pc` mid-tree**, so an async
interrupt (exception redirect) can only be delivered cleanly at a recomp→JIT boundary
(top-level `Run` return / `tail_ppc` handoff). Two boot crashes came from violating
this; both were intermittent (~timing/race) at full coverage:
  1. **OS interrupt primitives single-stepped.** `OSDisableInterrupts` (0x803458ac),
     `OSEnableInterrupts` (0x803458c0), `OSRestoreInterrupts` (0x803458d4) are
     `mtmsr`-based → JIT-only (`function_needs_jit`), so `call_ppc` ran them via
     `run_jit_sync` = interpreter `SingleStep`, which checks/delivers external
     exceptions **every step** → an interrupt could land *inside* `OSDisableInterrupts`
     (after `mfmsr` reads EE=1, before `mtmsr` clears it), diverting to the exception
     vector mid-critical-section → state corruption → wild fastmem fault. OS code wraps
     every critical section in these (3441 calls each during boot). Fix: native
     overrides in `runtime/overrides/sms_os_intr.cpp` that toggle `MSR[EE]` (0x8000) via
     `msr_set_raw` (sets MSR + `MSRUpdated()`, **no** synchronous `CheckExceptions()` —
     delivery deferred to the JIT boundary). Disable never delivers; enable/restore
     defer. Lesson: any `mtmsr`/MSR-EE primitive must NOT be single-stepped under
     `run_jit_sync`.
  2. **main↔EmuThread guest-memory race.** `widescreen_patch_tick`/`movie_skip_tick`
     poke guest RAM via `GetPointerForRange` every frame on the SDL/main thread; while
     the EmuThread is in `MemoryManager::Init()` (`State::Starting`) the arena base is
     being (re)built → torn/stale host pointer → wild write. Fix: gate all main-thread
     guest-memory pokes on `g_core_running` (`State::Running`) in `main_sdl.cpp`.

## Hybrid execution: what runs where
The recompiler deliberately leaves some functions to Dolphin's JIT — `function_needs_jit()`
in `tools/recompiler/main.cpp` drops any function that writes MSR (`mtmsr`/`rfi`),
touches the MMU/segments/TLB, or does `mtspr`/`mfspr` to a **hardware SPR** (HID0/HID2,
L2CR, WPAR, BATs…). Those carry side effects (cache flush, gather-pipe reset, the
WPAR/L2CR status bits OS code polls) that only Dolphin reproduces; running them in
recomp causes boot to spin. `mfmsr` is OK in recomp — it reads the live MSR via
`msr_get()`. SPRs we *do* model (LR/CTR/XER/GQR) stay in recomp.

## Memory bridge (`runtime/memory_bridge.cpp`)
- Main RAM (0x8xxxxxxx / 0xCxxxxxxx mirrors, low 24 MB): fast raw pointer + byteswap.
- Everything else (MMIO: VI/PE/DSP/SI/CP/EXI): routed through Dolphin's
  `Read_U*`/`Write_U*` so hardware register handlers run.
- Write-gather pipe (0xCC008000): routed to `GPFifo::Write*` — this is how GX
  display-list commands reach the GPU FIFO. Must NOT go through generic Write_U32.

## Overrides & manual JIT routing (`runtime/overrides/`)
Runtime escape hatches that take precedence over generated code (no regen needed):
- `SUNBRIGHT_OVERRIDE(name, addr) { ... }` — hand-written native replacement for a func.
- `force_jit(addr)` / `force_jit_range(lo, hi)` — route a stubborn function to Dolphin's JIT.
Registered at startup in `runtime/overrides/sms_overrides.cpp`. Consulted by both
`recomp_lookup()` and `SunbrightBridge`, so they apply to JIT-entry, `bl`, and indirect branches.

## Debugging recomp correctness
- `SUNBRIGHT_DISABLE_RECOMP=1` — run pure Dolphin JIT (same binary). The A/B baseline:
  if a hang reproduces here too it's not our recomp.
- `SUNBRIGHT_TRACE=1` — log every recompiled function entry (find spin loops by the
  function that repeats; identify the loop's non-recomp caller via `[call_ppc]` exits).
- `SUNBRIGHT_DIFF=1` — recomp correctness **harness**: per recomp function, runs our
  recomp vs Dolphin's interpreter from the same state and compares register/RAM at a
  matching exit PC. Aggregates EVERY diverging function (deduped, counted, named from
  `reference/sms_gmse01_funcs.txt`) into a frequency-sorted, incrementally-written
  report at `scratch/sunbright_diff_report.txt` (override with `SUNBRIGHT_DIFF_REPORT`;
  never /tmp — it's a small quota'd tmpfs). Each function is validated ONCE then runs
  at normal speed (so you can play through scenes); `SUNBRIGHT_DIFF_ALL=1` re-diffs every
  call; `SUNBRIGHT_DIFF_STOP=1` halts at the first divergence; `SUNBRIGHT_DIFF_RAM=1`
  also compares RAM. Skips MMIO-reading and long-loop functions (false positives).
  Workflow: play through a scene → read the report → fix the named functions in the
  emitter/intrinsics → repeat. See `diff_run()` in `runtime/sunbright_bridge.cpp`.
  (Found the `rlwinm` wrap-mask, `fcmpu` FU-bit, register-form `addc`, and full-precision
  `fres`/`frsqrte` bugs this way.)
  - **The harness `RegSnap` MUST snapshot/restore FPRs (both PS slots) + FPSCR**, not just
    GPR/CR/XER. The recomp run clobbers `ppc.ps[]`; without restoring them, the interpreter
    reconstruction reads the recomp's float *results* as its inputs → every FP-heavy
    function (sinf/powf/matan/GXSetFog/audio) falsely diverges on downstream int/CR/CA bits.
  - **Known false-positive class — pointer-discovered interior labels.** `SUNBRIGHT_DISCOVER_POINTERS`
    registers jump-table case bodies (e.g. the `80364xxx` cluster sharing epilogue `80364ad4`)
    as if they were function entries. Validated standalone they show benign CR0 divergences
    (the XER[SO]/CR set by the parent's `cmpi` before the switch is missing). These are NOT
    real functions and must NOT be treated as C-call entry points (task #5).
- Recompiler coverage: `SUNBRIGHT_DISCOVER_POINTERS=1` (recompiler env at `/recompile`
  time) recompiles vtable/pointer-referenced functions too (6032→13464); more coverage
  is now a net win because recomp→recomp calls stay on the native C stack (see "Call
  model"). Discovered entries should still be validated by the harness.
- Time base: `mftb`/`mftbu` read Dolphin's live TB (`SystemTimers::GetFakeTimeBase()`)
  so delay/timeout loops elapse. A frozen TB → infinite spin.

## Current status — RENDERING ✅
Boots the full GC OS → loads all assets → DSP mailbox handshake → **renders the
intro** (the Isle Delfino plane/sky scene) via GX→OGL. Confirmed by frame dump
(`SUNBRIGHT_DUMP=1` → frames as PNG in the user Dump/Frames dir).

The big unblock was the **MMIO API bug**: we used `MemoryManager::Read_U*/Write_U*`
(RAM-only) for hardware registers; they must go through `MMU::Read<T>/Write<T>`
(`runtime/memory_bridge.cpp`). That fixed the DSP handshake and everything HW.

Verification env vars: `SUNBRIGHT_DUMP=1` (dump presented frames as PNG — works under
XWayland where window capture is black), `SUNBRIGHT_VLOG=1` (INFO video logs + title
FPS). Debugging aids: `SUNBRIGHT_DIFF` (differential validator), `sunbright-recomp
--disasm <addr>` (read JIT-only code), `SUNBRIGHT_DISABLE_RECOMP=1` (pure-Dolphin A/B).

Caveat: a blanket `CheckExternalExceptions()` after each recomp call in `jit_hook.cpp`
is **harmful** (derails HW init) — don't reintroduce it.

## Input (keyboard → GameCube pad)
Wired in `main_sdl.cpp` via Dolphin's per-controller input override
(`EmulatedController::SetInputOverrideFunction`) — no device mapping needed, and it
bypasses the focus gate. SDL key events set a bitmask; the override (called on the
emu thread) maps it to pad 0. Keys: Enter=Start, Z=A, X=B, C=X, V=Y, Q=Z, A=L, S=R,
arrows=Main Stick. SI port 0 is forced to a Standard Controller. `SUNBRIGHT_AUTOSTART=1`
pulses Start/A on a timer (headless testing — drives the intro → file-select).

Reaches the interactive **file-select screen** (and 3D cutscenes) under recomp. The audio-init
boot stall (GC scheduler context switch spinning under `run_jit_sync`) is **fixed** by the
`OS_LOAD_CONTEXT` handoff (see Call model) — boot now proceeds past audio init to an interactive
state under recomp (autotest navigates menus, no step-budget abort).

A second, distinct **post-`w1stLoad` audio freeze** (deterministic once headless renders/mixes for
real) is **fixed** by a PC-native port of the JASystem TTrack tick (`func_8031d83c`,
`runtime/overrides/ttrack_tick_native.cpp`) — the recomp of that one function spun its phase-wrap
loop where a faithful native equivalent does not. **The recomp root cause is still OPEN** (f31
clobber ruled out; flagged for a recompiler-level investigation); the native port owns the path and
bypasses it. Isolated via `SUNBRIGHT_FORCE_JIT=LO-HI` range bisection; `SUNBRIGHT_SEQ_DIAG` /
`SUNBRIGHT_TICK_LOG` make the BMS command cycle + tick decision visible.

**File-select fixed (2026-06-10)** by two root-cause fixes: (1) recompiler bctr jump-table bases
now resolve via forward constant propagation (TCardManager::cmdLoop's prologue-built base was
missed → bctr emitted as tail_ppc → mid-thread-body JIT handoff killed the card worker thread);
(2) the CARD hardware layer is **native** (`runtime/overrides/native_card.cpp`: probe / mount /
read-segment / write-page / erase-sector served against a host .raw image, auto-created blank on
first run; synchronous completions) because memcard EXI DMA completion events were lost under
hybrid timing — the CoreTiming global timer crawls (~1 tick per idle iteration) while all guest
threads are parked, so the scheduled `memcardTransferCompleteA` was unreachable and `__CARDSync`
slept forever mid-mount (evidence chain in commit d1b88e2). All card FS logic stays recompiled.
`/recompile` should run with `SUNBRIGHT_DISCOVER_POINTERS=1 SUNBRIGHT_DISCOVER_CODEPTRS=1` —
code-materialized pointer discovery covers runtime-registered handlers (EXIIntrruptHandler
0x8036aa4c was interpreter-only before); the old "CODEPTRS destabilizes boot" caveat is obsolete
since function_needs_jit routing + native OS landed. Probe additions: `/pad?do=<combo>&ms=<ms>`
injects pad input over HTTP (works headed too), `ms`/durations parse as DECIMAL, dead clients
can't wedge/kill the probe, and `SUNBRIGHT_DBG_CARD` ring-traces EXI writes + CoreTiming card
events into `/tracelog`.

Headless now renders (real Vulkan, no present) and mixes audio for real — Null backends are gone,
so render/audio-timing bugs reproduce headless. `SUNBRIGHT_TURBO=1` is opt-in (headless defaults
to real-time). Watchdog catches freezes via no-VI-field (CoreTiming arms it, pre-first-frame too)
and `kill -QUIT <pid>` forces an on-demand dump.
**Audio is PC-NATIVE (2026-06-11, 56eb14d, `runtime/native_audio.cpp`):** our SDL 48 kHz device is
the audio clock — `Mixer::Push*` wraps feed our DSP/DTK ring buffers (fill servo 80 ms, starting
gate 60 ms, SILENCE on underrun), Dolphin's backend is null (its granule mixer REPLAYS audio on a
dry queue = the skipping-jingle class; never reintroduce it as the sink). Emulated time is paced by
`sb_time_ahead()` (dolphin_hook.cpp): host-clock governor + audio-fill servo; the GPU-backpressure
loop and `charge_guest_time` both drive CoreTiming to the governor target so a CPU/GPU stall never
starves audio production (hardware truth: the DSP is an independent processor). GOTCHA: governor
stop level (kCushionMs 80) must exceed the sink gate (kGateMs 60) or boot deadlocks. Audio dump:
`SUNBRIGHT_DUMP_AUDIO=1` → `scratch/wav/native_{dsp,dtk}.wav` (Dolphin's Dump/Audio is a stub now).
Diagnostics: `SUNBRIGHT_DBG_NAUDIO=1` (per-second ring fill + underrun counts — read THIS first for
any audio complaint; underruns correlating with `[vi-perf]` backpressure = GPU stall, not audio),
`SUNBRIGHT_DBG_MIXER[_BURST=N|_OUT=path]`, analyzers `tools/audio/wav_rms.py` + `loop_detect.py`
(beware: JAS loops wave samples — bit-exact repeats in the jingle are CONTENT, not sink loops).
**Audio fixed (2026-06-11, c069f31):** the dead-audio bug (silence after the first instant) was the
JAS audioproc thread silently exiting on a DSP frame-done message with intcount==0 — impossible on
hardware, routine under Dolphin's instant HLE mails. Native port `overrides/audioproc_native.cpp`
(+ tail_ppc bare-blr return, forced entry 0x80312000, native direct-OSExitThread). Verify audio
headlessly with `SUNBRIGHT_DUMP_AUDIO=1` → `scratch/wav/native_dsp.wav` → RMS per second (no ears
needed).
**Music fixed (2026-06-11):** silent BGM/SE ("single-frame samples") was TDSPChannel::updateAll's
DSP-overload limiter (breakLowerActive(126)) misfiring because Dolphin's DSP HLE delivers the 8
subframe sync mails instantly (HW pacing assumption broken — same class as the audioproc
intcount==0 fix). Native port `overrides/dsp_update_native.cpp` (faithful 64-voice loop, limiter
dropped). Verified by WAV RMS: sustained music for 280 s. Also ported native (exonerated but
owned): cmdNoteOn, TOscillator. New tools: `/jas` (live JAS track-tree walk), `/vpb` wave-source
fields, `SUNBRIGHT_WATCH_WADDR` write-watch (dladdr names the emitted writer). Oracle gotcha: use
`SUNBRIGHT_BACKEND=OGL` for DISABLE_RECOMP runs (Vulkan oracle dies with a spurious
VK_ERROR_OUT_OF_DEVICE_MEMORY at 3D-scene entry; cause open); never overlap two instances.

Next: THP-transition NULL-deref read (~79s under autostart), gameplay/Delfino, the recomp
TTrack-tick root cause, FP/edge-case accuracy, widescreen fade overlays / 3D screenspace
effects / culling (user backlog).

## Skills (slash commands)
| Command | What it does |
|---|---|
| `/recompile` | Run recompiler on the ROM → regenerate `generated/` |
| `/analyze-rom` | Dump DOL info, function list, disc structure |
| `/build` | CMake configure + build |
| `/update-docs` | Regenerate docs from current code state |
| `/patch-func ADDR` | Mark PPC address as manually patched (skip recomp) |

## Instruction coverage status
Update this table as ppc_decoder.cpp gains coverage:

| Category | Status | Notes |
|---|---|---|
| Integer (addi, add, sub, etc.) | ✅ | |
| Branches (b, bl, bc, bclr, etc.) | ✅ | |
| Load/Store integer | ✅ | |
| Compare (cmp, cmpi, cmpl, cmpli) | ✅ | |
| Logical (and, or, xor, etc.) | ✅ | |
| Rotate/Shift (rlwinm, slw, srw, etc.) | ✅ | |
| FP single (fadds, fsubs, fmuls, etc.) | ✅ | |
| FP double (fadd, fsub, fmul, etc.) | ✅ | |
| fres / frsqrte (recip estimates) | ✅ | bit-accurate: call Dolphin `Common::ApproximateReciprocal[SquareRoot]` (Gekko ~12-bit table), NOT `1.0/v` — Newton-Raphson refiners (matan) need exact estimate bits |
| addic vs addc | ✅ | distinct ops: `addic` (op12/13)=rA+SIMM, `addc` (op31/xo10)=rA+rB — were both conflated as ADDC w/ immediate emit, breaking register-form addc carry |
| CR ops (crand, cror, etc.) | ✅ | |
| SPR — modeled (LR/CTR/XER/GQR) | ✅ | in CPUState |
| SPR — HW (HID/L2CR/WPAR/BAT…) | ✅ | function routed to Dolphin JIT (side effects) |
| mfmsr / mtmsr | ✅ | mfmsr→live `msr_get()`; mtmsr→func routed to JIT |
| Paired singles (ps_*, psq_l/st) | 🔄 | GC-specific, critical for SMS |
| System calls (sc) | ✅ | HLE via Dolphin |
| Cache ops (dcbt, dcbf, etc.) | ✅ | NOP in recomp — EXCEPT `dcbz` and `icbi` |
| icbi | ✅ | NOT a no-op: `icbi32(EA)` invalidates Dolphin icache line + JIT blocks (interp fetches through them). Dolphin icache *emulation* itself is OFF (`MAIN_DISABLE_ICACHE`, main_sdl.cpp) — its set/way bookkeeping served the WRONG line under the hybrid (phantom-`bl` fetch 4800302d@803378a8 from 803428a8, same set; → wild blr → JUT crash, 2026-06-10). PC-port memory model = fetch-from-RAM |
| dcbz (Data Cache Block Zero) | ✅ | NOT a no-op: zeroes the 32-byte block at EA (`dcbz32`). NOP'ing it left stale THP DCT coefficients → FMV comb (fixed 8bc12c5) |
| lmw / stmw (multi-word load/store) | ✅ | expanded to per-reg loads/stores |
| mftb (time base read) | ✅ | monotonic fake counter |
| mffs / mtfsf / mtfsb0/1 | ✅ | FPSCR modeled in CPUState |
| psq_lx / ps_cmpo0 (indexed PS) | ❌ | Add to opcode 4 decoder |
| fcmpo | ✅ | Ordered FP compare — same as fcmpu for our purposes (no FP exceptions modeled) |

## Known SMS-specific patterns
- Heavy psq_l/psq_st usage for position/velocity data — do not NOP these
- Display lists at 0x80XXX — compiled via GX bridge
- JIT-modifiable code: none confirmed yet
- OSReport → maps to printf in runtime

## Adding a new instruction
1. Add decode case in `tools/recompiler/ppc_decoder.cpp`
2. Add emit case in `tools/recompiler/c_emitter.cpp`
3. Add intrinsic in `runtime/intrinsics.h` if needed
4. Update table above

## Dolphin integration notes
- We hook `JitInterface` to intercept compiled blocks
- When block address is in `g_recomp_table`, call native function instead of JIT
- Dolphin still handles: VideoBackend, DSP, EXI, SI, MemMap
- Do NOT touch `Source/Core/PowerPC/` — that's what we're replacing

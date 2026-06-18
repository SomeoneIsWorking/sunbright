# Sunbright — GameCube Static Recompiler

## ⚡ ARCHITECTURE NOW: NO RECOMPILER IN THE GAME (2026-06-18, Phase C — read FIRST)
The static recompiler is **ERADICATED from the game binary**. There is ONE execution mode:
**gameplay runs under Dolphin's JIT; the engine runs as PC-native overrides.** `generated/` is no
longer linked into `sunbright` (`sunbright_purejit_mode()` is unconditionally true; "Linked 0
recompiled functions"). The `sunbright-recomp` tool is KEPT as **offline static-analysis tooling
only** (ROM decode / PPC disasm / **`--xref`** caller-finder + **`--callees`** call-graph) — it does
not feed the game. `./run.sh` is the JIT-native experience (ngx native renderer on by default); the
**Dolphin-GX baseline** = run with ngx capture OFF (no `SUNBRIGHT_NGX_PRESENT`/`NGX_SHAPE`) → guest
GX/J2D draws run under Dolphin's JIT.
Consequences for the rules below: the debugging-path "fix the recompiler / mistranslation" branches
are **dead** — every bug is now own-it-natively (RE + PC-native override) or a Dolphin-JIT issue.
Many CLAUDE.md sections below still describe the recomp era (call model, hybrid execution, recomp
correctness harness) — treat those as HISTORICAL. Live detail: `debug_journal/
2026-06-18_no_recomp_jit_native_pivot.md` + memory `no-recomp-jit-native-direction`.

### Engine re-grounding under no-recomp (which subsystems run PC-native)
After recomp eradication, an engine override runs ONLY if it's a purejit-safe full-replacement (the
`SUNBRIGHT_OVERRIDE_NATIVE` macro, or `register_override` + `mark_override_purejit_safe` at STATIC-INIT
— `native_os_init()`/`recomp_build_dispatch()` are NOT called under the eradicated build, so the old
`native_os_register` path is dead). Re-grounded PC-native so far: **renderer (ngx)**, **audio
(native_jas)**, **memory-card (native_card** — purejit-safe seams + guest leaf helpers via `call_ppc`).
Known gaps to own next: EFB-readback effects (sun occlusion, **pollution darkening
`drawShineShadowVolume`** — gated by 2 sites in `TModelWaterManager::perform`, found via `--xref`,
pollution coverage, mirror, dash-blur) read an empty EFB under ngx present; the Delfino "wash" is the
pollution instance (PARKED — see memory `delfino-lighting-wash`, do NOT re-assert "pollution=solved").

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

## 🔧 TOOLING / VERIFICATION FIRST (HARD RULE — user directive 2026-06-19)
**If the harness that would verify a change is missing or broken, FIX/BUILD THE HARNESS BEFORE the
change. No exceptions.** This is not a judgment call — it is a hard precondition, the same standing as
the debugging-path rule above.
- Before working a bug/fidelity/effect change, confirm the verification path actually works on real
  data RIGHT NOW. A green-looking tool that silently compares against garbage is worse than no tool —
  it manufactures false conclusions (e.g. `/abshot2`'s GX oracle was all-black under NGX_PRESENT, so
  `ab_diff` reported a bogus ~40% that nearly got cited as a regression; memory `abshot2-gx-oracle-empty`).
- When a tool can be fed a degenerate/empty/stale input, it MUST detect and refuse it loudly, never
  emit a number anyway. Guard the harness, don't trust it blindly.
- "I can't cheaply verify this in a reachable scene" is a STOP: build the reachability/oracle tooling
  first (or pick a target that IS verifiable). Do not port effects you cannot verify — that violates
  "verify before you declare done."
- This generalizes the TDD-renderer section below: tooling is the prerequisite, the fix is second.

## What this project is
Static recompiler for Super Mario Sunshine (GameCube/PowerPC) → native PC binary.
ROM: provided via `$SUNBRIGHT_ROM` (set it in a gitignored `.env` next to `run.sh`, or drop a
`rom.rvz` in the repo dir). No machine-specific ROM path is committed.

Pipeline: RVZ → extract DOL → decode PowerPC → emit C → compile → native .so
Runtime: Dolphin subsystems (GFX/DSP/Memory/Input) drive the native code via hook layer.

## Architecture direction (2026-06-15, user) — guest-layout native engine, no Dolphin
**Same GameCube memory layout, everywhere.** Engine objects stay guest-RAM, GC-layout (32-bit
big-endian pointers, GC offsets) in the shared arena. **PC owns the engine code as native C++
that operates ON that guest layout** (like `native_jas` / `sms_drawsync_lossproof` /
`native_card` / the native renderer in `runtime/render/` + `runtime/ngx/`, which read J3D
objects straight from guest RAM). **Gameplay stays recompiled** on the same memory — `mMaterials[i]`
is a plain guest load that Just Works. Boundary = plain function-call overrides over shared
guest memory; NO handles/getters/marshalling. End state: no Dolphin (own GPU/renderer/OS/audio).
Live frontier = the native renderer (`docs/native_port_plan.md`, N5 per-material TEV combiner).
⛔ The "flip" / host-layout-engine architecture (handles, tailored getters, `port/` engine,
`SB_FLIP_J3D`) was tried and **deleted** — see `docs/DO_NOT_REVISIT_FLIP.md`; do not resurrect it.

## Architecture overview

```
tools/recompiler/    Offline tool: ROM → generated/functions.cpp + jump_table.cpp
runtime/             Dolphin integration layer (JIT hook, memory bridge, OS HLE)
runtime/overrides/   Hand-written native overrides + manual JIT routing
generated/           Recompiler output — gitignored, regenerate with /recompile
                     functions.h (decls) + functions_<addr>.cpp ×N (bucketed by
                     address, compiled in parallel) + jump_table.cpp
externals/dolphin/   Dolphin submodule — now our FORK (SomeoneIsWorking/dolphin,
                     branch `sunbright`). Prefer runtime/ overrides; modify Dolphin only
                     for real engine/base bugs (e.g. the headless frame-cycle VRAM leak).
                     Commit in the submodule → push to the fork → bump the parent gitlink.
                     Still do NOT touch Source/Core/PowerPC/ (that's what we replace).
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
- `SUNBRIGHT_FASTBOOT=1` — boot straight into save File 1, Delfino Plaza (no input driving, no
  save state): a PC-native port of the boot sequencing + file-select→gameplay transition
  (`runtime/overrides/fastboot_native.cpp`). Skips logo/attract-movie/title; reads the File 1
  save block from the host memcard image, ports TFlagManager::load + moveStage's
  decideNextScenario (episode from save flags), returns APP_STATE_GAMEPLAY from the movie
  director. GOTCHAS learned: the movie-direct override must WAIT for gSetupThread (0x803FCBE8)
  to terminate + join before returning — tearing down TMovieDirector mid-THP-open crashes the
  THP workers (the game's own setup-failure path proves wait+join+return-5 is safe); scenario
  0xFF must be resolved (raw 0xFF fails mountStageArchive → loadResource 1 → DONE bail);
  TMovieDirector's ctor stores TWO vptrs — the final vtable is 0x803DFA50 (direct = +0x64 →
  0x802b5b30), 0x803E1D50 is the base sub-object's.
- `SUNBRIGHT_BACKEND=OGL|Vulkan|Software` — GFX backend (both OGL & Vulkan work under XWayland; Vulkan is the default)
- `SUNBRIGHT_NGX_SHAPE=1` — enable the native renderer's J3D capture + probes (`/ngxshape`, `/ngxrender`, `/ngxpresent`); diagnostic-only (does not change on-screen output)
- `SUNBRIGHT_NGX_PRESENT=1` — **native present**: the native renderer's frame (`runtime/render/ngx_present.cpp`) becomes the live on-screen image (and the frame dump), replacing Dolphin's GX output in the render path (Vulkan only). Implies NGX_SHAPE capture. Now composites the J2D/HUD overlay (coin/shine counters, FLUDD gauge, nameplate) over the 3D scene. `/ngxpresentlive` = renderer stats (incl. `hud_quads`)
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

## Native renderer (ngx) — GROUND RULES (read FIRST, every session)
These are standing user directives. Stop re-deriving them:
- **`./run.sh` shows the NATIVE ngx renderer by default** (`SUNBRIGHT_NGX_PRESENT=1`). So any
  headed visual bug the user screenshots IS an **ngx bug**, not Dolphin GX. The Dolphin-GX baseline
  is `SUNBRIGHT_NGX_PRESENT=0 ./run.sh` (use it only as the oracle, never as "the output").
- **It is a NATIVE Super Mario Sunshine renderer, NOT GameCube-rendering emulation.** Never source
  render data from Dolphin's EMULATED state (`xfmem`, `g_main_cp_state`, GX register hooks) — those
  are async-lagged AND the wrong layer. Read the game's **J3D object model straight from guest RAM**.
  Proven pattern: per-vertex array bases come from `j3dSys` — POS=`J3DSYS+0x10C` (unk10C),
  NRM=`+0x110` (unk110), **CLR0=`+0x114` (unk114)** (the magenta bug was reading CLR0 from
  `g_main_cp_state` instead — commit 70c5de3). When you need data, RE the J3D path in `reference/sms`
  and read the object, don't tap Dolphin. Do MORE RE if needed.
- **FIDELITY WORK ORDER (user directive): make the TITLE screen + FILE-SELECT render correctly
  BEFORE Delfino gameplay.** Do NOT jump to Delfino/in-game scenes while title/file-select are still
  wrong. Reproduce title/file-select headless with `SUNBRIGHT_AUTOSTART=1` (+ `tools/gpshot --fs`).
- FIXED 2026-06-17 (e1dbe76): the **title logo shred** was NOT geometry/matrices/clip — it was
  TEXTURE BLOCK-PADDING. The logo is a single RGB5A3 J2DPicture (w=460); the decode paths padded
  width to a multiple of 8, but RGB5A3 tiles at a 4-wide block → over-stride → diagonal shear. Fix:
  `sb_tex_pad_w/h` (format block dims) inside `texture_for()`; render-test unit `tex_pad`. A textured
  quad shredding into diagonal slivers ⇒ a tiled-texture width/stride bug, never geometry.
- FIXED 2026-06-17: the **file-select menu windows rendering WHITE instead of blue** was a TEXTURE
  block-padding UV leak (the UV counterpart of the title-logo shear fix). The native present uploaded
  each decoded texture into a Vulkan image at the format's BLOCK-PADDED size (`pw×ph`, e.g. a 20×20
  IA4 window-border corner → 24×20), but sampled with `u/v ∈ [0,1]`, so the garbage padding columns
  (cols 20–23, full-white here) got sampled. The J2DWindow 9-slice border EDGE quads use a degenerate
  `U=1.0` (`J2DWindow::draw_private`), landing squarely in the white padding → opaque white frame. Fix:
  `texture_for()` now creates the image at the LOGICAL `t.w×t.h` and copies only that region with
  `bufferRowLength=pw` (decode keeps the padded stride). Affects every non-block-multiple texture.
  Verified file-select: borders white(255,255,255)→blue(27,37,252); A/B 21.7%→18.8%. Diagnostics added:
  `/j2dscreens` (every recent J2DScreen root + per-window visibility/fill/border-tex inventory — proved
  there's ONE root with 4 VISIBLE blue windows, refuting the stale "all vis=0" handoff claim) and
  `/texat?a=&fmt=&w=&h=` (decode a guest texture → intensity/alpha grid; showed the IA4 decode is CORRECT
  and the padding cols are white). RESIDUAL: small-slot border edges read a bit light (the OPEN-#1
  brightness/blend-over-background class), not white.
- FIXED 2026-06-17 (a3d740e): the **file-select sky wash** was the native present **hardcoding its 3D
  clear** to (0.10,0.12,0.18). The sky base ti=11 is a SCREEN blend (src=ONE dst=INVSRCCLR) so the
  clear shows through → grey wash. Fix: capture the game's `GXSetCopyClear` (0x8035ea40) and clear to
  it (= black here). GOTCHA: GXColor is passed BY POINTER in r3 (`[r3]`=packed RGBA8888, r4=clear_z),
  not by value. Verified: skyM/watR pixel-perfect, skyL red 161→49. RESIDUAL sky wash (ti=10 additive
  / ti=9 premult white cloud layers) is the multi-layer-blend NO-ORACLE trap below — don't eyeball it.

## Debugging the native renderer — TDD, NOT eyeballing (read this before touching ngx)
The renderer was built straight to "draw the whole scene and look at it." That has no
falsifiable test, so fidelity bugs (the projection "wash", dropped ortho) produced a thrash
of contradictory root-cause claims that each survived multiple commits (`ti=9`→`ti=11`→"it
was a desync confound"). **A renderer fix MUST move a number in a deterministic test — never
"it looks better now."** Two confounds make eyeballing worthless: (1) comparing two
independently pad-driven processes that drift in animation phase; (2) whole-frame PNG diffing,
which says "different" not "why" and has no stable golden when the output is still broken.

- **`sunbright-render-test` (ctest target `render_test`)** — the renderer's `SUNBRIGHT_DIFF`:
  bottom-up unit tests over the *pure* renderer units, each asserting **spec-computed** ground
  truth (hand-derived expected values), Dolphin-free / no ROM / no GPU. Run with
  `ctest --test-dir build -R render_test` or `./build/sunbright-render-test`. Add a unit:
  extract the pure function into a header (e.g. `runtime/ngx/ngx_project.h`), write a
  `test_<unit>` with hand-computed cases, register it in `runtime/render/render_test.cpp`.
  **The tested function must BE the shipping function** — call it from the override (don't fork
  a copy), or the test validates dead code. Units so far: `vertex_decode` (GC attr dequant),
  `projection` (eye→clip→NDC, the dropped-ortho class). Next climb: near-plane clip
  (Sutherland-Hodgman vs `d=z+w≥0`, currently inline WIP in ngx_j3d_shape.cpp), TEV combiner,
  color remap. `tex_decode_selftest` (Dolphin-oracle texel parity, via `/tex`) is the same idea
  for textures.
- **When a unit isn't extractable yet, extract it first.** "Draw the scene and compare" is the
  integration test of LAST resort, after the pure units underneath it are green.

### The pixel oracle — the ONLY valid whole-renderer test vs Dolphin (use this)
CPU-side Dolphin state (`xfmem`, GP registers) is **async-lagged** and is NOT a valid oracle —
PROVEN 2026-06-16: `GXLoadPosMtxImm` matrices come straight from the call args (= exactly what
Dolphin loads) yet 77% disagree with `xfmem` read one instruction after the load. So never build
a "ngx CPU-state vs xfmem" differential (the retired `SUNBRIGHT_NGX_DIFF`/`/ngxgeomdiff` did,
and its verdicts are unsound — see memory `xfmem-not-cpu-oracle`). The ONLY trustworthy whole-
renderer reference is **rendered PIXELS**, captured zero-drift from the SAME present:
- **`/abshot2`** (probe, needs `SUNBRIGHT_NGX_PRESENT=1`) writes `scratch/screenshots/ab2.gx.ppm`
  (Dolphin's GX XFB = oracle) **and** `ab2.ngx.ppm` (ngx's native render) from the *identical*
  present → pixel-perfect camera alignment, one process, no drift. This is the geometry analog of
  `tex_decode_selftest` (Dolphin rendered live each run; NOT a stored golden — that's why the
  stored-PNG approach was rejected).
- ⚠ **BROKEN under the no-recomp pivot (verified 2026-06-19): the `/abshot2` GX oracle (`ab2.gx.ppm`)
  comes back ALL-BLACK** — under `NGX_PRESENT` Dolphin no longer renders the guest GX draws (ngx
  replaces them; "Dolphin EFB confirmed empty"), so there is no GX XFB to capture. A diff against a
  black oracle silently reports a meaningless ~40% — almost cited as a real regression this session.
  **The "Baseline 2026-06-16: 40% mean delta — water/sky black" below was very likely this same
  empty-oracle artifact, not a true ngx gap.** `ab_diff.py` now REFUSES an empty/black frame (exit 3).
  For a real GX oracle, capture it from a SEPARATE `SUNBRIGHT_NGX_PRESENT=0` (Dolphin-GX baseline) run,
  frame-matched via `SUNBRIGHT_STATE=<save>` (memory `ngx-render-fidelity-gap`) — single-present
  `/abshot2` cannot produce both halves anymore.
- **`tools/render/ab_diff.py`** turns the two PPMs into a NUMBER: mean abs pixel delta (overall +
  4×4 per-region grid, which localizes WHICH part is wrong) + a heatmap. A fix MUST drop this
  number. (Historical) Baseline 2026-06-16 (intro gameplay): **40% mean delta** — but see the ⚠ above:
  treat that figure as suspect (probable empty-oracle artifact). Workflow: run headless w/
  `SUNBRIGHT_NGX_PRESENT=1 SUNBRIGHT_PROBE=1` to a 3D scene → `curl /abshot2` (check `/ngxpresentlive`
  shows `frames>0` first) → `python3 tools/render/ab_diff.py --heat out.ppm` (exit 3 = empty oracle).
- GOTCHA: `pkill` returning nonzero (nothing to kill) aborts a chained `&` launch — launch the
  game on its own line. And ALWAYS `pkill -9 -f "build/sunbright "` a finished run: a stale
  instance squats probe port 17654 and silently serves the OLD binary to your curls.

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

**Garbage-audio fixed (2026-06-11 pm):** the native sink was fed raw Mixer::Push*Samples
buffers, which are BIG-ENDIAN R-L (Dolphin converts only inside PushSample) → byteswapped PCM
= loud noise. na_push_dsp/dtk now convert. WAV-RMS canNOT catch this (byteswapped noise has
healthy RMS) — verify dumps with adjacent-sample delta²/energy (real ≲0.05, garbage ≳0.25) and
a constant-DC check (constant nonzero = frozen voice feed, hear silence).

**Time-independent device service (2026-06-11 pm):** CP interrupts, PE tokens, and idle-driver
device service must NEVER depend on CoreTiming advancing — the host-clock/audio governor parks
time at target, and any 0-cycle "deliver later" CoreTiming event then never fires (CP
m_interrupt_waiting wedged the GPU loop at 99% = the logo boot freeze). poll_yield now services
pending CP interrupts from live FIFO state + drains the PE token ring + flushes deferred JAS
mails; idle_run gates only time advancement, not device service; drawsync recovery also runs
from the backpressure spin (idle_run can't run while the main thread spins Ready).

**Native TDrawSyncManager (2026-06-12, sms_drawsync_lossproof.cpp) — the backpressure-wedge
ROOT CAUSE fix.** The guest threadFunc paces CPU↔GPU by COUNTING queue messages (boundary
pushes vs tokens); that is only sound when a frame's token always arrives after its boundary
push. In the hybrid, tokens batch at dispatch points while pushBreakPoint posts immediately —
an early token underflowed the guest TFifo, leaving a phantom entry; terminal state (size 1,
breakpoint enabled at a completed boundary, GPU parked, queue empty) was captured live and the
old state-based recovery refused it (`size>=2` guard) → permanent vi.gpu_backpressure spin →
watchdog kill (the Delfino-entry freeze). Now the WHOLE protocol is PC-native:
overrides on drawSyncCallback 0x802a9318 (token 0 = retire; ranged tokens call their guest
callback via vtable+8 then retire) and pushBreakPoint 0x802a9020 (GXFlush + record live CP
write ptr) keep a host deque + CREDIT counter (order-independent accounting) and apply the
faithful breakpoint policy (≥2 outstanding → BP at second boundary; else off) idempotently.
GOTCHA: guest GX calls (GXEnableBreakPt) deliver nested token events on the same host thread —
the manager uses a recursive mutex + g_applying re-entrancy guard (a plain mutex self-
deadlocked). The guest counting thread now sleeps forever (no messages); the synthetic-token
recovery is retired (sms_drawsync_native.cpp is a stub). Verified: 7-min headless run,
18432 pushes / 18431 retires, backpressure phase 0ms throughout, gameplay reached past the
Delfino entry animation. SUNBRIGHT_DBG_DS=1 = per-256-push accounting line.

**Idle-driver crash fix (2026-06-12, dolphin_hook.cpp idle_run):** never borrow the current
OSThread's saved OSContext for the idle spin — when that thread has just EXITED its context
reads back zeros, the idle register file got r13=0, and the next natively-dispatched ISR read
SDA at 0-0x7138 (ea 0xffff8ec8, PC 0x80002FF8 = the idle `b .`) — the boot-logo/THP-transition
crash. idle_run now uses a dedicated idle OSContext at 0x80001900 and caches SDA bases (r2/r13)
from the last valid context.

**Boot pacing has TWO engage signals (2026-06-12):** first audible sample (na_ever_pushed) OR
first timed visual — TSMSFader::startWipe override (overrides/fader_pace.cpp, sb_visual_live).
The GC-logo fade-in precedes all audio; unpaced it completed in milliseconds and the logo
popped in fully visible.

**CORRECTION (2026-06-11, late): the "jingle fixed" claim below was FALSE.** What played was
the THP-movie/HardStream mix (low-pitch, RMS ~400 vs the real jingle's ~5000, zcr ~300-500 vs
1208-1316 — verified against the oracle and the ROM-decoded wave). ALL sequenced JAS audio
(SE + BGM) is dead under recomp; only streams are audible. RMS alone cannot tell them apart —
always check zcr/pitch against `scratch/wav/jingle/w1stLoad_0_w02.wav` or the oracle.
**THE REAL FRONTIER (precisely pinned, reproduce with the probes below):** the JAI init sound
(id 0x80000800, handle at `gpMSound[0x8040E17C]+0x38`) sticks at state 3 (byte +1; oracle
reaches 4 in ~3 s). State 3→4 needs `checkSeqActiveFlag`: root TTrack (0x80625848) must have
CHILD tracks (+0x2C4..) — oracle has 2, recomp 0. Children are opened by the parser, but
`TTrack::mainProc` is stuck in wait-for-note-end (`mSeqCtrl.mWaitTimer==-1` + a NoteMgr channel
that never reaches state 0xFF): the note's TChannel never starts — `TDSPChannel::alloc` /
`playLogicalChannel` are NEVER called (DSPQueue empty or enQueue/breakLower failing), no VPB
ever gets enabled=1, so the voice neither plays nor ends. Suspects: cmdnoteon_native /
oscillator_native ports, or the enQueue path under recomp. Trace tooling lives in
`overrides/jas_rate_diag.cpp` (SUNBRIGHT_JAS_RATE=1; [vstart] traces alloc/play).
**CORRECTED (2026-06-12): overrides DO fire on recomp→recomp calls under the current call
model.** The emitter renders every `bl`/`bctrl` as `call_ppc(...)`, which consults
`recomp_lookup()` → override table first (dolphin_hook.cpp). The old "overrides are blind to
recomp→recomp direct calls" claim dates from the pre-C-call emitter and is FALSE now — override
tees are a reliable interception seam everywhere (proved: the setSeInterVolume tee fires from
recompiled JAI callers). A zero override count means the guest genuinely doesn't CALL that
function — e.g. setSeDistanceVolume never calls setSeInterVolume; the compiler inlined it into
a direct param-slot write (the loud-ambient-SE root cause).

The infrastructure below from this session is real and kept (it fixed crashes/protocol wedges,
just not the sequenced-audio death):
- **AID/DSP-interrupt chain native** (`overrides/aid_native.cpp`): AID raise claimed at the
  `UpdateAudioDMA` --wrap seam (`GenerateDSPInterrupt` is INLINED there — wrapping it alone
  misses the raise), delivered as a level-triggered FLAG (bursting a backlog desyncs the
  JAS↔ucode cycle → Zelda HLE self-halt); DSP-mail interrupts captured at
  `GenerateDSPInterruptFromDSPEmu` so they are never CoreTiming-scheduled (the time-parked
  starvation root cause behind intr7 ~2.5/s). SUNBRIGHT_DBG_AID=1 = per-second chain liveness.
- **Native SMS DAC ucode** (`overrides/zelda_ucode_native.cpp`, --wrap `UCodeFactory`, CRC
  56d36052): our own SYNC_PER_FRAME mail state machine reusing Dolphin's pure-math
  ZeldaAudioRenderer. Tailored: NO permanent HALTED state (Dolphin's "Sync mail received when
  rendering was not active. Halting." killed audio for the whole run); out-of-phase syncs
  dropped, unknown commands acked. SUNBRIGHT_DBG_ZN=1 traces every mail + state.
- **Synchronous mail delivery** (`memory_bridge.cpp` mem_w16_slow): the captured DSP interrupt
  is flushed at the post-store boundary of the CPU→DSP mailbox-low write (0xCC005002) — but
  ONLY with EE on. Flushing into an EE-masked critical section interleaves nested JAS mail
  sends into an in-flight mail sequence (torn mail `cdd17ac0` = CDD1 high + param low);
  flushing from inside HandleMail deadlocks boot. Without the flush the round-trip waited for
  poll_yield: ~1.5 s per render cycle = audio at 1/40 speed = silence.
- **Unpaced boot**: neither the time governor (`sb_time_ahead`) nor the 60 fps frame pacer
  (`ov_VIWaitForRetrace`) engage until the game pushes its first NONZERO audio sample
  (`na_ever_pushed`, native_audio.cpp — "ever pushed at all" latches too early: the DAC pushes
  silence from audio-init). Boot loading loops yield once per frame, so pacing from frame 0
  cost ~8 s wall. PC-game rule: load uncapped, pace from the first audible sample.

**Audio data decodes PC-natively from the ROM** (`docs/audio_data_formats.md`, `tools/jingle/`):
RVZ→FST (`sunbright-jingle` extractor), nintendo.szs→Yaz0→RARC→mSound.aaf→WSYS wave table→AFC
(`jingle.py`). `w1stLoad_0.aw` wave 2 = the boot jingle (verified by ear). mSound.aaf is NOT on
the FST — it lives inside /data/nintendo.szs. Never decode an .aw flat: waves are offset-cut by
the WSYS table (flat decode = garbage). `tools/audio/run_check.sh [secs] [ENV=V…]` = one-command
headless run + per-second WAV profile.

Additional session landings (2026-06-11 late):
- **Native JAS frame driver** (`overrides/jas_driver_native.cpp`): replaces AID-IRQ/DSP-mail
  ping-pong with direct OSSendMessage posts (msg0 + batches of 7 msg1, gated on guest intcount
  ==7) to the real audioproc thread, device-clocked (one period per 839 output frames via
  `na_consumed_frames`). Engages on the ucode's first cmd02; ack mails suppressed; AID delivery
  and DSP-mail interrupts off in driver mode. v1 (running updateDac/updateDSP NESTED on
  arbitrary threads) corrupted TApplication state — the work must run on the audioproc thread;
  only message posts are ISR-safe. 9/9 runs crash-free.
- **Oracle guard**: all audio wraps/overrides pass through under SUNBRIGHT_DISABLE_RECOMP —
  the native AID capture had silently broken the oracle (110 s of zero pushes). Oracle runs
  need this or they prove nothing.
- **force_jit_range(0x80301c00,0x80301e00) REMOVED** (sms_overrides.cpp): an old stopgap that
  interpreted the JAI frame work; under current architecture it killed SE-request processing
  outright. Its original excuse (data-dependent JAIBasic corruption) predates the emitter fixes.
- SUNBRIGHT_PACED_BOOT=1 = A/B env to re-pin boot to 60fps/host clock (ruled OUT pacing as the
  seq-audio death cause — wedge identical both ways).

**NATIVE AUDIO ENGINE M1 ✅ (2026-06-12): the jingle (and all SE-class sounds) play through a
fully PC-native JAS engine** (`runtime/native_jas.cpp`, plan + verified details in
`docs/native_audio_engine.md`). It loads WSYS/IBNK/sequence.arc straight from the ROM, runs
the REAL SE BMS (a TSeqParser/TTrack port), synthesizes AFC voices, and mixes into the DSP
sink inside `na_push_dsp`. Intake: override tee at JAIBasic::startSoundBasic 0x803020ac
(`overrides/se_native.cpp`); the guest path still runs for bookkeeping (deleted at M4). The
old init-BMS note-start wedge is thereby MOOT for audibility (the guest JAS stack remains
wedged but unreferenced for SE output). Verified by engine-solo dump (`SUNBRIGHT_DUMP_NJAS=1`
→ scratch/wav/njas_solo.raw): jingle zcr within 0.7% of the ROM-decoded reference; boot SE id
is 0x7915. BMS disassembler: `tools/audio/bms_dis.py` (opcode names/args derived from the
JASSeqParser decomp — note sCmdPList names ≥0xD0 are shifted −1 vs a naive reading; the
Arglist in the tool is byte-stream-verified). Engine debug: `SUNBRIGHT_DBG_NJAS=1`; disable:
`SUNBRIGHT_NO_NJAS=1`; oracle (DISABLE_RECOMP) is automatically unaffected (overrides never
dispatch there).

**NATIVE AUDIO M2 ✅ (2026-06-12): the JAI SE handle layer is native** — TOuterParam port on
native tracks (vol×/pitch×/pan-replace, JASTrack.cpp:391 semantics), per-sound move-param
slots (9× vol/pitch/pan) flushed at a 60 Hz JAI tick, tees keyed by sound id (guest
JAISound+0x8) for stopSoundHandle 0x80302224 / setVolume/setPan/setPitch 0x8030a57c/a604/a68c
/ setSeCategoryVolume 0x803029a4; startSoundBasic captures JAISoundInfo swbit+prio (gpr[9]).
Same-id retrigger (unless swbit bit19), idle release via worker port2 busy→idle, fade-stop =
vol slot 6 → 0 (stopSoundHandle semantics). Verified 100 s headless: jingle zcr 2476
(unregressed), the game's own 30-frame jingle fade-on-skip now audible, real category volumes
captured (cat5=74…), 0 unhandled BMS ops / 0 missing waves / no voice leaks. Raw-dump
analyzer: `tools/audio/raw_profile.py` (per-second RMS+zcr of njas_solo.raw). NOT yet: 3D
distance attenuation/pan (guest computes those into internal slots we don't tee), fxmix/dolby
outer, per-category concurrency limits/priority stealing — M2.5 list in
docs/native_audio_engine.md.

**NATIVE AUDIO M3 ✅ (2026-06-12): BGM plays natively.** The seq table is **BARC** (mSound.aaf
chunk 4) — 48 entries, BGM id & 0x3FF = index, offsets into sequence.arc (which has NO Vload
header; JaiArcS.hed isn't on the SMS FST — the decomp's Vload path is dead code for SMS).
Multi-root subframe driver (init/SE root + one root per BGM), seq-class ids (0x8xxxxxxx) tee
to njas_bgm_start, handles reuse the M2 registry (fade-stop, dedupe-while-playing, recursive
root close). Bank residency moot: all banks/waves decoded at load. Verified live: k_title →
t_select → k_camera (fade=20 stop) → k_dolpic, 831 noteOns, sustained music RMS 5–7k zcr
1.5–3k, 0 unhandled BMS ops, no crash 150 s. Open risk: per-scene wave-id collisions in the
merged WSYS tables (none observed; revisit if a stage BGM sounds wrong).

**NATIVE AUDIO M2.5 ✅ (2026-06-12): SE 3D distance attenuation/pan/pitch native.** SE param
tees moved to the inner setters setSeInterVolume 0x8030b700 / Pan 0x8030b8c8 / Pitch
0x8030be20 (slot=r4, f1=value, time=r5) — the funnel for the public API AND the per-frame
MSHandle distance code. Verified: ambient SEs get per-frame vol/pan/pitch with 4-frame
smoothing (23k param events / 130 s). Outer setVolume/Pan/Pitch tees route seq ids only.

**NATIVE AUDIO M2.6 ✅ (2026-06-12 pm): PC-native 3D SE layer + JAI request lifecycle** —
mirror deleted; engine owns cameras (setCameraInfo 80300ce4 tee), positions (JAIActor Vec*),
MSHandle curves (smSeCategory/smACosPrm/calcVolume/calcPan as static data), and the request
lifecycle (continuous-class re-request = REVIVE not restart; lifeTime 10 frames; expiry fade
cancelable). Fixed alongside: pad double-action (pad_override must CLAIM controls — returning
nullopt fell through to Dolphin's default keyboard profile, every key fired 2 GC buttons);
perc TPmap pitch@0 (⚠ FALSIFIED 2026-06-12: binary BNKParser+TDrumSet::getParam prove
**vol@0 / pitch@4**; the −30 dB fix actually came from the PER2 pan-as-volume correction —
see docs/re_notes/audio_re_findings.md §2.3); spray stop-fade (engine owns vol[6] during stopFade).
**Audio A/B harness:** tools/audio/delfino_ab.sh (+ spray driver) joins oracle /vpb voices to
native /njas voices by wave content hash (FNV of first 64 .aw bytes) → per-wave pitch ¢ / vol
dB report (p90 peak-gain — medians mix BGM/SE contexts). HARNESS GOTCHAS (both burned a
triage round): dolby voices must read dolby_volume_current (channels[6] are IGNORED by the
ucode when use_dolby_volume — stale ch values masked all 3D motion as constant volume), and
scene timelines drift between sides.
**Inaudible-BGM-drums FIXED (9271566) — the osc-route class:** cmdSimpleADSR/SimpleEnv do NOT
route the track osc onto notes (only cmdOscRoute; reg-6 write resets routes to 0xF); our
force-route + initStart hijacked percussion attacks (1-tick drum notes peaked 0.0167) and
wiped perc directRelease. PER2 release 0xC00A is NOT garbage (bits14-15=curve mode).
OPEN (quantified, post-fix): wave 10:1 ~-16 semitones; 11:0 -300¢; 5:229 drum keys ±100-300¢
medians; 6:77/6:231 +13..25 dB; ~9-17 oracle-only waves missing per run; fxmix/dolby buses
unimplemented. Pikmin 2 decomp clone at scratch/ref/pikmin2 (same-gen JASystem, matched —
better nav source than reference/sms; but its TPmap getParam field-crossing is a trap: trust
the ORACLE for data semantics); Dusklight at scratch/ref/dusklight (native JAudio2 PC port —
freeverb fxmix reference).

**Whole-game crawl fixed (2026-06-12): Dolphin's CoreTiming::Throttle is OFF on recomp runs**
(MAIN_EMULATION_SPEED=0, main_sdl.cpp). Two governors fought: our native host-clock/audio
governor parks emulated time, so when a worker guest thread (audio prio2 / THP decode prio20 /
DVD) caught CoreTiming up via charge_guest_time, Dolphin's throttle slept ~16 ms PER VI FIELD
inside that thread's nthr token slice → the VIWait bounded drain (16.7 ms) overshot by 100+ ms
→ 6–12 fps at a reported "speed 1.0000" (title + THP + everywhere). Diagnosed with
SUNBRIGHT_DBG_DRAIN=1 (nthr-hold per-prio token-hold accounting, native_threads.cpp) + gdb
stack sampling (CoreTiming::Throttle inside call_ppc was the smoking gun). The DISABLE_RECOMP
oracle keeps Dolphin's throttle (it has no native governor). Also landed: locked-L1-cache
(0xE0000000) inline fast path (intrinsics.h/memory_bridge, backed by Dolphin's flat L1 array)
+ native LC DMA overrides (overrides/sms_lc_native.cpp: LCStoreBlocks/LCStoreData = memcpy,
LCQueueWait/LCFlushQueue = no-op; addresses verified vs decomp OSCache.c — funcs.txt has a gap
there) — THP decode uses the locked cache as scratch and its LC calls are mtspr-HW → were
interpreted per 4 KB chunk. Perf triage order for "low fps at speed 1.0": vi-perf phase line →
SUNBRIGHT_DBG_DRAIN → gdb -batch thread apply all bt (find the token holder sleeping).

Next: native audio M4 (delete the guest audio path) per docs/native_audio_engine.md; oracle
ear-check of BGM fidelity (tempo/instruments); fxmix/dolby + category concurrency; residual
backpressure wedge (~1/3 runs), THP-transition NULL-deref read, gameplay/Delfino, the recomp
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

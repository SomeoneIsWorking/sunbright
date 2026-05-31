# Sunbright — GameCube Static Recompiler

## What this project is
Static recompiler for Super Mario Sunshine (GameCube/PowerPC) → native PC binary.
ROM: `$SUNBRIGHT_ROM`

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
for **both OGL and Vulkan**. Default ROM: `$SUNBRIGHT_ROM`.

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
Graphics config is applied in `main_sdl.cpp` via `Config::SetBase` (GFX_EFB_SCALE,
GFX_ASPECT_RATIO, GFX_WIDESCREEN_HACK) before boot.

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
    `cpu.lr`, then continue the caller inline.
  - `tail_ppc` (b/bctr that leave the function): recomp target → nested call then the
    caller returns; non-recomp target → commit state and `siglongjmp` back to
    `SunbrightBridge::Run`, unwinding the recomp C stack and letting the CPU loop take
    over. This is what makes the **boot→main-loop handoff** (a never-returning tail
    branch into OS/runtime code) work — running it synchronously under the interpreter
    instead would try to run the whole game inside `SingleStep` and spin. `Run` installs
    the longjmp target via `sunbright_set_tail_jmp`.
  - `cpu_to_dolphin_state` writes back XER[SO]/OV too (needed for the C return).

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
  report at `/tmp/sunbright_diff_report.txt`. Each function is validated ONCE then runs
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

Reaches the interactive **file-select screen** (and 3D cutscenes) under recomp.
Next: gameplay/Delfino, audio output, FP/edge-case accuracy via `SUNBRIGHT_DIFF_RAM`.

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
| Cache ops (dcbt, icbi, etc.) | ✅ | NOP in recomp is fine |
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

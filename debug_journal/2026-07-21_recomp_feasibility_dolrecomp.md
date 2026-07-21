# 2026-07-21 — Recomp-with-overrides feasibility spike (DolRecomp)

Evaluating the user's proposal: keep decomp+Aurora, and add a SEPARATE recomp with
overrides alongside it — explicitly NOT the retired recomp/decomp hybrid.

## Why this is not the thing that was retired

The retired architecture was the **flip/hybrid**: recompiled PPC calling into
*native decomp objects*, requiring per-field marshaling between guest layout
(32-bit BE, guest vtables) and host layout (LP64, LE, native vtables). That
boundary is what proved intractable, and it is structural, not incidental.

A STANDALONE recomp has no such boundary: guest layout end-to-end, with overrides
only at HW/OS seams (GX, DVD, PAD, VI, audio) that are already narrow APIs. Same
model as N64Recomp / Zelda64Recompiled. Coherent.

Two things changed since the retirement that materially improve the odds:
1. **We now have an ORACLE.** decomp+Aurora renders the title screen and
   file-select correctly, so a recomp can be verified by in-process differential
   testing per function, instead of "boot it all and hope".
2. **We do not need the whole game hand-ported to be playable** — the recomp runs
   the real code, which is the exact opposite of our current 2/10-stages,
   ~80-gap-actors position.

## Tooling: DolRecomp, not a XenonRecomp fork

XenonRecomp (hedge-dev) is Xbox 360: PPC + **VMX128**, hand-written instruction
impls, TOML config, 32-bit addressing, BE swaps, FPU denormal-mode switching. Good
project — but Gekko has **paired singles**, not VMX, so the expensive ISA work
would have to be written from scratch.

**DolRecomp** (ExpansionPak, public June 2026, GPL-3.0) targets GameCube/Wii/WiiU
(Gekko/Broadway/Espresso) and already implements **236 opcodes including the full
`ps_*` paired-single set and `psq_*` quantized load/store with GQR dequantization**
— precisely the part we would otherwise pay for. It is **CPU-code-only and ships no
runtime**, which is exactly our strength (aurora already provides GX/DVD/PAD/VI/audio).

=> The "do we need two XenonRecomps" question dissolves: **XenonRecomp for a 360
project, DolRecomp for GameCube.** No fork-juggling.

## Spike result: it recompiled all of Super Mario Sunshine, first try

```
git clone https://github.com/ExpansionPak/DolRecomp
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j   # 0 errors
./build/dolrecomp scratch/bin/sms.dol GMSE01 <out>                    # rc=0
```
- input: our US GMSE01 DOL, 4,128,928 bytes
- output: **221 C chunk files**, ~8.4M lines, 217 MB
- **139 self-modifying-code sites** flagged for manual patching
- DolRecomp source itself is modest: 47 files, ~10.5k lines

### The 8.4M lines is an artefact of running WITHOUT a symbol map
Every emitted function opens with a giant `switch (ctx->pc) { case ...: goto ... }`
covering EVERY instruction address, i.e. without function boundaries the tool makes
every instruction a valid entry point. Usage is
`dolrecomp --map <main.map> <main.dol> <ID> <out>`; with a real map it knows
boundaries and should emit far less dispatch. Our `reference/sms_gmse01_funcs.txt`
is a simple `<addr> <mangled symbol>` list and needs converting to the map format
DolRecomp expects — small task, and the next thing to do before judging output size.

## Honest remaining hard parts

1. **OSThread.** SMS is genuinely multi-threaded. Our current runtime deliberately
   DELETED its cooperative scheduler in the one-runtime consolidation, and
   XenonRecomp-style recompilers have no threading story. This has to come back.
2. **Guest runtime**: 24 MB MEM1 + ARAM, BE accessors, MMIO, interrupts.
3. **139 SMC sites** need manual patches.
4. **Compile time / binary size** at this output scale (revisit after the map fix).
5. **GPL-3.0** on DolRecomp. The tool being GPL does not by itself infect its
   output, but check whether it emits GPL'd helper/runtime code inline before any
   distribution plan.

## Standing rule

CLAUDE.md still carries the hard rule "NATIVE-ONLY, NO RECOMP … none will be
reintroduced" (set at the user's delegation, 2026-07-15). This spike is EVALUATION,
which does not require reversing it. If we proceed, that section must be rewritten
deliberately — the old rule's reasoning (the flip boundary) does not apply to a
standalone recomp, and its claim that native-only is *required* for interpolated
60 fps is overstated: `runtime/interp60.h` from the recomp era interpolated by
capturing `J3DModel::viewCalc` matrices.

## ❌ CORRECTION — the retired recompiler WORKED; I mischaracterised it

Everything above about "our retired prototype" was WRONG, and the user corrected it.
I had grepped only `ppc_decoder.cpp` for quoted `"ps_..."` strings, found none, and
concluded "zero paired-single support, a prototype". Both conclusions were false.

What the retired stack actually contained (verified from git at `9283f44^`):

* **A working hand-built Gekko recompiler.** `c_emitter.cpp` has **188
  `case PPCOp::` emitter cases**; `ppc_mnemonic.cpp` has 41 `ps_*` entries. It
  implemented paired singles AND quantized load/store with GQR —
  `PSQ_L/LU/ST/LX/STX` calling `psq_load/psq_store(ea, cpu.gqr[n], w, ...)`, with a
  comment recording the exact lesson that emitting `MEM_R32(ea)/MEM_R32(ea+4)` is
  only valid for float and quantized u8/s8/u16/s16 need narrow, correctly-strided
  access. That is precisely the work I credited DolRecomp for.
* **A native OS/threading layer.** `runtime/native_threads.cpp` (431 lines) plus
  `docs/native_threading.md`, which records that external-interrupt delivery was
  FULLY PC-NATIVE — a behaviour port of OSInterrupt.c walking InterruptPrioTable and
  calling the registered guest handler via `call_ppc`. So OSThread was NOT a
  from-scratch cost, contrary to what I listed as the biggest remaining risk.
* **74 files of native overrides** under `runtime/overrides/`: ngx renderer, native
  JAS audio (jas_driver/aid/dsp_update/cmdnoteon), EFB, matrix, fastboot, widescreen,
  and the interp60 60 fps stack (capture/redraw/verify/replay).

**It was retired by USER DIRECTIVE on 2026-06-18 ("I don't want a recomp"), not
because it failed.** The pivot journal records it live in Delfino at ~2.16M recomp
calls/sec with Dolphin rendering the scene correctly. The thing that was genuinely
intractable was the *flip engine* — recomp↔decomp field marshaling — which is the
hybrid the user is explicitly NOT proposing.

### What this changes about the recommendation

Do NOT adopt DolRecomp as the base. We already have a working recompiler tailored to
this project, with the override architecture built around it. DolRecomp is still
useful as a **cross-check / gap-filler** (236 opcodes vs our 188) but resurrection
beats adoption.

The real delta for a STANDALONE recomp is narrower than I claimed: the old stack ran
on **Dolphin's** substrate (Dolphin JIT dispatch, MMIO, interrupt sources). A
standalone recomp needs its own substrate — which is exactly what **aurora** now
provides (GX, DVD, PAD, VI, audio) and did not maturely provide back then. Plus we
now have the decomp+Aurora **oracle** for per-function differential verification.

So the work is: restore `tools/recompiler` + `runtime/native_threads` + the override
stack, and swap Dolphin's substrate for aurora's — not build a recompiler.

## ✅ Recompiler RESTORED and Dolphin-free (same day)

After the NO-RECOMP directive was removed, `tools/recompiler/` was restored from
`9283f44^` and now builds and runs with **no Dolphin dependency at all**.

Changes needed (small):
* **Dropped `host_stubs.cpp`** — it existed only to satisfy Dolphin's `Host`
  interface when linking Dolphin DiscIO. `disc_loader.cpp` turned out to include no
  Dolphin headers, so it was kept.
* **Restored `runtime/cpu_state.h` + `intrinsics.h`** into `tools/runtime/` so the
  sources' existing `"../runtime/..."` includes resolve unchanged (provisional
  placement — they belong with the recomp runtime once it is stood up).
* **Added a `.dol` input path.** RVZ needs Dolphin DiscIO; in the two-runtime
  architecture aurora owns disc reading and the recompiler only ever needed the
  executable. `recompile_mode` never used the DiscLoader at all (its "disc" hits
  were the words discovery/discovered), so the unused parameter was removed;
  `--analyze-only` still requires a disc and now says so instead of failing oddly.
* Standalone `tools/recompiler/CMakeLists.txt` (`cmake -B build-recomp -S tools/recompiler`).

### Verified output
```
./build-recomp/sunbright-recomp scratch/bin/sms.dol --output scratch/recomp_out
Loading DOL: scratch/bin/sms.dol
Routed 23 HW/privileged functions to Dolphin JIT
Recompiling 6064 functions...
  functions.h (6064 declarations), functions_*.cpp (24 files), jump_table.cpp
```
**6064 functions, 1,099,958 lines, 33 MB.** For scale, DolRecomp on the same DOL
without a symbol map emitted 8.4M lines / 217 MB — ours defaults to
`reference/sms_gmse01_funcs.txt`, so it knows function boundaries and is ~8x tighter.

### The 23 JIT-routed functions = the real remaining HW seam list
`function_needs_jit()` routes only genuine hardware side effects:
`MTSR/MTSRIN` (segment regs), `TLBIE/TLBSYNC` (TLB), `ECIWX/ECOWX` (external
control), and `MTSPR/MFSPR` on unmodeled SPRs (cache / MMU / gather-pipe / power).
In a STANDALONE recomp these need native handling rather than a JIT fallback — most
are tractable (flat guest memory makes MMU/TLB largely no-ops; the gather pipe maps
to aurora's GX FIFO; cache ops become no-ops or explicit flushes).

Note the recorded history in that function: `MTMSR`/`RFI` ARE modeled, i.e. the OS
interrupt/scheduler/context-switch primitives are recompiled ("PC port owns them"),
and a 2026-06-05 note says the post-THP crash is fixed by *finishing native
threading*, not by reverting to JIT. That is the next structural piece.

## ✅ Entire recompiled game COMPILES (same day)

Layout established: `sms-recomp/runtime/` (tracked: cpu_state.h, intrinsics.h) and
`sms-recomp/generated/` (gitignored build output) — chosen so the generated code's
existing `#include "../runtime/..."` resolves with no source edits.

```
./build-recomp/sunbright-recomp scratch/bin/sms.dol --output sms-recomp/generated
ls functions_*.cpp | xargs -P$(nproc) -I{} g++ -std=c++20 -O1 -c {} -o /dev/null -I.
```
**All 24 chunks / 6064 functions / 1.1M lines compile with ZERO errors** (~44 s wall,
1086% CPU). So the recompiler output is well-formed C++20 against the restored
runtime headers — not just "it emitted something".

### The complete runtime surface still to implement (~20 symbols)

From `nm -u` on a compiled chunk:

* **Guest memory** — `g_ram_base`, `g_l1_base`, and the slow/MMIO paths
  `mem_r{8,16,32,64}_slow` / `mem_w{8,16,32,64}_slow`. (The fast paths are already
  inline in intrinsics.h: `sb_r32` etc. do `sb_ram_fast(ea)` + `__builtin_bswap32`.)
* **Dispatch** — `call_ppc(CPUState&, u32)` and `tail_ppc(CPUState&, u32)`.
* **CPU/OS state** — `msr_get()`, `msr_set_raw(u32)` (MSR/interrupt-enable, needed by
  the recompiled scheduler since MTMSR/RFI are modeled), `icbi32(u32)` (icache invalidate).
* **Diagnostics carried over from the Dolphin era** — `g_in_poll_yield`, `g_poll_last`,
  `g_poll_reps`, `sb_poll_fire` (spin-loop/poll detection) and `g_watch_wa`,
  `sb_watch_fire` (memory watchpoints). These can start as no-ops.
* **libm** — `fma`, `sqrt`.
* Plus `psq_load` / `psq_store` (declared in intrinsics.h; not referenced by the
  sampled chunk but needed for paired-single quantized access).

That is the whole boundary between the recompiled game and the host. Standing it on
aurora means backing guest memory with a flat 24 MB MEM1 (+ARAM) allocation, routing
the slow paths to MMIO/hardware, and pointing dispatch at the generated jump table.

NEW DIRECTIVE NOTE: any diagnostics added to this runtime must use **lucent**
(the project-wide C++20 logger), not ad-hoc gated fprintf.

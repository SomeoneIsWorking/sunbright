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
  (compare: our retired prototype was ~2.4k lines with ZERO ps_* support)

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

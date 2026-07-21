# Standalone recomp: boot advance, discovery fixes, override seam (2026-07-21)

Continues `2026-07-21_recomp_feasibility_dolrecomp.md`. Start of session: the recompiled
DOL entered `__start` and immediately aborted on `call to un-recompiled address
0x80342518`. End of session: it runs through OS init and stops in the thread scheduler.

## Discovery bugs found (all general, none address-specific)

1. **Cross-section `bl` targets were silently dropped.** `find_functions` ran per text
   section and its `enqueue` clipped targets to *that section's* address range. `__start`
   lives in `.init` and calls into `.text`, so every one of its callees was discarded and
   the image had no working entry path at all. Fixed by adding `find_call_targets` (the
   same scan, unclipped) and unioning targets across all text sections before emission.
   Cross-section targets go into `real_funcs`, so they also act as `fend` boundaries —
   otherwise the preceding function swallows their body. **+9 functions.**

   Deliberately NOT fixed by adding `0x80342518` to `kForceEntry`: that hides the same
   class of gap everywhere else in a 14k-function image.

2. **Trivial ctors/dtors were rejected as candidates.** The pointer-discovery filter
   required the first word at a candidate to not be a terminator, to reject stray data
   values pointing into `.text`. But an empty C++ function *is* a single `blr`, and one is
   handed to `__construct_array` as the element constructor. Now only padding (`w == 0`)
   and non-`blr` terminators are rejected. **+274 functions.**

3. **Pointer + codeptr discovery are now ON by default.** They were opt-in because under
   Dolphin an un-recompiled callee fell through to the JIT, so extra coverage only bought
   round-trips. Standalone has no JIT and no fallback: a function reached via vtable or
   function pointer that is not in the image is a hard abort. Coverage is a *correctness*
   requirement here, not a perf tradeoff. Opt out with `SUNBRIGHT_NO_DISCOVER_POINTERS` /
   `SUNBRIGHT_NO_DISCOVER_CODEPTRS`.

   **6088 -> 14241 functions.** Unhandled-instruction count went 16 -> 98; all 98 come from
   the codeptr-discovered set, and they now abort loudly rather than being skipped (below).

## Unhandled instructions were SILENT no-ops — fixed

`c_emitter.cpp`'s `default:` case emitted only a `// UNHANDLED:` *comment*. The
instruction's effect was simply dropped and execution continued with corrupt state,
surfacing somewhere unrelated later. This is exactly the banned silent-stub pattern. It
now emits `rt_unhandled_insn(...)` inline at the instruction's own address, which logs
opcode/pc/raw/lr and aborts — the stop happens at the real cause. Under Dolphin the JIT
covered these; standalone there is no fallback, so it mattered immediately.

## Physical-address code fetch

OS code clears `MSR[IR|DR]` and `rfi`s to a **physical** address (`rlwinm rN,rN,0,2,31`
right before `mtsrr0` is the giveaway). The dispatcher is keyed by cached-virtual
addresses, so it saw `0x003464a0` and aborted on an ordinary OS control transfer.
`code_addr_fold` now folds the physical / `0xC` uncached aliases onto the `0x8` window —
which is exactly what the GameCube BATs do, so this is faithful, not a fudge.

## Override seam (the "recomp WITH overrides" architecture)

`sms-recomp/overrides/` — `override_lookup` is consulted at the top of `call_ppc`. That
one place covers direct `bl` and indirect `bctrl` alike, because the generated code routes
every call through `call_ppc`. Each override carries a symbol name and a reason string and
announces itself on first call at `info` level, so a swapped function can never silently
masquerade as working code.

**Gotcha:** overrides self-register from static initializers and export nothing else, so in
a STATIC archive the linker discards the whole object and the override never installs
(observed: no announcement, boot unchanged). They are an **OBJECT** library for this reason.

Landed overrides — both are genuine hardware seams, not "it misbehaved so I skipped it":

- `__OSInitAudioSystem` @ `0x803433b4` — retail polls DSPCR (`0xCC00500A`) for the DSP
  reset handshake. No DSP in a host-audio port, so nothing ever completes it: measured
  **3.97 M polls of `0xCC00500A`** in a 6 s run, a hard hang.
- `__OSInitMemoryProtection` @ `0x803465b8` — programs GC memory-protection hardware.
  Also *structurally* un-recompilable: it `rfi`s to physical, works with the MMU off, then
  `rfi`s back to an address **mid-function** (the instruction after the `bl`). A recompiler
  emitting one C function per guest function cannot resume at an interior address. The
  runtime's `mprotect(PROT_NONE)` poison past 24 MB already gives a stronger version of
  what this function was arranging.

## Where it stops now

```
[override] 0x803433b4 __OSInitAudioSystem -> native
[override] 0x803465b8 __OSInitMemoryProtection -> native
[os:warn]  unhandled syscall at 0x803436b8 (r3=0x80000000)
[rt:error] call to un-recompiled address 0x803487e0 (lr=0x803487e0)
```

`0x803487e0` is **mid-body in `SelectThread` +0x104**, and `lr == address`. This is an OS
**context switch** resuming at an arbitrary PC (the `sc` at `0x803436b8` is `OSLoadContext`).
Same structural limit as `__OSInitMemoryProtection`, but this one is not a one-off boot
routine — it is the scheduler, so it cannot be no-op'd.

**Next arc: restore the native OSThread overrides.** They existed and worked before the
recomp was retired — `runtime/native_os.cpp` (449 lines) + `docs/native_threading.md` at
`9283f44^`, covering `OSCreateThread` / `OSResumeThread` / `OSSuspendThread` /
`OSGetCurrentThread` backed by host threads. That is the documented reason the scheduler
never needs to run: the guest never context-switches because host threads do it.

Note this is a *recomp-runtime* decision and does not touch the ONE RUNTIME single-thread
doctrine, which governs the decomp+Aurora runtime.

## Tooling

`tools/recompiler/whereis.py <addr>...` — symbol, containing function, whether the address
is recompiled or swallowed (and by what), whether the previous word is a terminator (i.e.
is it a real function start or mid-body), and a short disassembly. Each boot blocker is an
address and this answers all of that in one call.

## Honest status

The boot does not reach GX yet, so title-screen / file-select oracle matching is still
downstream. The decomp+Aurora runtime remains the renderer and the oracle.

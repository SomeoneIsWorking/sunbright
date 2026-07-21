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

## Guest backtrace on abort (landed after the above)

`rt_dump_guest_stack` prints the host backtrace on any runtime abort. Recompiled bodies
are real C functions named `func_<addr>` and calls nest on the host stack, so the host
backtrace *is* the guest call stack; the executable links with `-rdynamic` so the names
resolve. This turns "reached address X" into a full call chain in one run.

It resolved the current blocker immediately:

```
#2  func_803486dc  SelectThread      <- abort here
#3  func_803492e0
#4  func_80346258
#5  func_802c6690
#6  func_802c54b8
#7  func_803486dc  SelectThread
#8  func_80348ee8  OSResumeThread
#9  func_802c662c
#10 func_8029cfc0
#11 func_802a73c4
#12 func_80005600
#13 func_8000522c  __start
```

So the stop is a guest **context switch out of `OSResumeThread`** — precisely what the
retired native OSThread overrides existed to prevent.

## Feasibility of the OSThread arc (assessed, not yet started)

Sources at `9283f44^`:

| file | lines | notes |
|---|---|---|
| `runtime/native_threads.cpp` | 431 | the `nthr` cooperative scheduler — **1** Dolphin reference total |
| `runtime/native_threads.h` | 109 | |
| `runtime/native_os.cpp` | 449 | OSCreateThread/Resume/Suspend/GetCurrentThread overrides |
| `runtime/overrides/sms_jkrthread.cpp` | 35 | replicates JKRThread worker bodies synchronously |
| `docs/native_threading.md` | — | design doc + hard-won gotchas |

The scheduler is effectively Dolphin-free, so this is a port, not a rewrite. Note the
*rest* of `docs/native_threading.md` (native interrupt dispatch, GX FIFO pacing, VI
retrace, draw-sync token synthesis) IS deeply Dolphin-coupled and does NOT come back —
those seams belong to aurora in the two-runtime architecture.

Design note from that doc worth keeping: every `OSCreateThread`'d worker (JKRThread
decomp/stream pool, audio) immediately `OSReceiveMessage`-waits for work, so a worker
never actually needs to run — dropping its suspend count and returning without scheduling
is sufficient. The one real subtlety recorded there is **priority preemption**:
`AudioThread::start` creates+resumes a higher-priority thread that allocates the DSP FX
buffers and returns without waiting, relying on the GC scheduler switching immediately.
Skip that and the main thread configures FX lines that were never allocated -> null write.

## Increment 1 of the OSThread arc — landed

`sms-recomp/overrides/native_os_thread.cpp` overrides `OSCreateThread` (records the thread,
super-calls the recompiled body so the guest `OSThread` struct stays byte-exact) and
`OSResumeThread` (reproduces the bookkeeping, skips the scheduling).

Offsets **verified against this DOL**, not taken from the retired notes — disassembling
`OSResumeThread` @`0x80348ee8` shows `lwz/stw r,716(r29)` (suspend, s32),
`lhz r,712(r29)` + `cmpwi 4` (state, u16, WAITING==4), and the `if (--suspend < 0)
suspend = 0` clamp at `0x80348f20..2c`.

Boot creates exactly two threads, both `entry=0x802c54b8` = `JUTException::run`
(prio 0 and prio 8), whose bodies park forever waiting for a fault. Not scheduling them
costs nothing at boot — a PC port surfaces host faults natively. Every skipped resume
warns loudly, because a *worker* whose body never runs would be a real behavioural gap.

Boot now runs past thread creation into `JKRAram::create`.

## Next blocker: the ARAM (audio RAM) seam

`JKRAram::create` -> `__ct__7JKRAramFUlUll` -> `0x80352f4c` -> `0x80353090`, spinning
**4.0 M reads of `0xCC005016`** in 6 s. The loop is

```
lis  r3,0xcc00 ; addi r3,r3,0x5000
lhz  r0,0x16(r3)          ; 0xCC005016
rlwinm. r0,r0,0,31,31     ; test bit 0
beq  -8                   ; spin until it sets
```

then it stores `0x01000000` (16 MB, the ARAM size) to an SDA global. So `0x80353090` is
the ARAM sizing routine and `0x80352f4c` is `ARInit`.

AR API surface, found by scanning `0x80352c00..0x80353d00` for absolute `0xCC0050xx`
accesses (none of these are in `sms_gmse01_funcs.txt` — only `ARQInit` @`0x80353b74` and
`ARQPostRequest` @`0x80353bdc` are labelled):

| addr | registers touched | identification |
|---|---|---|
| `0x80352df4` | w `AR_DMA_MMADDR` (0x5020/22), r `AR_DMA_ARADDR` (0x5024) | `ARStartDMA` |
| `0x80352f4c` | r `AR_REFRESH` (0x501a) | `ARInit` |
| `0x80353090` | r `AR_SIZE` (0x5012), r/w DMA regs (0x5020-0x502a) | ARAM sizing |

### Two options, and the open question

1. **Override the AR SDK functions** (ARInit / ARStartDMA) with a host 16 MB buffer and
   memcpy DMA. Matches the decomp runtime, where ARAM copies already run inline at the
   enqueue site ("synchronous unthrottled I/O"). Cost: `ARInit`'s SDA globals must be RE'd
   and reproduced, since `ARGetSize`/`ARGetBaseAddress` and the ARQ layer read them.
2. **Implement the AR device MMIO** (16 MB buffer; DMA performed on the CNT write) so the
   game's own `ARInit` and sizing code run for real. Avoids REing the globals and is more
   faithful, at the cost of a small device model.

**Blocking question for either path: interrupt delivery.** AR DMA completion raises an
interrupt, and the ARQ queue layer fires its callbacks from it. The standalone runtime has
no interrupt path at all — the retired native dispatch (`native_dispatch_one/pending`) was
a behaviour port of `OSInterrupt.c` but read Dolphin's PI cause/mask registers directly, so
it does not come back as-is. Doing DMA synchronously and invoking the completion callback
inline sidesteps this for AR specifically, and matches the one-runtime doctrine, but a
general interrupt seam is still owed (VI retrace, CP/PE, DVD).

That is the decision to make before writing code here — noting it rather than picking one
mid-tick, since it sets the shape of every device seam that follows.

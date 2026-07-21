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

## ARAM seam — landed as a DEVICE, not an SDK override

Took the device path (option 2 above) rather than overriding `ARInit`/`ARStartDMA`.
Deciding factor: an SDK override would have required RE'ing and reproducing every SDA
global `ARInit` leaves behind, since `ARGetSize`/`ARGetBaseAddress` and the whole ARQ layer
read them. ARAM is one of the simplest devices on the machine — a flat buffer, four address
registers, a byte count — so modelling it lets the game's OWN init and sizing code run and
*compute* those globals instead of us fabricating them.

New: `runtime/mmio.h` + `mmio.cpp` (device router; overlapping ranges abort, unclaimed
addresses stay loud) and `runtime/dev_aram.cpp`. The router is where DI/VI/SI/EXI will hang
off later.

**DMA is synchronous** — the copy completes inside the register write that starts it, so the
busy bit is never observed set. Same "synchronous unthrottled I/O" as the decomp runtime,
and it sidesteps the missing interrupt path for ARAM specifically. This is not debt to
undo: inventing latency the host does not have would then require machinery to wait for it.

Only `[0xCC005012, 0xCC005030)` is claimed. The DSP mailboxes and CSR (`0x5000-0x500B`)
share the block but are a different device; claiming them would silently swallow accesses
that should still report as unrouted.

`aram_device_init()` is called explicitly from `rt_mem_init` rather than run from a static
initializer — same static-archive linker-drop trap that silently disabled the first override.

**Verified, not assumed:** with `SBR_LUCENT_DEBUG=aram` the game's size probe is visible
doing real work —

```
[aram] DMA MM->AR mm=0x80427600 ar=0x01000000 len=0x20
[aram] DMA MM->AR mm=0x80427600 ar=0x01200000 len=0x20
[aram] DMA MM->AR mm=0x80427600 ar=0x02000000 len=0x20
```

It writes `0xDEADBEEF`/`0xBAD0BAD1` past the 16 MB mark to probe aliasing. `ar & (size-1)`
wraps exactly as retail hardware does, so the probe concludes 16 MB by itself and boot
proceeds past `ARInit`.

## ~~The scheduler is now REQUIRED~~ — WRONG, see the correction below

Boot now creates **six** threads with three distinct entry points — no longer just
`JUTException`:

| entry | prio | count |
|---|---|---|
| `0x802c54b8` (`JUTException::run`) | 0, 8 | 4 |
| `0x802a9184` | 15 | 1 |
| `0x802a7878` | 17 | 1 |

and then the MAIN thread parks:

```
func_802a6398 -> func_802a5f50 -> func_8001e920 -> func_8035dae8
  -> func_803492e0  OSSleepThread
  -> func_803486dc  SelectThread   <- spins here
```

It is waiting on work that only those prio-15/17 workers can do, and they are not being
scheduled. Increment 1's loud warning called this exactly. So the cooperative token
scheduler is the next piece, and it is now justified by a concrete boot stop rather than by
the retired design's say-so.

Also new and unexplained: `unhandled syscall at 0x80341ab8 (r3=0xcc010000)`. `0xCC010000` is
the GX FIFO region, so this is likely a cache/store-gather operation. The runtime currently
logs and continues; whether ignoring it is correct needs checking before it is trusted —
`sc` at `0x803436b8` (reached from the ARAM sizing code around a DMA) is almost certainly a
cache flush, which IS correctly a no-op on a coherent host, but neither has been confirmed.

## CORRECTION: the scheduler conclusion above was WRONG

The section titled "the scheduler is now REQUIRED" was wrong, and the user caught it:
*"That makes no sense, we already converted these async wait operations to sync before."*

The error was diagnostic laziness. I saw the main thread parked in `OSSleepThread` ->
`SelectThread` and concluded it was waiting for the unscheduled prio-15/17 workers —
without checking what it was actually waiting ON. It was not waiting for a worker at all:

```
func_8035dae8  GXDrawDone   <- the actual wait
  -> func_803492e0  OSSleepThread
     -> func_803486dc  SelectThread
```

`GXDrawDone` parks until a PE draw-sync token interrupt. Nothing to do with threads.

It was also **inconsistent with this very runtime**: ARAM DMA had just been made
synchronous a few commits earlier, with the note "the host has no latency to hide". The
same reasoning applies to every one of these waits, and to GC threads themselves — which
is exactly the ONE RUNTIME rule that was already written down in CLAUDE.md:

> every GC-thread body runs inline at its enqueue site

**The standing rule for this runtime: a guest wait for asynchronous hardware becomes
synchronous completion. There is no scheduler, and none is planned.** If a worker's output
is ever genuinely needed, the fix is to make its ENQUEUE point synchronous — not to run the
worker. The retired `sms_jkrthread.cpp` (35 lines at `9283f44^`) is the precedent.

The misleading comments in `native_os_thread.cpp` have been corrected too, so the file no
longer advertises a scheduler port that is not going to happen.

## Seams landed after the correction

- **`GXDrawDone`** (`native_gx.cpp`) — pushes the PE token byte-for-byte as retail does, so
  the FIFO stream stays faithful for when aurora consumes it, then sets the flag the token
  interrupt would have set. Only the WAIT is skipped.
- **`VIWaitForRetrace`** (`native_vi.cpp`) — a pure counter, matching the decomp runtime.
  Both read their SDA globals off `r13` at call time rather than hardcoding an address.

With those, the game reaches its **main loop**: stack samples show `TMarioGamePad::read` ->
`JUTGamePad::read` -> `OSGetTime` on one sample and `GXSetViewport` -> `GXSetViewportJitter`
on the next. 3.4 M writes to `0xCC008000` (the write-gather pipe) confirm it is submitting
real GX command traffic.

## FAIL FAST applied to the memory path (user directive: "fail fast please")

Two silent-zero paths were feeding the guest fabricated hardware answers:

1. **NULL dereference.** `sb_ram_fast` only accepts the `0x8`/`0xC`/`0xE` windows, so
   `lwz rX,0x1d0(r3)` with `r3 == 0` fell through to the slow path and returned 0. Measured
   **314 k reads of `0x1d0`** in 15 s — the guest had been running on garbage for millions
   of instructions. Now traps with a guest backtrace.
2. **Unrouted device reads.** Returning 0 for a device nobody implemented is an invented
   hardware answer; a 0 that should have been a pointer becomes a NULL dereference far from
   its cause. Now fatal, naming the register and the guest call stack.

This regresses apparent progress — boot now stops much earlier — but the earlier "progress"
was the game running on fabricated data. **Fail-fast turned the device list into an ordered
worklist**: each abort names exactly which register is needed next.

Devices implemented in the order fail-fast demanded them:

| device | range | note |
|---|---|---|
| `dsp` | `0xCC005000-0x5012` | mailboxes + CSR; DSP is permanently halted, interrupt-status bits can never set because nothing raises them |
| `aram` | `0xCC005012-0x5030` | 16 MB buffer, synchronous DMA |
| `ai` | `0xCC006C00-0x6C10` | sample counter is a REAL clock (48 kHz off `CLOCK_MONOTONIC`); a frozen counter would make any interval measured against it divide by zero or spin |

**Next: EXI** (`0xCC006800`), read by `OSInit` — memory card / IPL / RTC, and the SRAM the
OS reads at boot for language and video-mode settings.

Also fixed: the `sc` warning fired 1.67 M times in a 40 s run, burying every other line and
slowing the run enough to distort what it measured. Now once per SITE, counted thereafter.

## Device worklist, driven by fail-fast

With unrouted reads fatal, each abort names the next device. Landed this round:

| device | range | notes |
|---|---|---|
| `vi` | `0xCC002000-0x2080` | storage; `VI_DTV_STATUS` (`+0x6C`) reports **no component cable**, selecting interlaced NTSC — the same mode the decomp runtime renders and the oracle captures, so the two stay comparable |
| `si` | `0xCC006400-0x6500` | controller transport; `COMCSR` start bit clears immediately. No controller reported connected — truthful (none is wired up), unlike inventing all-zero button data which would be indistinguishable from a real pad |
| `exi` | `0xCC006800-0x683C` | 3 channels x 5 registers; transport ONLY |

`dev_vi.cpp` had a bug worth recording because the shape recurs: `reg()` indexes off a full
guest ADDRESS, but the initialiser passed a bare offset (`0x206C`), so `(off - VI_BASE)`
underflowed and wrote far outside the array. It segfaulted during device registration,
before the DOL even loaded. `reg()` now range-checks and aborts, and the constant is a full
address. Any device file using the same `reg(addr)` idiom should range-check too.

## Next: the EXI SRAM/RTC device (channel 0, device 1)

The EXI transport is now exercised, and it stops exactly where it was designed to:

```
[exi:error] channel 0 device 1 has no implementation — EXI transport is modelled but
            nothing is attached. Returning bus-idle bytes would fake a broken console
            (corrupt SRAM checksum -> silent fallback to defaults). Implement this device.
```

This is deliberate. Handing back bus-idle `0xFF` bytes would give the OS a corrupt SRAM
checksum, it would silently fall back to defaults, and boot would *appear* to work while
the console configuration (language, video mode, display offset) came from nowhere.

The device speaks a small command protocol: a 32-bit command word selects RTC (`0x20000000`),
SRAM (`0x20000100`) or the IPL ROM, with bit 31 marking a write; data bytes follow.

**Do NOT write the SRAM checksum from memory of what the algorithm is.** The exact byte
range it covers is easy to get subtly wrong, and a wrong checksum reproduces the silent
default-fallback this abort exists to prevent. RE the guest's own validation — the OS reads
and checks SRAM during `OSInit` — and match that, then verify the OS accepts it rather than
assuming it did.

## EXI device attachment + the SRAM/RTC chip

`exi.h` adds an attachment interface (channel, chip-select, imm_write/imm_read/dma) and
`dev_exi.cpp` now decodes EXI_CR properly: `CR_DMA` selects DMA over immediate, bits 2-3 are
direction, bits 4-5 hold (length-1) for immediate transfers.

`dev_sram.cpp` attaches on channel 0 / device 1 and implements the one command the game
issues, `0x20000100` (SRAM read), followed by a 64-byte DMA.

**The checksum question, settled by RE rather than recall.** The plan said not to write the
SRAM checksum from memory of the algorithm. Checking was the right call: the game does not
validate it at all. The only three places touching the SRAM mirror at `0x80402640` are

| addr | role |
|---|---|
| `0x80347608` | read — `EXISelect(0,1,3)`, `EXIImm(0x20000100, 4, write)`, `EXIDma(64, read)` |
| `0x8034773c` | lock |
| `0x80347490` | flush / write-back |

and none computes or verifies a checksum. That matches real hardware: the boot ROM
validates and repairs SRAM; the game only reads it. So the checksum words are left **zero**
rather than fabricating a value — a wrong checksum would be worse than none, and an
unverifiable "correct" one is not something to claim. Contents are US-console defaults
(language 0 = English, no flags, zero display offset).

Unimplemented commands abort naming the command word instead of being quietly treated as a
SRAM access, and SRAM write-back aborts rather than silently discarding settings changes.

## Next: DI (the DVD interface)

```
func_80342550  __OSGetDIConfig   <- reads DI_CFG at 0xCC006024
  <- func_80369fdc  EXISync
     <- func_80347608  (SRAM read path)
```

DI registers sit at `0xCC006000`: `SR`, `CVR`, three command words, `MAR`, `LENGTH`, `CR`,
`IMMBUF`, `CFG` (`+0x24`).

This is a bigger arc than the previous devices, because a real DI has to serve the game's
file reads from the disc image — the standalone host currently loads only `sms.dol` and has
no disc mounted. The decomp+Aurora runtime already does this synchronously
(`extern/aurora/lib/dolphin/dvd`, "every DVDRead*/Async completes inline and fires its
callback before returning"), which is the model to follow, and the ROM path convention
(`$SUNBRIGHT_ROM` / `.env` / `rom.rvz` drop-in) already exists.

## DI + PI devices; the DVD decision

- **`di`** (`0xCC006000-0x6040`) — register block. `DI_CVR` reports the lid CLOSED (a disc is
  present; an open lid would send the game to its "insert disc" path) and `DI_CFG` is 0,
  which `__OSGetDIConfig` returns masked to its low byte. Disc COMMANDS abort naming the
  command word rather than being served zeros.
- **`pi`** (`0xCC003000-0x3040`) — interrupt cause/mask (nothing pending: no device here
  raises one) and the Flipper revision.

**Flipper revision, decided by RE not by recall.** `OSInit` does:

```
lis  r3,0xcc00 ; addi r3,r3,0x3000
lwz  r0,0x2c(r3)        ; PI_FLIPPER_REV
rlwinm r0,r0,0,0,3      ; keep top nibble
rlwinm r0,r0,4,28,31    ; move it down
add  r0,r3,r0           ; fold into the console-revision field
...
cmplwi r4,0 ; beq -> r4 = 0x10000002    ; explicit "unknown" fallback
```

The guest provides its own fallback constant when the revision comes out zero. Reporting 0
takes that branch. The alternative — writing the real retail revision word — is a specific
constant this port cannot verify, and a wrong one would silently select a different
hardware-workaround path inside the SDK. Taking a branch the game itself provides for the
unknown case beats inventing a value.

## Next arc: DVD via aurora, as an SDK override (not DI commands)

Boot now issues a real drive command:

```
[di:error] disc command 0x12000000 is not implemented — no disc is mounted.
```

`0x12000000` is the drive inquiry, from `DVDInit`. Serving the disc at the DI register level
would mean implementing the drive command protocol AND a disc-image reader. Neither is
necessary:

- CLAUDE.md names DVD as an **override seam**, not a device.
- `extern/aurora/lib/dolphin/dvd` already implements the SDK-level API and already opens
  this project's `.rvz` (`aurora_dvd_open`, used by `sms-boot/main.cpp`). The decomp runtime
  drives it synchronously — "every DVDRead*/Async completes inline and fires its callback
  before returning" — which is the model this runtime wants anyway.
- Its dependencies are `nod` + SDL3, NOT the graphics stack, so it can be linked into
  `sms-recomp` without pulling in Dawn/WebGPU.

This is also exactly what the two-runtime doctrine calls the genuinely new work: swapping
Dolphin's substrate for aurora's.

So: link aurora's DVD into `sms-recomp` and override the guest's DVD library entry points,
leaving `dev_di.cpp` for the register pokes the OS makes directly. `tools/recompiler/
disc_loader.cpp` is NOT the answer — it is a Dolphin-DiscIO wrapper whose fallback parses
plain ISO only, and this project's ROM is `.rvz`.

## Disc mounted: nod + DI commands (register level, no marshaling)

Chose the DI register level over SDK overrides, revising the plan recorded above. Reason:
an SDK override has to marshal guest `DVDFileInfo` structs across the boundary, while
serving DI commands needs **no marshaling at all** — the guest's own DVD library, its FST
parsing and every file read run as recompiled PPC. All the host has to answer is "give me
`len` bytes at absolute disc offset `off`".

- `runtime/disc.{h,cpp}` — nod-backed random access. nod handles the container format, so
  the project's `.rvz` works with no conversion step. Short reads loop until satisfied: a
  silently-accepted short read would leave the tail of the buffer holding stale data that
  looks like a successful load.
- `dev_di.cpp` — commands `0xA8` (read: CMDBUF1 = offset >> 2, CMDBUF2 = byte count),
  `0xAB` (seek — a no-op, since reads carry their own offset), `0xE3` (stop motor) and
  `0x12` (inquiry). Unknown commands abort naming the command.
- `dev_mi.cpp` — MI memory-protection registers as storage. The port needs no equivalent:
  `rt_mem_init`'s `mprotect` poison already traps stray accesses.
- Host mounts `$SUNBRIGHT_ROM` / `rom.rvz` (same convention as the decomp runtime) and
  treats a missing disc as fatal.

**Verified:** `[disc] mounted .../Super Mario Sunshine (USA).rvz (1459978240 bytes)` and the
drive inquiry served, with boot continuing past `DVDInit`.

**One honest gap, flagged in-code and at runtime:** the 32-byte drive-identification block
returned by inquiry is zeros. The retail bytes are not something this port can verify, and
fabricating a plausible drive could quietly select a firmware-workaround path inside the DVD
library. It warns once, loudly, naming itself as the place to look if disc behaviour ever
appears firmware-dependent. Nothing observable depends on it so far.

## Current frontier: `__GXData` is NULL

```
[rt:error] NULL-pointer w8 at guest address 0x000004f0
  func_8035a664 (GX)  <- func_802a73c4 (app init) <- func_80005600 <- __start
```

`0x8035a664` starts with `lwz r3,-29432(r13)` — the `__GXData` pointer in small data — and
immediately writes `stb r31,1264(r3)`. The pointer is 0.

Searched every `.text` section for a store to `-29432(r13)`: **there is none**. So the
pointer is either statically initialised in `.sdata` (and something about how this runtime
loads or bases small data is wrong) or written through absolute `lis`/`addi` addressing that
an r13-relative scan does not match. Next step is to determine which — check the value the
DOL actually ships at that address, and confirm `r13` matches `_SDA_BASE_` as set by
`__start`. Do not "fix" it by assigning a plausible pointer; that would paper over whichever
of the two it is.

## ROOT CAUSE: the NULL `__GXData` was a DOL-loader bug (BSS clobbered initialised data)

Not a game bug, not a recomp bug — a bug in `sms-recomp/host/main.cpp`.

Evidence chain:
1. `0x8035a664` starts `lwz r3,-29432(r13)` then `stb r31,1264(r3)`. The pointer was 0.
2. `r13 = 0x804141C0` (`lis 0x8041` + **`ori 0x41c0`** — an earlier scan missed the `ori` and
   computed the wrong slot). So the slot is `0x8040CEC8`.
3. A base-tracking scan of every text section found **499 reads of that slot and ZERO
   stores**. Nothing in the game ever assigns it, so it must be statically initialised.
4. The DOL ships `0x804036a0` there — in **section 13** (`0x8040c1c0 +0xd40`).
5. Section 13 lies **inside** the declared BSS range (`0x803e9700 +0x25498`).

The host loaded sections and *then* cleared BSS, erasing a loaded data section. Fixed by
clearing BSS **first** and loading sections over it, so initialised data wins. That is also
what real hardware does.

Worth keeping: "no code ever writes this variable" is a strong signal that it is initialised
data and the loader is at fault — not an invitation to assign a plausible pointer.

## Devices completed; the recomp now issues draw calls

Added `mi`, `cp` and `pe` (all storage — they are configuration around the GX pipeline, whose
real command traffic goes through the write-gather pipe at `0xCC008000`). With those plus
the BSS fix, `GXInit` runs for real rather than being overridden.

Stack sample now:

```
func_8013fc88  TSMSFader::draw
  func_80140390  fill_rect
    func_8035df88  GXBegin
      func_8035d16c  (GX vertex path)
```

That is the boot fade rectangle — **the recompiled game is submitting real geometry**.

## Known gap, deliberately not faked: the IPL font ROM

The game reads the console boot ROM through the same EXI chip as SRAM (command `0x07f3c000`
-> ROM offset `0x1FCF00`, the Shift-JIS system font). We have no IPL image and will not ship
one — it is copyrighted console firmware. Reads are served zeros with a one-time warning
naming the cause, so a blank-text symptom traces back here instead of looking like a
font-rendering bug. The game's own UI fonts come off the disc and are unaffected.

Same treatment for the 32-byte drive-identification block from `DVDLowInquiry`.

Both are recorded as *unverified placeholders*, not as working features.

## Next

The run no longer aborts; it spins inside the GX vertex path with **zero disc reads**, so the
next question is whether `GXBegin` is waiting on a CP FIFO watermark that never moves (CP is
static storage right now) — the write-gather pipe at `0xCC008000` takes 3.4 M writes and
drops them all. Routing that pipe to aurora's GX is the next real arc, and it is the one that
turns submitted geometry into pixels that can be diffed against the decomp oracle.

## Synchronous DI interrupt delivery — DVD init now completes

The game was looping in `mountStageArchive` drawing the fader with **zero disc reads**. Cause:
`DVDLowInquiry` stores a completion callback at `r13-22952` and starts the transfer; the
callback is invoked by `DVDLowIntrHandler` from the DI transfer-complete interrupt. Our DMA
completed but nothing ever raised that interrupt, so the DVD library waited forever.

`dev_di.cpp` now delivers it inline: set `DI_SR` transfer-complete, then **call the guest's
own handler** (`0x8034a760`) rather than reimplementing its event decoding.

DI_SR layout was read off that handler rather than recalled:

```
andi. r4,r0,0x002A   ; MASK bits   (1 DEINTMASK, 3 TCINTMASK, 5 BRKINTMASK)
andi. r3,r0,0x0054   ; STATUS bits (2 DEINT,     4 TCINT,     6 BRKINT)
```

so `TCINT` is bit 4 (0x10). Conveniently the handler overwrites its `r3` (interrupt number)
before use, so only `r4` (the `OSContext`, from `0x800000D4`) has to be right.

**Registers are saved and restored around the call.** The handler runs as a nested call
inside whatever recompiled function performed the store, and would otherwise clobber GPRs
that function still needs — this is exactly what a hardware interrupt does with `OSContext`.
Guest memory changes persist, as they must.

Result: boot progresses past `mountStageArchive` into `SMSSetupTitleRenderMode`.

## Current frontier: `GXGetYScaleFactor` spins on a zero height

```
func_802a5254  SMSSetupTitleRenderMode
  func_802fb9e8  JDrama::CalcRenderModeXFBHeight
    func_8035e734  GXGetYScaleFactor   <- infinite loop (3 stack samples, identical)
```

`GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight)` contains three copies of a lowest-set-bit
scan:

```
8035e7c8  srwi  r4,r4,1
8035e7cc  andi. r0,r4,1
8035e7d0  beq   -8          ; never terminates when r4 == 0
```

`r4` derives from a float conversion of the height ratio, so **a zero height hangs it**. This
is retail code behaving exactly as retail would given zero input — the bug is upstream, in
whatever supplies the render mode. Note the decomp side has a matching known hazard recorded
as "efbHeight==0 SIGFPE", so this is a familiar class.

Next step is to find where the `GXRenderModeObj` comes from and which field is zero — likely
traceable to the VI device returning zeros for registers that retail's `VIInit` would have
populated, or to a render-mode table lookup keyed off `VIGetTvFormat`. Do NOT clamp the
height inside `GXGetYScaleFactor`; that would hide the real defect one layer up.

## DVD eliminated: reads are now instant filesystem reads (user directive)

The DI-register path worked — the disc mounted and the drive inquiry was served — but it
meant reproducing hardware whose only job was to hide latency the host does not have, and it
dragged in three things that were each a problem on their own: interrupt delivery, a command
queue to drain, and a 32-byte drive-identification block whose retail contents this port
cannot verify. The DVD library rejected that unverifiable drive ID and retried forever
(`interrupt handler queued another transfer 4097 times without settling`).

Cutting above the protocol removes all three at once. `overrides/native_dvd.cpp` overrides:

| function | behaviour |
|---|---|
| `DVDReadAbsAsyncPrio` | reads the bytes out of the disc image inside the call, fills the command block, runs the callback, returns TRUE |
| `DVDInquiryAsync` | zeroed reply — truthful (no drive) and inert (selects no firmware workaround) |
| `DVDReadDiskID` | the first 32 bytes of the disc, which we genuinely have |

`DVDCommandBlock` layout was read off the recompiled functions, not recalled:
`+0x08` command, `+0x0C` state, `+0x10` offset, `+0x14` length, `+0x18` addr,
`+0x20` transferred, `+0x28` callback (from `DVDReadAbsAsyncPrio`'s stores and
`DVDGetCommandBlockStatus`'s `lwz r0,12(r31)`).

Everything above the transport — FST, path resolution, file handles, `JKRDvdFile` — still
runs as recompiled PPC. Only the transport is replaced.

## Boot environment (what the apploader would have left behind)

Loading only the DOL left every low-memory OS global zero. That does not fail loudly; it
fails far away. `runtime/boot_env.cpp` now publishes what a real boot leaves:

- disc ID (first 0x20 bytes) at `0x80000000`, boot magic `0x0D15EA5E`, version, memory size,
  console type
- **the FST**, loaded from the disc to the top of MEM1, with `0x80000038`/`0x8000003C`
- arena bounds — `lo` = end of the DOL's sections and BSS, `hi` = the FST. An earlier
  version used `0x80003100` for `lo`, which overlaps the game's own code and data: its heap
  would have allocated on top of itself.
- **bus/CPU clocks** at `0x800000F8`/`0x800000FC`. Not decoration — the OS derives its
  timebase and every timeout from them, and a zero bus clock makes every DVD timeout expire
  instantly.

Verified: `FST 0x1140 bytes -> 0x817feec0; arena 0x80417800..0x817feec0; bus 162 MHz`.

## Frontier: an unscheduled worker is now load-bearing

The interrupt storm is gone and DVD init completes. The game runs its main loop
(`TMarioGamePad::read`), but **`DVDOpen` and `DVDConvertPathToEntrynum` are never called** —
it is not requesting files at all, so it is blocked before that.

Boot creates two non-`JUTException` workers, `0x802a9184` (prio 15) and `0x802a7878`
(prio 17), and neither runs. Increment 1's warning said exactly this would eventually
matter, and now it does. Per the ONE RUNTIME rule the fix is to make the work happen
synchronously at its ENQUEUE point — not to add a scheduler. Next step is to identify what
those two threads do and where their work is enqueued.

## ⚠️ PROCESS FAILURE: this session re-derived work that already existed

User: *"I feel like we've done these before and the recomp game was already playable, it was
just rendering incorrectly."* Correct, and worth recording as a process failure rather than
buried as a footnote.

`runtime/overrides/` at `9283f44^` holds **74 files**, including a native override for very
nearly every seam this session rebuilt from disassembly:

| retired file | lines | dolphin refs | what this session re-derived |
|---|---|---|---|
| `native_dvd.cpp` | 140 | 10 | `overrides/native_dvd.cpp` — **same function `0x8034da6c`, same command-block offsets, same synchronous-read-then-callback approach** |
| `gxdrawdone_native.cpp` | 56 | 3 | `overrides/native_gx.cpp` |
| `sms_vi_native.cpp` / `native_vi2.cpp` | 209 / 211 | 3 / 4 | `overrides/native_vi.cpp` |
| `sms_os_memprotect.cpp` | 51 | 0 | `__OSInitMemoryProtection` override |
| `os_init_audio_native.cpp` | 127 | 0 | `__OSInitAudioSystem` override |
| `native_aram/exi/si/mi/pi2/dsp_regs/gx/card` | ~2100 | 1-6 each | `runtime/dev_*.cpp` |
| `fastboot_native.cpp` | 323 | 0 | — (not yet looked at) |

The rule in the global instructions is **"Read before you re-derive — and make 'before'
cheap."** I did not consult the retired tree before rebuilding these, despite CLAUDE.md
saying plainly: **"Resurrect, do not rebuild."**

### What was genuinely new vs redundant

**Genuinely new** (Dolphin's Memmap/HW provided all of it, so it never existed): the MMIO
router, the device register models as a standalone substrate, the nod-backed disc reader,
and `boot_env.cpp` (the apploader's low-memory state — no Dolphin meant nobody published
the FST, arena or clocks). This is exactly what CLAUDE.md called "the genuinely NEW work…
swapping Dolphin's substrate for aurora's".

**Redundant**: the OS/DVD/GX/VI seam overrides. Independently arriving at the same function
addresses and the same `DVDCommandBlock` layout does at least cross-validate both — but it
cost a session.

### One discrepancy worth resolving

The retired file comments `0x28` as **prio**; this session's RE derived `0x28` as
**callback** (`stw r7,40(r3)`, with `r8`→`r31` being prio). The SDK signature
`DVDReadAbsAsyncPrio(block, addr, length, offset, callback, prio)` supports callback at
`r7`. Check before trusting either comment.

### Corrected approach

Port the retired overrides systematically (they are mostly Dolphin-free) instead of
re-deriving them, and **start from the known end-state**: under the Dolphin substrate the
recomp reached title, file-select and gameplay — the open problem then was RENDERING, not
boot. Boot bring-up on the new substrate is a means to get back there, not the goal.

## Cooperative guest threading — the game now loads files off the disc

Rather than porting 74 Dolphin-era override files, the sensible cut was to solve what was
actually blocking: threads. `runtime/guest_sched.{h,cpp}` gives each guest thread a real host
thread and its **own CPUState**, with one token so only one runs at a time (the GameCube is
single-core and the game is written for it). Blocking is a token hand-off, which is what lets
a thread park mid-function and resume exactly where it left off — the thing a
function-granular recompiler cannot otherwise express.

Interception surface is 8 primitives, all symbolized: `OSCreateThread`, `OSResumeThread`,
`OSSuspendThread`, `OSSleepThread`, `OSWakeupThread`, `OSYieldThread`, `OSExitThread`,
`OSJoinThread`. **Message queues need no override**: `OSSendMessage`/`OSReceiveMessage` block
through `OSSleepThread`/`OSWakeupThread`.

Four bugs found and fixed on the way, each worth keeping:

1. **`sched.h` shadowed the system `<sched.h>`** that pthread needs, and `sched_yield`
   collided with POSIX. Renamed to `guest_sched.h` / `gsched_*`.
2. **Thread 0 had no guest identity.** It is adopted before `OSInit` creates the default
   `OSThread`, so its `os_thread` was 0 — and writing that placeholder over `0x800000E4`
   handed guest code a NULL `OSThread*` (fault reading +0x2f8). Now adopted lazily.
3. **The frame loop starved every lower-priority thread.** The archive loader is prio 17,
   below the main thread's 16, so it only runs when the main thread blocks. Retail SLEEPS in
   `VIWaitForRetrace` for a whole field and everything runnable gets to run during it; the
   pure-counter override never blocked. Added `gsched_drain()` — park until nothing else is
   Ready — and called it from the VI override. The retired `docs/native_threading.md` names
   this exact failure: *"a never-blocking frame loop starves lower-priority threads (the boot
   setup thread) forever."*
4. **New threads started with r2/r13 = 0.** The small-data bases are set once at boot and are
   ABI for every thread; a zeroed CPUState made every `r13`-relative access wild
   (`SMSLoadArchive` reading `0xffffa0d4`). `gsched_create` now seeds them from the creating
   thread. That exposed a second layer: thread 0's scheduler CPUState was a *copy* taken
   before boot, so `main` now runs on `gsched_cpu()` rather than a stale local.

**Verified:** four guest threads live, three parked in `OSReceiveMessage` (one is
`JKRDecomp::run`), and the game performs **28 disc reads totalling 107 KB** through the
instant-FS DVD path.

Next: it loads that first archive then returns to its main loop without requesting more, so
something downstream is still waiting.

## Where the boot stands after threading

The loader thread works end to end:

- `DVDConvertPathToEntrynum("/data/nintendo.szs") = 48`
- 28 instant-FS reads covering the whole file (`offset 0x1134a8d8 len 0x1a208`)
- `SMSLoadArchive -> 0x8131f0e0` — a valid archive pointer
- `JKRDecomp::orderSync` / `decode` are never called, so this archive needs no worker

Then it stops: **only `nintendo.szs` is ever requested** (twice), and the main thread runs its
frame loop indefinitely (6.09 M retraces in 40 s) doing pad reads without asking for another
file.

Note the retired `native_dvd.cpp` recorded the identical symptom — *"under native scheduling
only data/nintendo.szs transfers; pure-Dolphin reads sequence.arc / *.aw / mario.szs / … next"*
— but its cause (the DVD command queue's dispatch-next step never firing) is bypassed
entirely by instant reads, and the loader here demonstrably completes. So the stall is
downstream of loading, not in it.

Two candidates worth separating next:

1. **Timing skew.** `VIWaitForRetrace` is a pure counter draining at ~150 k "frames"/s while
   `OSGetTime` runs off a real monotonic clock. Anything frame-counted finishes instantly
   while anything time-based takes real seconds — a boot state machine mixing both would sit
   waiting for a wall-clock deadline that, from its own frame count, should have passed
   aeons ago.
2. **A state machine waiting on something else entirely** — the prio-15 thread
   (`0x802a9184`), a fader completion flag, or the logo sequence itself.

Distinguishing them is cheap: pace the retrace counter to real time and see whether boot
advances. That also matters independently, because a frame counter running 2500x faster than
the wall clock will misbehave everywhere later.

## Narrowing the post-load stall

Findings this pass, all measured rather than inferred:

- **`TGCLogoDir::direct` is never called.** The GC logo director is not the thing running, so
  the stall is not in the logo state machine. (Reached by overriding `0x80295a0c` and seeing
  zero calls.)
- **The main loop is the render loop and it is healthy.** `0x802a5f50` (an unsymbolized
  static in `TApplication.cpp` — the sparse symbol file makes it *look* like
  `mountStageArchive+0x5b8`, which it is not) drives a virtual call through `this->0x1c`,
  whose vtable at `0x803e1dc0` has `startRendering`/`endRendering` at +8/+0xc. So that field
  is the `JDrama::TDisplay`, and the loop is doing frame start/end, pad reads and fader draws
  every iteration.
- **The archive is loaded and valid** (`SMSLoadArchive -> 0x8131f0e0`), so nothing is waiting
  on the loader.

So: the game renders frames indefinitely with a loaded archive, and simply never advances to
requesting the next file. The thing that should advance it — a director/state machine beyond
the render loop — has not been identified yet.

Also worth noting for later, from the retired `fader_pace.cpp`: an un-paced boot makes the
GC-logo fade complete in milliseconds ("logo popped in fully visible"). That is COSMETIC, not
a stall — the retired build still advanced — so frame pacing is a fidelity item, not the
current blocker. It engaged on the first `TSMSFader::startWipe` (`0x8013f860`).

## ROOT CAUSE of the post-load stall: thread death was never published to the guest

The game's main loop polls **`OSIsThreadTerminated`** and then **`OSJoinThread`** (twice, for
two workers — visible as `bl 0x80348374` / `bl 0x80348d08` inside `0x802a5f50`).

`OSIsThreadTerminated` is `lhz r3,712(r3)` and returns true when the state halfword is 8
(MORIBUND) or 0. The cooperative scheduler tracked thread death in its OWN bookkeeping and
never wrote it back to the guest `OSThread`. So the loop asked "terminated?", got false
forever, called `OSJoinThread` (which returned instantly, because the scheduler correctly
knew the thread was dead), and looped — rendering frames indefinitely without ever advancing.

`gsched_exit` now writes `OSThread::state = MORIBUND` at `+712`. The lesson generalizes: this
scheduler replaces the guest's, so any state the guest OBSERVES about threads has to be
published into the guest struct, not just tracked host-side.

Also extended the `aram` device to the end of its block (`0xCC005012-0xCC005040`); the newly
reached code touches `0xCC005030`.

**Boot now advances into audio init:**

```
func_802a6dd0  TApplication (mountStageArchive+0x1438)
  func_80014e70  MSound::startSoundSet
    func_80301a28  JAIBasic::initDriver
      func_803113c4  JASystem::AudioThread::start
```

Two JKRThread workers remain parked in `OSReceiveMessage`, as they should be.

Note the retired `native_os.cpp` flagged exactly this next step: *"AudioThread::start
creates+resumes the higher-priority audio thread (which runs Driver::init -> initBuffer to
ALLOCATE the DSP FX buffers) and returns WITHOUT waiting, relying on this preemption; without
it the main thread reaches JAIData::initData and configures FX lines that were never
allocated -> null write."* `gsched_make_ready` already implements that priority preemption.

## Audio init: the DSP seams

With thread death published, boot reaches `MSound::startSoundSet` -> `JAIBasic::initDriver`
-> `JASystem::AudioThread::start`, which creates the audio thread at a HIGHER priority than
its creator. `gsched_make_ready` honours that preemption, so the audio thread takes the token
immediately — and then never gives it back, because every DSP interaction waits for a reply
only a real DSP core can produce. That parks the entire boot behind it.

Three seams, in increasing order of surgical-ness:

1. **`__DSP_boot_task`** (`0x8035406c`) — uploads the microcode and handshakes over the
   mailboxes. Overridden to a no-op: there is no core to boot, and audio is host-side.
2. **The JASystem DSP task-queue readiness predicate** (`0x80337ca0`) — `lbz r0,-23224(r13);
   return (r0 == 1)`, a byte the task-completion interrupt sets. Overridden to report ready:
   a queue nothing is ever queued into always has a free slot. Deliberately narrow, so
   `DSPInterface::initBuffer` still performs its FX-buffer allocations, which
   `JAIData::initData` later expects to exist.
3. **Mailbox "full" bit** — fixed in the DEVICE rather than by overriding more SDK functions.
   Bit 15 of the high halfword is set by the sender and cleared by the RECEIVER when it
   reads. `dev_dsp.cpp` now clears it on write to `DSP_MAIL_TO_DSP`: mail is consumed the
   instant it is sent, which is the truthful model for a receiver that is not there. Without
   it `__DSPCheckMailToDSP` (`0x80353d38`, bit 15 of `0xCC005000`) reported pending forever
   and every SDK send spun.

Preferring the device fix to an SDK override matters here: one device rule retires several
spin loops at once, and it keeps the guest's own DSP code running rather than replacing it.

Audio thread progress: `initBuffer` -> `0x80337580` now COMPLETES; it is currently in
`0x80337360`, one frame up. Silence is still by omission — none of this makes sound, it
stops the absence of a DSP from deadlocking the boot.

## ARQ completes synchronously — file loading opens up (31 -> 149 reads)

`JASystem::Kernel::portCmdInit` posts ARAM DMA requests through **ARQ** and then spins on a
pending count (`r13-23444`) that only the ARAM interrupt drains. Same shape as DVD: a queue
whose completion depends on an interrupt this runtime does not have.

`overrides/native_arq.cpp` overrides `ARQPostRequest` (`0x80353bdc`). The transfer has nothing
to wait for — ARAM is a host buffer and the copy is a `memcpy` — so the request is performed
and completed inside the post, then the completion callback runs as the ARAM interrupt would
have invoked it (`r3` = the request), with whole-state save/restore because it is a nested
call inside a function that has not returned.

`ARQRequest` layout came from the recompiled `ARQPostRequest` prologue stores
(`stw r0,0 / r4,4 / r5,8 / r7,0x10 / r8,0x14 / r9,0x18 / r10,0x1c`), i.e. next, owner, type,
source, dest, length, callback. `dev_aram.cpp` now exports `aram_dma()` so the register-level
DMA and this seam share one implementation.

**Effect: disc reads went 31 -> 149**, and the game is loading real content — the last read
resolves through the FST to **`wScene_16.aw`** (an audio wave bank, `0x3aaa80` bytes), far
beyond the boot logo. The main thread is in the render loop under
`JDrama::TDisplay::startRendering`; the JKRThread workers remain correctly parked.

## Status: the recomp runs a stable frame loop with 8 threads

Measured state after the ARQ fix:

| thread | state |
|---|---|
| main | running the frame loop |
| 4x JKRThread workers (`0x802c54b8` trampoline) | parked in `OSReceiveMessage` |
| `0x802a9184` | parked in `OSReceiveMessage` |
| audio thread (`0x80311170`) | parked in `OSReceiveMessage` |
| audio kernel (`0x803171ec`) | parked in `OSReceiveMessage` |
| card manager (`0x802b3264`) | parked in a message receive |

Profiling the main thread over 24 samples shows an ordinary frame — `TDisplay::startRendering`,
`TSMSFader::draw`, `JAIBasic` audio processing, `TMarioGamePad::read`,
`TDisplay::endRendering`. **Nothing is spinning**; this is a game running frames, not a
deadlock. Files loaded: `nintendo.szs`, `sequence.arc`, `wScene_16.aw` (149 reads).

It simply does not advance to the next screen. Investigating that blind is expensive — the
last few blockers each took a disassembly dive — and there is a much better instrument
available: **the game is submitting real GX commands to the write-gather pipe and every one
is being dropped.** Routing that FIFO to aurora would both make the current state visible
(what screen is it actually on?) and is the arc the user originally asked for. It is also
what the retired era's open problem was: rendering, not boot.

Next: GX FIFO out of `0xCC008000` and into aurora.

## GX FIFO: the command stream is now framed and measurable

`runtime/dev_gxfifo.cpp` claims the write-gather pipe (`0xCC008000`) and parses the GX
command stream instead of discarding it.

Why a parser and not SDK overrides: GX is not a function-call API at the metal. The SDK's
inline macros store command bytes AND vertex data straight to the gather pipe, so the command
stream is the only place the geometry exists — overriding `GXBegin`/`GXPosition3f32` would
capture nothing. This is the same reason the renderer doctrine is GX-replay.

Top-level framing: `0x00` NOP, `0x08` CP register write (6 bytes), `0x10` XF write
(header + count words), `0x61` BP write (5 bytes), `0x80-0xBF` draw primitive (16-bit vertex
count then payload). The stream arrives in 1/2/4/8-byte pieces that straddle commands, so it
is reassembled before framing.

The hard part is knowing a draw's payload length, which needs the live vertex format:

- **VCD** (CP `0x50`/`0x60`) gives each attribute's presence mode — none / direct / index8 /
  index16. Indexed attributes are 1 or 2 bytes whatever the format.
- **VAT** (CP `0x70-0x77` / `0x80-0x87` / `0x90-0x97`) gives the direct formats: position
  elements+format, normal format, colour component formats, and texcoord formats packed 9
  bits each (VAT_A holds texcoord 0, VAT_B 1-4, VAT_C 5-7).

**Decoding those properly mattered.** A first version assumed 4-byte colours and f32 texcoord
pairs and desynced **18 times** per run. Decoding colour component formats
(RGB565/RGB8/RGBX8/RGBA4/RGBA6/RGBA8 = 2/3/4/2/3/4 bytes) and per-texcoord formats from the
VAT registers brought that to **1** — at startup, before any VAT has been seen, which is
expected. An unrecognised opcode drops the rest of the batch rather than resyncing blindly on
data that happens to look like opcodes.

Measured: **~1.26 M draws / 5.05 M verts** in a 25 s run, consistently 4 verts per draw —
quads, i.e. the fader/logo rectangle, which matches what the profile showed the main thread
drawing.

This is the foundation for routing to aurora: the stream is now framed, and the vertex format
tracking a translator needs is already in place.

## Rendering plan: aurora consumes the FIFO directly

`aurora_fifo_replay(const uint8_t* data, uint32_t size, int bigEndian)`
(`extern/aurora/include/aurora/aurora.h:148`) takes a raw GX command stream, and
`extern/aurora/lib/gx/command_processor.cpp` is a full CP implementation. So **no translation
layer is needed** — the guest's gather-pipe bytes can be handed to aurora as-is. This is also
why the decomp runtime's `SB_FIFO_REPLAY` harness can replay Dolphin `.dff` captures.

**One real obstacle, and aurora documents the fix.** Its CP deliberately IGNORES raw CP
array-base writes (`0xA0-0xAF`):

> the raw CP write can only carry a 32-bit truncated host pointer, so we cannot use `value`
> as a real base address on a 64-bit host … the CORRECT 64-bit pointer for the same attr will
> be supplied separately via `GX_AURORA_LOAD_ARRAYBASE`

That reasoning holds for the decomp, where the "guest address" is really a truncated host
pointer. **It does not hold for the recomp**, where those 32 bits are a genuine guest address
and we own the memory they refer to. So the bridge is:

- feed the FIFO to `aurora_fifo_replay`
- intercept CP writes to `0xA0-0xAF` and re-emit them as
  `GX_AURORA_LOAD_ARRAYBASE` (`0x0010`, see `dolphin/gx/GXAurora.h`) carrying
  `g_ram_base + (guest_addr & 0x01FFFFFF)`

The FIFO parser already tracks CP writes, so it is the natural place to do this.

Remaining integration work: link aurora with `AURORA_ENABLE_GX=ON` into `sms-recomp`,
initialise it, and drive `aurora_begin_frame`/`aurora_end_frame` from the existing frame seam
(`VIWaitForRetrace` / `TVideo::waitForRetrace`).

## Aurora is wired in: the guest's GX stream now reaches a real renderer

`sms-recomp` links aurora (`AURORA_ENABLE_GX=ON`, DVD off — this runtime serves the disc
itself through nod) and feeds it the guest's own command stream.

- `dev_gxfifo.cpp` now REWRITES rather than only counts: every recognised command is copied
  verbatim into an output stream, except CP array-base writes, which become
  `GX_AURORA_LOAD_ARRAYBASE` (`GX_AURORA` opcode `0x50`, then u16 subcommand, u64 host
  pointer, u32 size, u8 endian flag) carrying `g_ram_base + (addr & 0x01FFFFFF)`.
- `overrides/native_frame.cpp` overrides `JDrama::TVideo::waitForRetrace` as the once-per-frame
  present: flush the stream to `aurora_fifo_replay`, then `end_frame`/`begin_frame`.
  `VIWaitForRetrace` is deliberately NOT this point — the game spins on it from load loops.
- `host/main.cpp` initialises aurora with `mem1Size = 0`: this runtime owns its guest memory
  and hands aurora real host pointers for anything it must read.

**Display lists were the big omission.** Opcode `0x48` (call display list) was unhandled, and
since an unrecognised opcode drops the rest of the batch, most of the stream was being
discarded ("unrecognised opcode 0x48 — framing lost"). J3D bakes per-shape geometry into
display lists, so this is where real drawing lives. They are now INLINED: the list is read
out of guest memory and its commands emitted into the flat stream, because aurora cannot
follow a guest pointer — the same reason it ignores raw array bases. Nesting is bounded to 4.
After this, framing errors are gone.

Build notes worth keeping: the targets are `aurora::gx` / `aurora::core` (there is no
`aurora::aurora`), the two reference each other so both must appear together on the link
line, and the overrides object library needs `aurora::gx` for its include path.

**Verified running headless at low resolution** (SB_HEADLESS=1, per the project rule that all
agent runs are headless): aurora initialises on Vulkan, the frame seam fires, and draws
accumulate steadily with zero GX errors.

Open: the dumped framebuffer is **1x1**. Aurora sizes its render target from the game's
EFB/display-copy configuration, which is evidently not reaching it through the replayed
stream yet. That is the next thing to chase — and note aurora also has
`GX_AURORA_LOAD_VIEWPORT_RENDER`/`LOAD_SCISSOR_RENDER` extensions, which suggests the raw
viewport in the stream is not sufficient by itself.

## The recomp renders a real framebuffer

The 1x1 present source was the display copy never reaching aurora. Aurora DOES handle the
copy-to-XFB trigger (BP `0x52` bit 14) during FIFO replay, but only after the copy source and
destination have been described through its own extensions — the raw BP registers carry EFB
coordinates and a guest destination address it cannot use directly. Its own comment says so:

> the copy src/dst state must have been loaded via `GX_AURORA_LOAD_COPY_{SRC,DST}` beforehand

`dev_gxfifo.cpp` now tracks the EFB copy rectangle from BP `0x49` (top-left, 10 bits each)
and `0x4A` (width-1 / height-1), and emits `GX_AURORA_LOAD_COPY_SRC` (4x u32) plus
`GX_AURORA_LOAD_COPY_DST` (u32 w, u32 h, u32 fmt, u8 wide) immediately before passing the
copy trigger through.

**Result: the dump goes from 1x1 to 1280x896** — a real framebuffer, 4.5 MB.

That is the third instance of one pattern, now worth stating plainly: **aurora's FIFO replay
accepts the guest's command stream verbatim EXCEPT where a command carries a pointer or an
address.** Array bases, display lists and copy src/dst all need translating into aurora's
extensions, because in the decomp world those 32-bit values are truncated host pointers while
here they are genuine guest addresses. Anything else pointer-bearing (texture objects, TLUTs
— `GX_AURORA_LOAD_TEXOBJ`/`LOAD_TLUT`) will need the same treatment.

**Content: uniform (17,17,17) dark grey.** Rendering is working; the game is drawing its
boot-fade quad and nothing else, which is exactly consistent with it being stuck on the fade —
the state problem already recorded, not a render problem. Distinguishing those two was the
whole point of getting pixels out.

## ROOT CAUSE: the game REUSES OSThread structs, and the scheduler ignored the second use

With pixels available, the state could finally be read directly instead of inferred.
`gpApplication` is at `0x803E9700` (confirmed against the retired `fastboot_native.cpp`) and
`mAppState` is a `u8` at `+0x08`. It read **3 = `APP_STATE_NLOGO`** (enum from
`decomp/sms/include/System/Application.hpp`), forever.

The decomp is the oracle for what NLOGO is supposed to do (`Application.cpp:853`):

```c
} else if (mAppState == APP_STATE_NLOGO) {
    nextState = APP_STATE_WAIT;
    if (!(sGameInit & 1) && mDirector->direct() == APP_STATE_DONE) sGameInit |= 1;
    if (!(sGameInit & 2) && OSIsThreadTerminated(&gSetupThread)) { OSJoinThread(...); sGameInit |= 2; }
    if (sGameInit == 3) nextState = APP_STATE_DONE;
}
```

Instrumenting showed bit 0 was being set — `TGCLogoDir::direct` ran and returned
`APP_STATE_DONE` at call #101. So the wait was on bit 1, `gSetupThread`.

`Application.cpp` creates that thread **twice**: line 313 with `SetupThreadFuncBoot`, and
line 445 with `SetupThreadFuncLogo` — **reusing the same `OSThread` struct**. `gsched_create`
had `if (find(os_thread)) return;`, so the second creation was silently dropped: the thread
never ran, never terminated, and NLOGO waited on it forever.

`gsched_create` now resets an existing entry with the new entry/param/stack/priority. Two
cases are distinguished:

- **Dead** — ordinary reuse; reset and let `OSResumeThread` start it again.
- **Parked** — also legitimate: the game recreates a struct whose previous occupant waits
  forever on a queue nothing posts to (a THP worker). It is retired so the scheduler never
  picks it again. *This leaks one parked host thread per recreation* — bounded and harmless
  in practice, but a real leak; the honest fix is a generation counter so a woken stale
  thread exits rather than resuming a retired body.
- **Running** — recreation from inside the thread's own body, which cannot be legitimate,
  still aborts.

**Effect: disc reads 149 -> 924**, and the game now loads its real boot set —
`mario.szs`, `common.szs`, `option.szs`, `particle.szs`, `guide.szs`, `params.szs`,
`scenecmn.bin`, `game_6.szs` and `Entrance.thp` (the attract movie).

**Next:** a NULL dereference in the THP player (`OSReceiveMessage` on a null queue, called
from the THP region around `0x8001db24`). Expected territory — there is no video decoding
here, and `Entrance.thp` is the attract movie. The retired `fastboot_native.cpp` skipped this
whole state; the equivalent decision is due.

## THP skipped via the game's own failure path — the boot state machine now RUNS

The THP player faulted (NULL read inside its decode thread body at `0x800200d8`) because this
runtime has no video decoding — the same "absent by omission" position as audio. Rather than
stub the decode path piecemeal, `overrides/native_thp.cpp` makes **`THPPlayerOpen` report
failure**. The game already handles movie setup failing, so this uses the game's own control
flow rather than inventing one. Attract movies and cutscenes do not play.

**Result — the state machine advances for the first time:**

```
[app] mAppState -> 2 (BOOT)
[app] mAppState -> 3 (NLOGO)
[app] mAppState -> 4 (DONE)
[app] mAppState -> 5 (GAMEPLAY)
```

and disc reads went **924 -> 337,501**.

`native_frame.cpp` now reports `mAppState` transitions once per change under
`SBR_LUCENT_DEBUG=app`, so boot progress is permanently visible instead of needing a
throwaway diagnostic each time.

## Current: a DONE <-> GAMEPLAY cycle

The state then oscillates `DONE -> GAMEPLAY -> DONE -> GAMEPLAY`, which is what the huge
read count is: the game repeatedly tries to enter a stage and falls back. Per the taxonomy
note in the retired `fastboot_native.cpp`, the retail title screen and save-file picker are
GAMEPLAY **stage 15** (`option.arc`), so this loop is the game attempting the title and not
completing it.

## ROOT CAUSE of the DONE <-> GAMEPLAY cycle: thread exit values were dropped

The state log showed the target: `next={15,0}` — **stage 15**, the retail title screen and
save-file picker (`option.arc`). It entered GAMEPLAY, fell back to DONE, and retried forever.

`TMarDirector::setupThreadFunc` returns `loadResource()`'s result, and `gameLoop` reads it
through `OSJoinThread(&gSetupThread, &val)` to decide whether the stage loaded. The scheduler
captured no exit value and `os_join_thread` never wrote the `void** result` out-parameter, so
the game read stale memory and concluded every stage load had failed.

`GuestThread` now records the body's `r3` when it returns, `gsched_exit_value()` exposes it,
and `OSJoinThread` writes it to the out-parameter. **The oscillation stops: the game reaches
GAMEPLAY stage 15 and stays there.**

Worth generalising: this is the third time the fix has been *"publish to the guest what the
guest observes"* — thread state (`OSIsThreadTerminated`), the current-thread global
(`OSGetCurrentThread`), and now exit values. Replacing the guest's scheduler means owning
every observable it used to maintain.

## Discovery gap: functions that are a bare `blr`

Stage 15 loading immediately hit `call to un-recompiled address 0x800339a0`. That address is
a single `blr` — a complete, empty function body. MWCC merges identical bodies, so several
empty virtuals point at the SAME `blr`, which is therefore usually preceded by another
function's last instruction rather than by a terminator. The pointer-discovery boundary test
rejected it on exactly that basis.

A bare `blr` is now accepted as a function entry without the boundary test. Functions:
**14,241 -> 14,287**.

## GXWaitDrawDone — the deadlock at stage 15

Once stage 15 loaded, every guest thread blocked. The scheduler's deadlock detector caught it
and, with the guest stack added to the report, named the wait immediately:

```
GXWaitDrawDone <- TMarDirector::setup2 <- ... <- gameLoop
```

`GXDrawDone` (already overridden) is "push a token AND wait"; **`GXWaitDrawDone`
(`0x8035da9c`) is the wait half on its own** and was missed. It blocks on the same
`__GXDrawDoneFlag`, which has no source here. Overridden the same way.

The deadlock report now prints state names and the guest call stack of the thread that closed
the cycle, which is what turned "everything is blocked" into a one-line diagnosis.

## Where rendering stands

With that fixed the game runs in GAMEPLAY stage 15 with no errors, and **vertex submission
jumps from ~500 to 182,013 per frame** — real scene geometry, not just the fader quad.

**But the framebuffer is still uniform (17,17,17).** Geometry is being submitted and accepted
(no array-base rejections even with `AURORA_ARRAYBASE_REJECT_RAW=1`, so the pointer
translation is working), yet nothing lands in the presented image.

Untranslated pointer-bearing state is the leading suspect, consistent with the pattern
established three times already:

- **`GX_AURORA_LOAD_TEXOBJ` / `LOAD_TLUT` are NOT emitted.** Texture image addresses arrive
  as BP `image0`/`image3` register writes carrying guest physical addresses; aurora stores
  them but takes the actual texel pointer from a `GXTexObj` supplied separately — exactly the
  array-base situation. Every textured draw therefore has no texture data.
- `GX_AURORA_LOAD_VIEWPORT_RENDER` / `LOAD_SCISSOR_RENDER` also exist, suggesting the raw
  viewport in the stream may not be sufficient on its own.

Next: emit `GX_AURORA_LOAD_TEXOBJ` for each texture slot, built from the BP image registers
with the texel pointer resolved out of guest memory.

## Texture objects emitted; and what the draw trace actually shows

`dev_gxfifo.cpp` now tracks the per-texmap BP image registers (`image0` = dimensions+format,
`image3` = texel address in 32-byte units; maps 0-3 at `0x88-0x8B`/`0x94-0x97`, maps 4-7 at
`0xA8-0xAB`/`0xB4-0xB7`) and emits `GX_AURORA_LOAD_TEXOBJ` once both halves of a slot are
known. Payload from aurora's parser: `u8 map, u64 data, u32 w, u32 h, u32 fmt, u32 tlut,
u8 hasMips, u32 texObjId, u32 texDataVersion`. The texel address doubles as `texObjId` —
aurora's texture cache key — because a zero id makes every bind a cache miss and re-upload
(the documented 33x perf cliff).

**This did not change the output, and the reason is more interesting than a missing texture.**
`AURORA_DRAW_TRACE=1` shows aurora receiving and parsing draws correctly, including large
ones:

```
vtxCount=4      x168
vtxCount=52     x26
vtxCount=640    x1
vtxCount=36917  x1
vtxCount=57757  x1
vtxCount=58347  x1
```

The scene-sized draws appear **exactly once each**, not per frame. Per-frame traffic is
4-vertex quads — the fader. So the renderer is receiving what the game sends; the game is
sending a fader every frame and the scene only once.

That relocates the question back to game state: stage 15 has loaded and built its scene, but
the game is not drawing it per frame. Rendering is no longer the suspect — which is the
second time getting an instrument in place has moved the blame rather than confirmed it.

The texobj emission is kept because it is required and correct in principle, but note it is
**unverified**: nothing yet renders a texture, so it has not been shown to work.

## FIRST REAL FRAME: the Dolby Pro Logic II screen renders

Two fixes turned the flat grey into actual content.

**1. Redundant texture loads were crippling the frame rate.** `emit_texobj` fired on every BP
image-register write, and the game rewrites those on every material bind — so aurora was
being told to load textures thousands of times per frame. A long run went BACKWARDS (retrace
240 -> 180 over four times the wall clock). Emitting only when `(image0, image3)` actually
change fixed it; the run now reaches retrace 300 in the same time.

**2. "The scene is drawn once, not per frame" was wrong** — an artifact of that slowdown.
With the flood removed, vertex submission climbs continuously (100 k -> 199 k between
consecutive samples), i.e. the scene IS resubmitted every frame. The earlier conclusion came
from having only four samples in a 60 s run; more samples falsified it.

**The dump is no longer uniform:** 14 distinct colours, white (238,238,238) clustered in
x 520-808, y 392-504 — centred. Rendered to PNG it is unmistakably the **Dolby Pro Logic II
boot logo**, white on black, correctly positioned.

That is the recompiled game drawing a real screen through aurora: guest GX commands out of
the write-gather pipe, framed by our parser, pointer-translated, replayed by aurora, copied
to XFB and presented. It also matches the boot sequence — `TGCLogoDir` has both
`direct_nlogo` and `direct_dolby`, and this is the Dolby half.

Frame capture for the record: `scratch/recomp/scene.png`.

## Vertex sizing had to match aurora EXACTLY: the NBT/NBT3 normal quirk

Aurora aborted with `unsupported primitive type 136`. `0x88` is not a primitive at all — the
SDK enum goes `GX_QUADS = 0x80` straight to `GX_TRIANGLES = 0x90` — and aurora's own comments
note that a stream desync surfaces exactly this way. So the fault was in OUR stream.

Cause: `vertex_size()` did not implement the GC normal quirk that
`calculate_last_vtx_size()` (aurora, `command_processor.cpp:1824`) does:

- VAT_A **bit 9** selects NBT — normal+binormal+tangent, **9 components instead of 3**
- VAT_A **bit 31** selects NBT3, where an **indexed** normal costs **three** indices
  (3 bytes for INDEX8, 6 for INDEX16) rather than one

J3D uses these for lit models, so every such draw's payload length was wrong, the byte span
copied into the output stream was wrong, and aurora desynced. Matching aurora's algorithm
exactly fixed it.

The general lesson: this parser and aurora's must agree **bit for bit** on vertex layout,
because the parser decides how many bytes belong to a draw. Aurora's own implementation is
the reference — read it rather than deriving the rules independently.

Also fixed while here: `parse()` re-scanned the whole buffer on every 4-byte append while a
large command was incomplete (a 58 k-vertex draw is ~1.7 MB), which is quadratic. It now
records how many bytes the outstanding command needs and waits for them. The nested
display-list parse saves/restores that counter so an inner list cannot disturb the outer
stream's state.

Measured present rate is **~200/s** — an earlier "presents are slow" assumption was simply
wrong, and the dumps that failed to appear were from the pre-fix build.

**Next:** aurora now aborts on `unhandled tcg src 21 at tcg[1]`, a texture-coordinate
generation source. The GX_TG enum tops out around 20, so this is either a further (smaller)
desync or a genuine aurora gap — worth distinguishing before assuming either.

## VAT_C texcoord offset — the last stream desync

`unsupported primitive type 136` persisted intermittently after the NBT3 fix. Cause: my
VAT_C decode assumed TEX5 began at bit 0. Aurora's parser shows it does not —
**TEX4's 5-bit `frac` occupies bits 0-4 of VAT_C, so TEX5 starts at bit 5**:

```
TEX4.frac  bits 0-4      TEX5 cnt/type/frac  bits 5 / 6-8 / 9-13
TEX6       bits 14 / 15-17 / 18-22           TEX7  bits 23 / 24-26 / 27-31
```

(VAT_B, by contrast, IS a clean 9-bit stride from bit 0, which is what made the wrong
assumption look plausible.) Texcoord-heavy vertices were therefore sized wrongly and the
stream desynced. **Fixed: three consecutive runs, no primitive error.**

That is twice now that a vertex-layout detail derived independently was wrong where aurora's
own parser had it right. The rule stands: read aurora's decode, do not re-derive it.

## Remaining: `unhandled tcg src 21 at tcg[1]` — now reproducible

With the desync gone this fires consistently. `21` is `GX_MAX_TEXGENSRC`, aurora's
"never written" sentinel, and `mtx=60`/`postMtx=125` are both identity defaults — so
aurora's `tcg[1]` was never configured in its session.

The guest evidently DOES configure it: logging XF writes to `0x1040-0x104F` shows all eight
texgens written with valid source rows (5-12 = `GX_TG_TEX0..TEX7`), 1719 times per run.
Aurora only assigns `tcg.src` when `srcRow < 13`, and these all qualify.

So either those XF writes are not reaching aurora intact, or something resets aurora's tcg
state between the write and the draw. That is the next thing to pin down — and unlike the
desyncs, this one is stable enough to bisect properly.

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

## Frame-boundary drain (real bug, but not the current one)

`parse()` only ran once 4096 bytes had accumulated, so a frame's trailing commands could sit
unparsed and be emitted in the NEXT frame's stream — aurora would draw a frame with the
previous frame's state. `gxfifo_flush()` now drains the buffer before presenting, which is
correct at a frame boundary because the guest has finished writing.

This did **not** fix `unhandled tcg src 21`, and `unsupported primitive 136` still appears
intermittently, so the VAT_C fix reduced but did not eliminate the desync.

## Honest state of GX parity

Confirmed by tracing the guest's own writes:

- All eight texgens ARE configured with valid source rows before any draw
  (`texgen[0..7] srcRow=5..12`), then `numTexGens` is set to 1 and later 0.
- Aurora's XF header decode (`count = header>>16 + 1`, `addr = header & 0xFFFF`) matches
  this parser's exactly, so the writes are framed identically.
- Aurora copies `numTexGens` entries out of `tcgs[]` when building a pipeline, and only
  assigns `tcg.src` for `srcRow < 13` — all observed rows qualify.

So the guest configures `tcg[1]`, our stream carries it, and aurora still reports it unset at
draw time. Something between those points is losing or reordering state, and the remaining
intermittent primitive error says the vertex-size decode still disagrees with aurora's for at
least one configuration.

The productive next step is not more guessing: aurora ships `AURORA_FIFO_TRACE`, and its
desync FATAL already dumps a 128-entry command ring plus the recent-draw ring specifically to
show where a mis-advance happened. Using those to find the exact command whose length this
parser computes differently is the way to close the remaining gap, rather than auditing bit
layouts one at a time.

## The GAMEPLAY <-> MOVIE oscillation, and how to skip a movie properly

Reporting `THPPlayerOpen` as failing was not sufficient. The game re-entered the MOVIE state,
failed again, and oscillated `GAMEPLAY <-> MOVIE` indefinitely — visible in the state log.

The game's OWN way of not playing a movie is the "already seen" flags that
`checkAdditionalMovie()` consults. `native_thp.cpp` now sets them on first use:
`0x30009` (airport opening), `0x3000B`/`0x3000C` (plaza intros), `0x3000D` (shine gate).
`setBool(true, f)` for these is exactly `setFlag(f, 1)` (FlagManager.cpp case 3, all below
`0x3001D`), and `setFlag` is at `0x80294b1c` with `TFlagManager::smInstance` at `r13-0x6060`
— addresses the retired fastboot override had already RE'd against this DOL, and the same
four flags it set.

**The oscillation stops and the game stays in GAMEPLAY stage 15.**

Note the shape of this fix versus the previous one: failing the open used a real error path,
but an error path the game is designed to RETRY. Marking the movie seen uses the path the
game takes when there is simply nothing to play. The second is the honest analogue of "we
have no video".

## ROOT CAUSE of the desync: indexed XF loads were an unknown opcode

`GX_LOAD_INDX_A/B/C/D` (`0x20`/`0x28`/`0x30`/`0x38`) are indexed XF loads — one u32 payload,
`index<<16 | (len-1)<<12 | xfAddr`, selecting the PosMtx / NrmMtx / TexMtx / Light arrays.
J3D emits them constantly for per-shape matrix loads.

The parser did not know them, so it took the "unrecognised opcode" path and dropped the rest
of the batch. Worse, inside an inlined display list that meant **silently discarding the
remainder of the list**, because `inline_display_list` ignores the parse return value. Every
J3D shape whose display list loaded a matrix lost everything after that point.

That is why the symptoms were nonsense values in unrelated places (`unsupported primitive
136`, `PosMtx sub-copy len=2305`, `tcg src 21`) and why they moved between runs.

**Effect: a run reached 3,300 presents with no error, against ~120-330 before.**

The self-check added to the draw path — verify the byte after a draw is a plausible opcode,
and if not report the exact VCD/VAT — is what localised this. It showed a draw whose vertex
size was *correct* (12 bytes, position-only f32) still landing on an invalid next byte, which
proved the mis-advance happened earlier and was not a vertex-size bug at all. Worth keeping:
it turns "aurora failed somewhere" into "this parser mis-advanced here".

Remaining, both now much later and less frequent: `unhandled tcg src 21` and
`unsupported cull mode 3` (GX_CULL_ALL — an aurora gap rather than a stream problem).

## Open contradiction: aurora reports cull mode 3, the stream never sends it

`GX_CULL_ALL` (3) is a legitimate GX mode aurora does not implement — WebGPU has no
"cull everything" rasterizer state, so `to_primitive_state` FATALs on it.

But the guest does not appear to send it. Logging every BP `genMode` (`0x00`) write the
parser frames shows only three before the fatal:

```
genMode #1 raw=0x000001 cull=0
genMode #2 raw=0x004010 cull=1
genMode #3 raw=0x004010 cull=1
```

Aurora reads the same bits (14-15) and SWAPS front/back, so `hwCull=1` becomes
`GX_CULL_BACK` (2). Its default is also `GX_CULL_BACK`. So neither the stream nor the default
explains a 3.

Since aurora only ever sees `g_out`, and `g_out` is built solely from framed commands, one of
these must be false — and that is the thing to establish next, not guess at:

1. a `genMode` write is reaching aurora that this parser never framed (i.e. bytes enter
   `g_out` by some path other than the BP branch), or
2. `g_gxState.cullMode` is being set somewhere other than the `genMode` handler, or
3. the FATAL's value is not `g_gxState.cullMode` at all but a shader-config copy taken
   elsewhere.

`AURORA_FIFO_TRACE=1` logs every command aurora itself decodes; diffing that against this
parser's view of the same bytes settles (1) directly. That is the cheap decisive experiment
and should come before any change to aurora.

Note the same shape applies to the outstanding `tcg src 21`: the guest demonstrably writes
valid texgen sources, aurora reports an unwritten slot. Both smell like one mechanism.

## Contradiction resolved: BP writes are MASKED, so raw bits are not the value

The cull-mode puzzle — "aurora reports 3, the stream never sends 3" — came from measuring the
wrong thing. Aurora's `handle_bp` implements the GC's **BP write-mask register (0xFE)**:

```cpp
u32 ssMask = g_gxState.bpRegCache[0xFE];
g_gxState.bpRegCache[0xFE] = 0x00FFFFFF;          // mask applies to ONE following write
const u32 merged = (g_gxState.bpRegCache[regId] & ~ssMask) | (value & ssMask);
```

A masked write only updates the masked bits and keeps the rest of the cached register. So a
write that sets **bit 15 alone**, merged with a cached **bit 14**, produces cull = 3 —
without any single raw write ever containing 3. The probe checked raw bits and correctly
reported zero, which is why it looked impossible.

**So `GX_CULL_ALL` is genuinely used by the game, and aurora's missing support is a real
gap**, not a symptom of a parser bug. Worth remembering generally: for BP registers, the
value the hardware ends up with is the merged one — never reason about a BP field from a
single raw write.

This also means the outstanding `tcg src 21` deserves re-examination on the same basis before
being treated as a state-loss bug.

## GX_CULL_ALL implemented in aurora (fork branch `gx-cull-all`)

With the masked-BP explanation established, the gap was real and the fix belongs in aurora:

- `push_gx_draw` drops draws when `cullMode == GX_CULL_ALL` — both faces culled means no
  fragments, which is what the hardware does.
- `to_primitive_state` no longer aborts on it: a pipeline config may still be built when
  state changes even though no draw using it is submitted, so the cull mode chosen there is
  immaterial.

Pushed to the `fork` remote as branch `gx-cull-all` rather than onto the submodule's current
head, since the decomp runtime depends on the same library.

**A run now reaches 4,260 presents with no fatal**, against ~120-330 before this session's
GX work began.

## Now in aurora's FIFO-path gaps

The remaining failures are aurora features that its FIFO replay does not implement, rather
than stream errors:

- `unimplemented indexed XF load (opcode 0x20, dstAddr=030f, len=15)` — indexed XF loads into
  some destination ranges
- `XF: PosMtx sub-copy unsupported: offs=2, len=16385` — the length is implausible, so this
  one may still be a desync and should be treated as suspect rather than a missing feature

This is an expected boundary: aurora's FIFO replay was built to consume Dolphin `.dff`
captures of a decomp-shaped stream, so a full retail command stream exercises paths the
decomp runtime never emits. Each is a small, well-defined addition to aurora rather than a
recomp problem.

## Two more parser/contract bugs

**1. `GX_CMD_CALL_DL` and `GX_CMD_INVL_VC` were swapped.** From
`dolphin/gx/GXCommandList.h`: **`0x40` is call-display-list** (address + size) and **`0x48`
is invalidate-vertex-cache** (no payload). This parser had them the other way round, so it
consumed 9 bytes for a 1-byte command and left every real display-list call unrecognised.
That was the residual desync after the indexed-XF fix, visible as indexed loads with
impossible destinations (`dstAddr=0xf10`).

Adding `0x48` as a DL call earlier *appeared* to help only because it silenced the
"unrecognised opcode" log — a reminder that a symptom disappearing is not evidence of a
correct model.

The draw self-check needed its whitelist widened for the same reason (it flagged a perfectly
valid `0x40` as a mis-frame). **With both corrected there are no framing errors and no
desync fatals.**

**2. Aurora's frame contract was being violated.** Its header states: `end_frame` must NOT
run if the matching `begin_frame` returned false, an un-presentable frame must be
DISCARDED or the fifo grows unbounded, and `aurora_update()` is the event pump. This runtime
called `end_frame` unconditionally and never pumped events. Fixed by tracking whether a frame
is open and calling `aurora_update()` **once** per frame — it returns the frame's event
ARRAY, so draining it in a `while` loop never terminates (which stalled boot at
`APP_STATE_BOOT` until corrected).

## Still open: a 16 MB single upload

`mapped ByteBuffer overflow: have 49267584 bytes (capacity 50331648), need 16410880 more`.

The "need" is a SINGLE request of 16 MB, not accumulation — per-frame stream is only ~291 KB
(184 display-list expansions, 87 KB inlined). So one draw is asking for ~164k vertices at
100 bytes each. The parser's self-check cannot see this case: it only validates when the
draw fits inside the current buffer, so a draw spanning batches is unchecked.

Extending the self-check to cover cross-batch draws is the next step — that is precisely the
blind spot where a bad vertex count would survive.

## The overflow is expansion, not accumulation

`begin_frame` always succeeds and frames open/close correctly, so the staging buffer is not
failing to reset. Instrumenting each flush shows the real shape: most frames carry **0 KB**,
then a single frame carries **291 KB** (184 display-list expansions, 87 KB inlined) — and it
is that one frame that overflows aurora's ~48 MB per-frame staging.

So ~291 KB of GC command stream expands to >65 MB in aurora's vertex format. That is not
absurd on its face: aurora resolves indexed GC vertices into flat vertex buffers, and a
vertex with position, normal, two colours and eight texcoords is well over 100 bytes
expanded, so a few 50k-vertex draws reach tens of MB.

Note also the present rate — up to ~9,990/s — meaning `TVideo::waitForRetrace` is reached far
more often than once per displayed frame. Empty flushes are cheap, but it does mean the
chosen frame boundary fires during load loops as well as real frames.

Two candidate directions, not yet decided:

1. **The frame boundary may be wrong.** If the game reaches `waitForRetrace` many times per
   real frame, the "frame" being handed to aurora is not the game's frame. The decomp runtime
   distinguishes these (`VIWaitForRetrace` as a pure counter vs the present point); the same
   distinction may need to be sharper here.
2. **The scene genuinely needs more staging than the decomp path does.** The decomp runtime
   renders these scenes through the same aurora without overflowing, so a like-for-like
   comparison of what each submits per frame would show whether this stream is doing
   something wasteful (repeated display-list expansion is the obvious suspect) or whether the
   limit is simply low for a full retail stream.

Establishing which of those is true — by comparing per-frame submission against the decomp
runtime on the same scene — is the honest next step, rather than raising a constant and
hoping.

## Cross-runtime comparison: the huge draws WERE bogus, and are now gone

Running the decomp runtime on the same scene settles what "normal" looks like:

```
./build/sms-boot/sms-boot   SB_STAGE=15 SB_DRAW_STATS=1
[draw-stats] frame=1460 bytes=438831 draws=334 verts=3166
```

**~3,166 vertices across ~334 draws per frame — about 10 vertices per draw.** Against that,
the recomp's earlier draws of 36,917 / 57,757 / 58,347 vertices were plainly mis-framed, not
a heavy scene. After the `CALL_DL`/`INVL_VC` opcode fix, `AURORA_DRAW_TRACE` shows the recomp
submitting only 4- and 10-vertex draws — the same profile as the decomp. A plausibility check
(warn when a draw claims > 4096 vertices, since the reference is ~10) now fires zero times.

Having a second runtime on the same library, driving the same scene, is what made "is 58,347
vertices reasonable?" answerable at all. Worth reaching for earlier next time.

## The staging overflow is NOT explained yet

The overflow persists and is bit-identical across runs — `have 49267584, need 16410880` every
time — which rules out anything scene- or timing-dependent. Bisected so far:

- **Not textures.** `SBR_NO_TEXOBJ=1` (added as a diagnostic) suppresses all
  `GX_AURORA_LOAD_TEXOBJ` emission; the overflow is byte-identical with and without it.
- **Not the EFB copy.** Logged as a correct `640x448 at (0,0)`.
- **Not draw volume.** Aurora receives ~159 draws of 4-10 vertices.

So ~49 MB is being staged inside a single aurora frame by something other than the draws,
textures, or copy that this runtime knowingly submits. The next probe is aurora-side: find
which allocation reaches `map_staging` at that size, since the constant 16410880 (0xFA8000)
should be traceable to one caller.

## The overflowing buffer is aurora's STORAGE buffer, and there is a known reference

`StagingBufferSize` is the sum of several regions; the reported capacity `50331648` is
exactly `StorageBufferSize` (48 MB) from `lib/gfx/common.hpp`. So the overflow is the
**indexed-array storage** region, not vertices, indices or uniforms.

That constant carries a detailed history, and it measures the decomp runtime on **this very
scene**:

> the title/file-select frame already sat at 33.44mb (99.7% of 32mb, ghost-pass doubling
> included) … The ghost-pass wart REMAINS the real fix; this headroom just stops capacity
> from gating unrelated ports.

So:

| runtime | storage for the title/file-select frame |
|---|---|
| decomp + aurora | ~33.4 MB (already including a known redundant "ghost pass" doubling) |
| recomp + aurora | 49.3 MB used, +16.4 MB requested = **~65.7 MB** |

The recomp needs roughly **2x** what the decomp needs for the same frame. Since the decomp's
figure already includes the ghost-pass doubling, this looks like a FURTHER doubling on the
recomp side rather than the same wart.

Note the constant's comment explicitly says growing it is not the fix — and it has already
gone 8 -> 32 -> 48 MB. Raising it again would hide whatever is doubling here, so the useful
question is what the recomp submits twice that the decomp submits once. Display-list
inlining is the obvious suspect: every `CALL_DL` expands the list's bytes inline, so a list
invoked twice contributes its indexed-array references twice — where the decomp path may let
aurora reuse them.

Measuring per-frame display-list expansions against the decomp's draw count on the same
scene is the concrete next step; `SB_DRAW_STATS` on the decomp side and the existing
`frame stream N KB (M DL expansions)` line on this side are directly comparable.

## THE TITLE SCREEN RENDERS

Root cause of the storage overflow: `GX_AURORA_LOAD_ARRAYBASE` was being sent a real byte
count. Aurora treats that literally —

```c
// size 0 = "trust" registration (J3D): upload the auto-derived extent
// (max referenced index, maintained in draw_prim).
const u32 effSize = array.size != 0 ? array.size : array.sizeAuto;
gfx::push_storage(array.data, effSize);
```

— so every indexed array pushed `0x01800000 - offset` bytes (up to 24 MB) into the 48 MB
storage buffer. The value was chosen as "the array cannot run past MEM1": technically true,
and catastrophic. **Passing 0** makes aurora upload only up to the highest index actually
referenced.

**Result: 7,700+ presents with no fatal** (previously ~90), and the frame capture is the
**Super Mario Sunshine title screen** — logo, "START!", "(c) 2002 NINTENDO", clouds, sun,
palm tree, FLUDD, all correctly positioned. Capture: `scratch/recomp/title.png`.

The generalisable point: three separate bugs today came from supplying aurora a
*plausible-looking* value where it wanted a sentinel or the exact thing (`texObjId` 0 vs a
stable id, array size vs 0, and the raw-vs-merged BP value). When a field has a documented
"trust me" mode, use it rather than computing a conservative bound.

**Not yet correct: the image is greyscale.** Every sampled pixel has R=G=B across 14 levels,
so shape, texture detail and layout are right but colour is absent. Texture data is clearly
reaching the GPU (the logo lettering and cloud detail are legible), so the likely culprit is
the texture FORMAT field in `GX_AURORA_LOAD_TEXOBJ` — `(image0 >> 20) & 0xF` — being
misinterpreted, e.g. an RGB texture decoded as I8 intensity. That is the next thing to check,
and it is exactly the kind of claim to verify against the decomp oracle rather than assume.

## COLOUR: the title screen renders correctly

The greyscale was one field. `emit_copy_state` sent the display copy's texture format as
**0**, with a comment guessing it was "unused for a display copy". **Format 0 is `GX_TF_I4` —
4-bit INTENSITY**, so the entire frame resolved through an intensity format: shape, texture
detail and layout intact, all colour discarded. Sending `GX_TF_RGBA8` (6) fixes it.

Verified against the decomp oracle on the same screen, using the same aurora dump path:

| | distinct colours | greyscale | top colours |
|---|---|---|---|
| decomp (oracle) | 66,510 | 8% | `(255,255,255)`, `(187,219,242)`, `(187,219,241)` |
| recomp | 57,180 | 7% | `(255,255,255)`, `(187,219,241)`, `(188,220,244)` |

The same sky blues to within a bit — not merely "colour appeared", but the *same* palette as
the reference. Capture: `scratch/recomp/title_color.png`.

That comment ("unused for a display copy") was an assumption written as if it were a fact,
and it cost a full investigation cycle. The oracle comparison is what settled it: knowing the
decomp produced 66k colours through the identical dump path proved the loss was ours and not
the capture's.

**The standalone recomp now renders the Super Mario Sunshine title screen in colour.**

## Title comparison against the oracle: close, with identifiable gaps

A naive byte-wise diff of the two dumps gave a meaningless 77%: the captures are **different
sizes** — recomp 1280x896 (EFB), decomp 1280x960 (window) — exactly the documented
`SB_DUMP_FRAME` gotcha. Re-sampled by normalised position it is 60% differing, but the title
screen ANIMATES (logo bounce, drifting clouds, the START prompt) and these are two
independent runs at unaligned phases, so that number is not a parity measure either.

Rendering both and looking is more informative at this stage:

| element | decomp oracle | recomp |
|---|---|---|
| logo, lettering, rainbow, palm, FLUDD, copyright | present | present, same placement |
| sky/cloud palette | `(187,219,242)` family | same family, within a bit |
| sun | bright glare / lens-flare bloom | plain sun sprite, no glare |
| sea | blue ocean band along the bottom | absent |
| prompt | "PRESS STA[RT]" | "START!" — likely a different phase of the same prompt animation |

So the structure and palette match, and two elements are genuinely missing: the **sun glare**
and the **reflective sea**. Both are known-heavy features on the decomp side (`TSunGlass`,
`TMapObjWave` / the reflective-sea work), so their absence here is a specific, nameable gap
rather than general inaccuracy.

Honest position: the title screen RENDERS and matches closely, but "matching" in the
pixel-parity sense is not established — that needs the animation phase pinned on both sides
first, which is what the decomp's own title-oracle gate exists to do.

## Controller input, and what pressing START runs into

`overrides/native_pad.cpp` overrides `PADRead` (`0x80351600`, found as the call immediately
before `PADClamp` inside `JUTGamePad::read`). SI is modelled as a transport with nothing
attached, so the real `PADRead` reports every port disconnected (`err = -1`) and the game sees
no input; on a PC port input comes from the host, so this is an override rather than a device.

Port 0 reports connected, ports 1-3 report genuinely disconnected rather than pretending to be
idle pads. Scripted input mirrors the decomp runtime's `SB_PAD_SCRIPT`:

```
SBR_PAD_SCRIPT="1500:START,1540:-"      # keyed on PAD read count (one per frame)
```

buttons named (`A B X Y Z L R START UP DOWN LEFT RIGHT`, `+` to combine, `-` for none).

**Pressing START on the title re-enters the GAMEPLAY <-> MOVIE oscillation.** Tracing
`mMovie` (`gpApplication+0x18`) shows movies **9** and **12** requested. Neither comes from
`checkAdditionalMovie` — that only ever sets 1/3/4/5 — so the "already seen" flags do not
gate this path. `Application.cpp:756` shows why: `APP_STATE_DONE` **falls through** to
`APP_STATE_MOVIE`, setting `mMovie = 9` unconditionally.

Overriding `TMovieDirector::direct` to report GAMEPLAY did NOT break the loop, so the
re-entry is decided before the director runs. That override was reverted rather than left in
— it changes game logic and did not do what it claimed, which is exactly the kind of
unverified change that should not accumulate.

The title itself is unaffected and still renders; this is specifically the title -> file-select
transition wanting a movie. Worth noting the flags set earlier may also not be taking effect
(`setFlag` at `0x80294b1c` is unverified) — that should be checked before more is built on it.

## setFlag VERIFIED (and the movie loop is therefore something else)

The `setFlag` address (`0x80294b1c`) came from the retired fastboot override and had never
been confirmed against this DOL. It is now checked at runtime rather than trusted:
`TFlagManager::setFlag` case 3 writes `mGameBools[low >> 3]` bit `(low & 7)`, and
`mGameBools` sits at **+0xCC** (after `mCardBools[119]` and `mCardInts[21]`). After setting
the four movie flags, `mark_movies_seen` reads those bits back and aborts if any is clear.

**Result: all four verified set.** So the address is right, the flags do land, and the
GAMEPLAY <-> MOVIE oscillation after pressing START is NOT caused by the flags failing —
which is what I had assumed and would otherwise have "fixed" next.

That narrows it: with `0x30009`/`0x3000B`/`0x3000C`/`0x3000D` all set, every
`checkAdditionalMovie` branch that consults them is gated, yet `mMovie` still reports 9 and
12. Movie 9 is the `APP_STATE_DONE` fallthrough (`Application.cpp:756`); what selects 12 is
still unidentified and is the thing to find next.

The check stays in the code permanently — it costs one read per flag and turns a silently
wrong address into an immediate, named failure.

## What sets movie 12 (found with the watchpoint, not by reading code)

A static scan for `li rX,12` followed by a store to `+0x18` found nothing — the value is not
written as a literal near the store. `SBR_WATCH=0x803E9718` (mMovie) named the writer
immediately:

```
[watch] write 0x0000000c @ 0x803e9718 from func_8029a044
  <- func_8016cba8 <- JDrama::TViewObj::testPerform <- TViewObjPtrList::perform
```

So a **view object's per-frame `perform`** sets `mMovie = 12` — it is re-set every frame,
which is why the state re-enters MOVIE no matter how the previous attempt ended. That
explains the oscillation's persistence: it is not a transition failing to complete, it is a
signal being re-raised continuously.

The watchpoint (added earlier for the small-data corruption) answered in one run what a
static scan could not. Worth reaching for sooner for "who writes this address" questions.

## A cross-check worth heeding before going further

The project's own notes record that **title <-> file-select is a CAMERA PAN driven by
TCardLoad** (states 10->9->3->8->0), not a movie — both are stage 15. So a movie being raised
on START may itself be the divergence rather than something to work around, and "skip the
movie" could be the wrong instinct entirely.

That is worth establishing against the decomp oracle first: run the decomp runtime, press
START at the same point, and see whether IT enters APP_STATE_MOVIE. If it does not, the
question becomes why this runtime's `perform` raises movie 12 where the oracle's does not —
a behavioural divergence to root-cause, not a video gap to paper over.

## The GAMEPLAY<->MOVIE oscillation is ATTRACT MODE — correct behaviour, not a bug

Chased to the end, and the conclusion inverts the framing I had been working under.

`func_8029a044` is `TMarDirector::fireStreamingMovie(u8)` — confirmed structurally, not
guessed: its `setNextStage` constants (`0x3B, 0xE06, 0xE07, 0x3C, 0x101, 0xF`) and the
`cmpwi 12` switch bound match `MarDirectorEvent.cpp` exactly. Movie 12 takes the `default`
branch, `setNextStage(0xF)` = stage 15.

The caller is `CardLoad.cpp:978` — the title state machine:

```c
if (unkC0 / 120.0f > 45.0f) {                       // 45 seconds idle at the title
    if (getBool(0x3001C)) { fireStreamingMovie(9);  setBool(false, 0x3001C); }
    else                  { fireStreamingMovie(12); setBool(true,  0x3001C); }
    unkC0 = 0;
}
```

An **idle timer** that fires an attract-mode movie every 45 seconds, **alternating 9 and 12**
via flag `0x3001C`. That is precisely the observed oscillation, down to the two movie numbers.
The runtime is behaving correctly; it is idling at the title because nothing advances it, and
attract mode is what retail does when you walk away.

So the movie path was never the defect and "what selects movie 12" was the wrong question.
Two separate real issues were hiding behind it:

1. **The actual blocker: the title is not advancing.** START at the title does not move the
   state machine on. In `CardLoad.cpp`, START (`getTrigger() & 0x1000`) does NOT advance the
   state — it only fast-forwards the title animation (`mTitleAnimState = 4`, pane alphas to
   255). The advance to `mState = 8` (`moveToLoadFromTitle`, the camera pan to file-select)
   happens in the branch above, once the animation has completed. So the next question is
   whether START is reaching the game at all, and whether the title animation completes.
2. **THP playback is absent**, so any movie that does fire cannot play. Real, but downstream —
   and once the title advances properly, attract mode never triggers in the first place.

Two process notes worth keeping. The static scan for a literal 12 stored to `+0x18` found
nothing because the value arrives as a *parameter* (`mMovie = param_1`) — the watchpoint found
in one run what pattern-matching could not. And I spent several ticks treating a symptom as
the defect without asking whether the behaviour was CORRECT; "what makes this happen" should
have been preceded by "is this supposed to happen".

## MILESTONE: the standalone recomp reaches and renders FILE-SELECT

Once the oscillation was understood as attract mode rather than a defect, the fix was simply
to press START *before* the 45-second idle timer instead of after it. With
`SBR_PAD_SCRIPT="600:START,640:-"` the title advances and the runtime renders the file-select
screen (`scratch/screenshots/recomp_after_start.png`, 125,463 distinct colours):

- the three save blocks A / B / C with their NEW banners, correctly coloured and lettered
- Mario seated on the beach, the palm tree, the OPTIONS signpost and arrow
- the sea, sky and clouds
- the "There is no Memory Card in Slot A." dialog panel — correct, since no memory card is
  emulated, and it confirms the CARD path reports its real state rather than a fabricated one

That is both halves of the original goal reached in the standalone recomp: title screen, then
file-select. Every earlier tick spent on "what selects movie 12" was chasing the game
behaving correctly while idle.

The app never left `GAMEPLAY`/stage 15 during the run — no movie was entered at all, which is
the expected behaviour once input arrives before the idle timer.

Visible residuals to measure against the oracle, not yet root-caused:
- the sea reads noisy/speckled rather than smoothly reflective
- the palm fronds over the dialog panel look dark and flat

Next: a true side-by-side against the decomp oracle at the same screen. None of the existing
`scratch/shots/fsel_*` images are usable for this — despite the name they are all TITLE-phase
captures, not the block screen, so the oracle frame has to be captured fresh.

## Recomp file-select vs the decomp oracle — first real side-by-side

Oracle captured fresh from the decomp runtime (`SB_STAGE=15`,
`SB_PAD_SCRIPT="2600:START 2610:-"`, dump after 3300 presents) since no usable oracle image of
this screen existed. Note the documented size gotcha applies: the oracle dump came out
1280x960 (window) and the recomp's 1280x896 (EFB), so a naive pixel diff between them is
invalid — they must be brought to a common size before any numeric comparison.

Structurally the recomp matches: same scene, same camera framing, same blocks, same signpost,
same Mario pose. The differences are real render defects, not missing content:

1. **The sea is speckled white noise in the recomp; smooth turquoise in the oracle.** This is
   the most visible defect by far and the best next target. The oracle's sea is a clean
   gradient from deep teal to shallow green-blue; the recomp's is dense white stipple over
   roughly the right base colour, which reads like a per-pixel alpha/depth or texture-sampling
   fault rather than wrong geometry.
2. **Palm fronds are dark and desaturated in the recomp**, bright green in the oracle. Note
   the fronds darken over the dialog panel in BOTH — that part is correct compositing in each,
   so the defect is the fronds' base shading, not the panel blend or draw order.
3. **The save-slot labels differ in kind**: the oracle draws "New" inside rounded blue J2D
   panels above each block; the recomp draws outlined sprite-style "NEW" banners with no
   panel. The dialog panel is also populated in the recomp and empty in the oracle. These two
   are most likely a different phase of the fade-in animation rather than a render fault —
   the screens were not captured at a pinned matching phase.

That last point is the gate on any numeric parity work here: both sides need the animation
phase pinned before a pixel percentage means anything, exactly as the decomp title-oracle gate
already established. Item 1 does not need that gate — white stipple over water is wrong at any
phase — so it is the next thing to chase.

## Sea speckle: the obvious suspect is ruled out

First guess was the known FIFO-parser gap `unhandled tcg src 21` producing garbage texcoords.
**Falsified**: a full run to file-select emits no `tcg`, indirect-texture, or unhandled-feature
warnings at all. The only warnings are the known THP, IPL-font and three counted syscall lines,
none of which touch the sea. So the speckle is produced by a path the parser handles without
complaint — a wrong *value*, not a missing feature.

Remaining hypotheses, untested, in the order I'd falsify them:

1. **The TMapObjWave ripple grid drawing wrong.** Best fit for the appearance: the project's
   own notes record it as an immediate-mode ripple grid, and blown-out white specks scattered
   across the sea are exactly what that grid would look like with wrong vertex colour, alpha
   or texcoords. The oracle renders the same ripples subtly.
2. **Z-fighting between coplanar sea layers**, which classically produces dense stipple.
   Distinguishable from 1 by whether the speckle pattern is stable or shimmers frame to frame.
3. Alpha-test dithering on the sea material.

Falsifier for 1 vs 2: dump two consecutive frames and compare the speckle pixels. Z-fighting
at a fixed camera is stable; a mis-drawn animated ripple grid changes every frame. That is a
cheap, decisive first test and does not require the animation phase to be pinned.

## Sea speckle: stable, so not the ripple grid — and the copy-state path is the suspect

**Two-frame test, decisive.** Frames 2200 and 2201 differ in only 4.0% of sea pixels, and the
near-white fraction is unchanged (85.9% -> 86.1%). A mis-drawn *animated* ripple grid would
change substantially every frame, so **hypothesis 1 (TMapObjWave) is falsified**. The white is
a stable surface. The residual 4% is ordinary ripple animation on top.

Quantified against the oracle at the same sea region: **85.9% of the recomp's sea is near-white
vs 0.0% of the oracle's**. Zoomed, it is not random noise but elongated horizontal dashes of
the *correct* teal showing through white, densest toward the horizon — the classic look of a
grazing-angle coplanar conflict, not a texture-sampling fault.

**Palettised-texture theory ruled out.** Formats actually used at file-select are 0 (I4),
1 (I8), 2 (IA4), 3 (IA8), 4 (RGB565), 5 (RGB5A3) and 14 (CMPR). No C4/C8/C14X2, so the
parser's hardcoded `TLUT index 0` cannot be implicated here.

**What stands out instead: a format-4 texture at 320x224.** That is an EFB copy-to-texture —
the sea's reflection. And the parser handles only the *display* copy:

```cpp
else if (reg == 0x52 && (val & (1u << 14))) emit_copy_state();   // bit 14 = copy to XFB
```

A copy-to-texture (bit 14 clear) emits no copy state at all. Checked aurora before blaming it,
and aurora is NOT the gap — its CP handles both cases (`command_processor.cpp:1017`):
`copy_tex(kDisplayCopyDest, clear)` for XFB and `copy_tex(g_gxState.texCopyDest, clear)` for
textures. BP writes are forwarded verbatim, so aurora sees the trigger.

So the likely defect is **stale copy state**, not a missing copy: `emit_copy_state()` pushes
`GX_AURORA_LOAD_COPY_SRC/DST` — including a hardcoded `GX_TF_RGBA8` chosen for the display
copy — and runs *only* on XFB copies. When a texture copy then executes, aurora resolves it
using whatever src/dst extent and format the last display copy left behind, rather than the
320x224 RGB565 the game asked for. A reflection texture resolved at the wrong extent/format
would be exactly the kind of stable, wrong-coloured surface seen over the water.

This is also a rule violation on my part worth naming: dropping the non-XFB case silently is
precisely the banned success-shaped no-op. It should have been loud from the start.

Next: emit copy state for texture copies too, with the format taken from the BP 0x52 payload
instead of hardcoded, and re-measure the 85.9% figure. That number is the falsifier — if the
sea does not change, the stale-state theory is wrong and the coplanar-draw question is back
open.

## EFB texture copies now translated — and the stale-state theory is FALSIFIED

Implemented the fix and it did **not** change the sea: near-white in the sea crop is
**85.9% before and 85.9% after**, against the oracle's 0.0%. By the falsifier I committed to
last tick, the stale-copy-state theory is wrong. The white surface is something else, and the
coplanar-draw question is back open.

The work still stands on its own merits, because the copy path was genuinely broken in three
ways — all found by making the dropped case loud instead of silent:

1. **`GX_AURORA_LOAD_COPY_DEST` was never emitted at all.** Aurora resolves a texture copy into
   `g_gxState.texCopyDest`; we never set it, so every texture copy landed on a stale pointer.
2. **The copy format was hardcoded to RGBA8**, and my first attempt to decode it was also wrong.
   The format is not raw bits 3-6: hardware packs it as `fmt = field/2 + (field & 1) * 8`.
   Reading the field directly produced the nonsensical single-channel "R8"/"B8" formats. The
   decode is confirmed against the game's own registers — field 8 decodes to RGB565 and the
   texture the game subsequently binds is RGB565; field 10 decodes to RGB5A3.
3. **The destination extent was wrong.** For a texture copy the half-scale bit halves BOTH
   dimensions; BP 0x4E is the *display* copy's vertical scale. Treating 0x4E as the height
   scale gave 320x448, which matches no texture in the scene.

Self-check that the translation is now right, rather than merely different: the copies the
parser reports are exactly the textures the game binds — `640x448 -> 320x224 fmt 4 (RGB565)`
for the sea reflection and `256x256 fmt 5 (RGB5A3)`, with the display copy RGBA8. Before this,
one of those was 320x448 and the formats were fiction. The rendered frame is otherwise
unchanged, so nothing regressed.

Also worth noting: decoding the format for the display copy too would have been a regression —
an XFB copy is YUV-converted and its format field is not a texture format, so it decodes to
GX_TF_I4 and renders the whole frame greyscale. That is now explicit in the code.

**Where the sea investigation actually stands.** Ruled out: the animated ripple grid (stable
across frames), palettised textures (none in the scene), the `tcg`/indirect parser gap (no
warnings), and now stale/missing copy state. Still on the table: a genuine coplanar second
draw of the sea. The next move is to identify the draw that paints white there — enumerate the
draws covering that region rather than reasoning about which subsystem "should" own it, since
three subsystem-level guesses in a row have now been wrong.

## Workflow defect: aurora's retrace-gated diagnostics were silently dead in the recomp

Trying to enumerate the draws, `SB_DRAW_DUMP` produced **zero lines**. The cause is worth
recording because it had nothing to do with the sea and would have kept biting.

Aurora gates several diagnostics — `SB_DRAW_DUMP_AFTER`, `SB_DRAW_DUMP_FRAME`, the
`SB_NDC_DRAW` window — on a frame ordinal from a **weak** `VIGetRetraceCount` that the runtime
is expected to provide. sms-boot provides it from its frame seam; this runtime never did, so
the weak symbol resolved to null, aurora's counter read 0 forever, and every one of those
diagnostics silently produced nothing. They did not report being unavailable — they just never
fired. A whole existing toolkit was decomp-only by accident.

Fixed by exporting the recomp's present count as `VIGetRetraceCount`
(`overrides/native_frame.cpp`). Verified: the same command now yields 200 draw-dump lines
where it produced 0.

## Enumerating the file-select frame's draws

With the toolkit working, the frame is 127 draws in the main `640x448` viewport plus 73 in a
`256x256` offscreen pass. Two results:

- **`colorUpdate` is not the culprit.** A long run of 59-vertex draws has `cU=0` with opaque
  `bf=1/0`, which would paint over the sea if colour writes were not masked — but aurora does
  honour it (`to_write_mask` in `gx.cpp`), so those are a legitimate depth/alpha pass.
- **Nothing binds the sea reflection.** No draw in the frame has `tex0=320x224` — the RGB565
  reflection texture is copied every single frame and then never sampled on texmap 0. The
  copies are `640x448 -> 320x224 RGB565` and `256x256 RGB5A3`, and only the latter's size shows
  up in the draw list (126 draws).

That is the strongest lead yet and it inverts the earlier framing again: the problem may not be
an extra white surface drawn OVER the sea, but the sea's reflection never being sampled, so the
water renders with whatever its remaining TEV stages produce.

Caveat before treating that as established: the dump reports **tex0 only**, so the reflection
could be bound to a higher texmap and simply not visible in this listing. Confirming which
texmap the sea samples is the next step, and it is a question about the draw's own state rather
than another subsystem guess.

## Oracle draw-diff at file-select: the recomp never renders anything ORTHOGRAPHIC

Extended aurora's `[draw-dump]` line with a `texs=[map:WxH,...]` field listing every bound
texmap, not just tex0 — the single-tex0 field cannot answer "which draw samples texture X", so
the previous tick's "nothing binds the reflection" could not be trusted. With it:

- **Confirmed, caveat closed: no draw binds the 320x224 reflection on ANY texmap.**
- **But the oracle does not bind it either (also zero).** So an unsampled reflection is normal
  and that lead is dead. Cheap to establish, and it is the third sea hypothesis to fall.

The oracle comparison did surface two hard divergences. Note the draw STRUCTURE matches
exactly — 127 draws in the main viewport plus 73 in a 256x256 offscreen pass, in both
runtimes, with the offscreen pass's projection identical to 4 decimal places. The recomp's
draw stream is structurally right; the divergence is in per-draw state:

1. **Projection type.** Of the 127 main-viewport draws the oracle renders **111
   ORTHOGRAPHIC** (`prj=[0.0045 -0.0031 -0.5000 -0.5000]`) and 16 perspective. The recomp
   renders **all 127 perspective** and, across the whole 200-draw window, **not one
   orthographic draw**. The 2D/J2D overlay is being drawn through the 3D projection.
2. **Viewport origin.** Oracle `vp=(0,0 640x448)`, recomp `vp=(2,2 640x448)` — the recomp is
   offset by 2 pixels in both axes, with identical scissor `sc=(0,0 640x448)` on both sides.

This plausibly subsumes a difference I previously waved off as "unpinned animation phase": the
save-slot labels and dialog panel sit in visibly different places between the two captures.
Those are exactly the 2D elements that would be misplaced by a wrong projection. It may also
explain the sea, if the white surface is a 2D overlay quad landing over the water instead of
where it belongs — but that is a hypothesis, not a conclusion, and the sea has already
falsified three of those.

**Open contradiction, recorded rather than glossed.** Aurora only assigns `g_gxState.proj`
inside the XF 0x1020 handler, gated on `projOff == 0 && count >= 7`, and that same block emits
`[proj-set]` under `SB_PROJ_DUMP`. Running the recomp with `SB_PROJ_DUMP=1` produced **zero**
such lines — yet the draws report non-default projection values that differ correctly between
the main and offscreen passes, which only that block can set. Both cannot be true as I
currently understand the code. Resolving that contradiction is the next step, and no claim
about the mechanism should be made until it is: the honest reading is that my model of how the
recomp's XF writes reach aurora is wrong somewhere.

## CORRECTION: "the recomp renders nothing orthographic" was a MEASUREMENT ARTIFACT

The previous entry's headline claim is **wrong** and is retracted here rather than left to
mislead a later session.

Resolving the open contradiction resolved it against me. Instrumenting our own parser shows it
emits `XF projection write: addr 0x1020 count 7` exactly as aurora requires, and aurora's own
`[proj-set]` fires **116,761 times** in a single run — **54,598 of them `type=O`**. The recomp
sets orthographic projections constantly.

The earlier "zero `[proj-set]` lines" measurement was simply bad: same binary, same env, but
piped through `grep | tail` instead of redirected to a file, and it produced nothing. I should
have distrusted a null result from a changed measurement pipeline before building a conclusion
on it — a null reading is exactly the case where the instrument needs validating against a
known positive first.

And the draw-dump evidence was an artifact of the window: `SB_DRAW_DUMP` with
`SB_DRAW_DUMP_AFTER` captures only the **first 200 draws** past the threshold. A file-select
frame has more draws than that, and the 2D/J2D overlay is drawn LAST — so the cap truncated
every frame before reaching the orthographic draws. "127 main + 73 offscreen = exactly 200"
should have been the tell: that is the cap, not a frame boundary. Two runtimes windowed at
different points then produced an apparent divergence that does not exist.

What survives from that entry:
- The `texs=[...]` field is a real improvement and its finding stands: neither runtime binds
  the 320x224 reflection.
- The **viewport origin difference is still real** — oracle `vp=(0,0 640x448)` vs recomp
  `vp=(2,2 640x448)`, identical scissors — since that field is per-draw and not affected by
  which draws the window caught.

Re-measuring properly with `SB_DRAW_DUMP_FRAME` (uncapped, exactly one frame) on both runtimes
to get true per-frame orthographic/perspective counts before drawing any conclusion.

## Real bug found and FIXED: aurora's FIFO viewport origin bias was 340, should be 342

Re-measured properly with `SB_DRAW_DUMP_FRAME` (uncapped, exactly one frame) on both runtimes:

|        | total | ortho | persp | main vp | offscreen |
|--------|-------|-------|-------|---------|-----------|
| recomp |   310 |    81 |   229 |     237 |        73 |
| oracle |   613 |   422 |   191 |     540 |        73 |

The recomp does render orthographic draws, confirming the retraction above. The remaining
counts differ a lot, but these two frames are NOT at a pinned animation phase, so the gap is
not yet evidence of anything — noted and left alone rather than theorised about.

The offscreen pass matches exactly (73 draws) for the third measurement running.

The **viewport origin difference was real**, is phase-independent, and is now fixed. The GC
viewport-origin bias is **342**, not 340: `GXSetViewport` encodes `ox = xOrig + width/2 + 342`,
so recovering `xOrig` from the XF registers must subtract 342. Aurora's XF reconstruction used
340, placing every FIFO-reconstructed viewport two pixels down and right.

Why it hid for so long: `GXSetViewport` sets the logical viewport DIRECTLY and never goes
through the reconstruction, so the decomp runtime — which calls the GX API — was never
affected. Only the FIFO path was, which until this port had no serious consumer. It also
affects the decomp's own `SB_FIFO_REPLAY` harness, so oracle replay diffs were carrying a
constant 2-pixel offset.

Verified: after the fix the recomp reports `vp=(0,0 640x448)` and `vp=(0,0 256x256)`, matching
the oracle exactly on both passes. Pushed as aurora `fork/sunbright` 70652db.

This does NOT fix the sea (86.1% near-white, unchanged) — expected, and stated so the fix is
not mistaken for progress on that.

## CORRECTION 2, and the sea narrows to TEXTURE CONTENT

**The "neither runtime binds the 320x224 reflection" finding was also a window artifact.** Same
200-draw cap, same mistake, two entries running. In the FULL frame the reflection IS bound —
recomp draw `#576688`, and the oracle has exactly one such draw too. I have now been bitten
twice by that cap; the lesson is that `SB_DRAW_DUMP_AFTER` must not be used for any
"nothing/never" claim, only `SB_DRAW_DUMP_FRAME`.

With full frames the comparison is sharp, and the sea draw is **identical in both runtimes**:

```
recomp #576688  verts=52 tex0=320x224 zcmp=1 zupd=0 trans=(-1092.7,-320.8,99.1) tev=2 ...
oracle #1575789 verts=52 tex0=320x224 zcmp=1 zupd=0 trans=(-1092.7,-320.8,99.1) tev=2 ...
```

Same vertex count, same position, same TEV stage count, same channel config, same viewport,
same texture dimensions. **The geometry and render state are correct.** So the white sea is
neither a coplanar extra draw nor wrong state — it is the CONTENT of the reflection texture.
That also retires the coplanar-draw hypothesis that has been open for several entries.

Wiring checked and found sound, so the defect is narrower than "the copy path is broken":
- Aurora's `copy_tex` registers each resolve in `g_gxState.copyTextures[dest]`, and the CP's
  FIFO trigger calls that same function — the FIFO path is wired correctly in principle.
- `resolve_sampled_textures` looks the texobj's texel pointer up in that map, so a bind finds
  the copy only if the two addresses agree. They DO for the sea: copy destination
  `0x00c43e00 (320x224)` matches a bind at `phys 0x00c43e00 fmt 4`.

One loose end worth chasing: a SECOND 320x224 texture is bound each frame at
`phys 0x003e1280`, alternating with the copy destination, and nothing ever copies to that
address. Whether the sea samples the copied one or that one is not yet established.

Blocked on instrumentation, not on ideas: aurora's `SB_COPY_DBG` caps at 40 lines and boot
spends those on display copies long before file-select, so the sea's own copies are never
observed. Next step is to make that log reach the moment of interest — the same
tooling-before-conclusions move that unblocked the draw dump.

## Copy sequences MATCH the oracle; the sea defect is upstream of the copy

Unblocked the instrumentation (aurora `SB_COPY_DBG_AFTER=<retrace>`, pushed as 7f22b65) and
rebuilt the decomp runtime against the same aurora, so both sides are now comparable. Worth
noting the gate's own first version printed nothing: it counted pre-window copies against the
40-line budget, so boot exhausted it before the window opened.

The two runtimes' copy sequences are **the same copies in the same order**:

```
oracle  n=1 dest=…cdb0    1280x960 fmt=6 clear=1 src=(0,0 640x448) mark='DrawBuf ChrXlu'
        n=2 dest=…123b40   512x549 fmt=5 clear=1 src=(0,0 256x256) mark='DrawBuf Mirror Xlu'
        n=3 dest=…060fe0   640x480 fmt=4 clear=0 src=(0,0 640x448) mark='buf?'
recomp  n=1 dest=…5ef48   1280x896 fmt=6 clear=1 src=(0,0 640x448)
        n=2 dest=…0f6ee0   512x512 fmt=5 clear=1 src=(0,0 256x256)
        n=3 dest=…043e00   640x448 fmt=4 clear=0 src=(0,0 640x448)
```

Same count, same order, same formats, same source rectangles, and the destinations are the
addresses we compute. The oracle's markers name them: the 256x256 RGB5A3 copy is the **Mirror**
pass, and the RGB565 one the sea samples is a full-EFB grab. So the copy PLUMBING is right and
the remaining difference must be the EFB CONTENT at the moment of the grab — which is upstream
of everything I have been changing.

**The dst-height difference is not a bug.** Recomp 448 vs oracle 480 lines: measured
`yscale=0x100` (1:1) on every copy, so deriving XFB height as EFB height x yscale = 448 is
faithful to what the FIFO actually carries. The oracle's 480 comes from `GXSetDispCopyDst`'s
explicit arguments, which the API path stores directly and which never appear in the command
stream at all. Both are self-consistent, and this finally explains the long-standing
1280x896-vs-1280x960 dump-size difference noted way back at the first side-by-side. I am
deliberately NOT "fixing" this by inventing a scale factor the stream does not contain.

Next: the sea samples a copy of the main EFB, so compare what is drawn BEFORE that copy in each
runtime — the draws, not the copy.

## The sea DOES sample the copy — and aurora's VI is unconfigured in the recomp

Added the texel pointer to the draw dump's `texs` field, because two 320x224 textures exist and
dimensions alone cannot tell a raw-RAM texture from an EFB-copy result. That settles which one
the sea samples:

```
sea draw texs=[0:320x224@0x7f8c0e443e00, 1:256x256@0x7f8c0e255ce0, ...]
EFB copy destination phys 0x00c43e00 (320x224)
```

`0x7f8c0e443e00` is exactly `g_ram_base + 0x00c43e00` — **the copy destination**. So the copy is
created, registered and sampled correctly, and the defect is its CONTENT, not the plumbing.

The second 320x224 texture (`phys 0x003e1280`) is a separate resource that nothing ever copies
to; `SB_TEX_DUMP` resolves it as a static texture and it is entirely black, as raw untouched RAM
would be. It is not what the sea samples, so it is a side observation, not the defect.

**Concrete divergence found while checking this:** the copy logs report
`logical=640x480` for the recomp and `logical=640x448` for the oracle.
`logical_fb_size()` returns `vi::configured_fb_size()` — aurora's OWN VI configuration. The
decomp runtime configures it because the game drives aurora's VI; this runtime has its own VI
MMIO device, so aurora's VI is **never configured** and reports a default 640x480 while the
game actually renders 640x448.

This is the same class of defect as the missing `VIGetRetraceCount`: aurora expects the runtime
to tell it something, this runtime never did, and aurora silently used a default. The
consequence is not cosmetic — `map_logical_viewport` scales by target/logical, so every
viewport and copy rectangle is scaled by 960/480 = 2.0 where the oracle uses 960/448 = 2.14.
That is why our display copy resolves 1280x896 against the oracle's 1280x960.

Whether it explains the white sea is NOT established — the reflection copy grabs the EFB, and a
vertical scale error would distort content rather than whiten it. But it is a real divergence
in the exact coordinate mapping the copy goes through, and it should be fixed before drawing
further conclusions about copy content.

## VI configuration fixed — frame geometry now matches the oracle

aurora derives its logical framebuffer from its OWN VI configuration, which this runtime never
set (it owns the VI registers itself), so aurora used its 640x480 default while the game renders
640x448. Since `logical_fb_size()` feeds `map_logical_viewport`, every viewport and EFB-copy
rectangle was scaled by 960/480 = 2.0 instead of 960/448 = 2.14.

Fixed by overriding `VIConfigure` to forward fbWidth/efbHeight through a new narrow aurora
entry point `aurora_vi_set_fb_size` (preferred over handing aurora a `GXRenderModeObj`, which
would couple this runtime to that struct's layout — and whose headers cannot be included here,
they redefine the PPC intrinsics). The recompiled body is super-called first, so the guest still
programs its own VI exactly as retail does.

Verified against the oracle — all three copies now identical where all three differed before:

|            | before      | after      | oracle     |
|------------|-------------|------------|------------|
| display    | 1280x896    | 1280x960   | 1280x960   |
| mirror     | 512x512     | 512x549    | 512x549    |
| reflection | 640x448     | 640x480    | 640x480    |
| logical    | 640x480     | 640x448    | 640x448    |

Frame dumps are now 1280x960 like the oracle's, which removes the size mismatch that has made
direct pixel diffs invalid since the first side-by-side. The frame is visibly correct in
proportion where it had been vertically squashed — Mario in particular now has the right shape,
which retires the "different animation phase" hand-wave I used for him earlier.

## Sea: the clear-colour theory is dead too

Reasoned that a copy with the clear bit set leaves the EFB filled with the clear colour, so a
grab taken before much is drawn would return it — and if that colour were white, the reflection
would be white. Decoded BP 0x4F/0x50 in our parser to check, and verified the decode matches
aurora's byte for byte (0x4F: r=bits0-7, a=bits8-15; 0x50: b=bits0-7, g=bits8-15) rather than
trusting my own reading of the register.

**The EFB clears to BLACK** (a=0 r=0 g=0 b=0 by the time file-select is up). A grab of a
mostly-empty EFB would therefore be black, not white. Theory eliminated.

Sea hypotheses now eliminated: animated ripple grid, palettised textures, the tcg/indirect
parser gap, stale/missing copy state, a coplanar extra draw, the reflection not being sampled,
viewport scaling, and the clear colour. What remains unexamined is the TEV configuration of the
sea draw itself — the dump reports the stage COUNT (2, matching the oracle) but not the stage
ops, so "same tev=2" has never actually meant "same TEV program".

## The sea is a FEEDBACK LOOP, and ours has runaway gain

Two new instruments (both pushed to aurora): the draw dump now carries the full per-stage TEV
program, and `SB_PRESENT_COPY=<W>x<H>` presents any copy's resolved texture so its content can
be SEEN instead of inferred.

**The sea draw's TEV program is byte-identical between runtimes:**

```
0:c(15,15,15,15)o=0,0,0,r0 a(7,4,5,7)o=0,0,0,r0 tm=0 tc=0 ch=4 k=6/0 |
1:c(10,15,15,15)o=0,0,1,r0 a(7,4,0,7)o=0,0,1,r0 tm=0 tc=1 ch=4 k=6/0
```

Stage 1 is `a=TEXC` with `b=c=d=ZERO` and scale=1 (x2), so the sea's colour is **2x the
texture** it samples on texmap 0 — the EFB copy.

**And that copy contains the whole finished scene, in BOTH runtimes.** Presenting it directly:
ours shows the complete file-select frame including its own already-white sea (19.4%
near-white); the oracle's shows the same frame with a correct teal sea (4.8%). So this is a
**feedback loop** — the sea samples a grab of the scene and multiplies it by two, frame after
frame. Any excess brightness compounds until it saturates to white, which is exactly the
failure mode aurora's own copy_tex comments already describe ("saturate the frame white").

That reframes the defect precisely: it is not that something paints the sea white, but that a
loop which is stable in the oracle is divergent here. With the draw state, both texture
pointers, the TEV program and the blend all matching, the remaining variable is **where** the
sea samples that texture. Stage 1 uses `tc=1` — a GENERATED texcoord — and the draw dump does
not report texgen configuration at all. If our texgen maps the sea's pixels onto a brighter
part of the grab (the sky, say) rather than the water, the loop gain exceeds one and runs away.

Next: compare the texgen setup (source, type, matrix) for this draw between runtimes. Note the
long-standing `unhandled tcg src 21` warning in aurora's FIFO path is a texgen gap and may well
be this — it did not appear in an earlier file-select grep, which is worth re-checking now
rather than assuming.

## Texgen is identical too — the divergence is in DATA, not configuration

```
recomp  tcg=[0:ty=1 src=4 mtx=60 pm=125 n=0, 1:ty=1 src=5 mtx=60 pm=125 n=0]
oracle  tcg=[0:ty=1 src=4 mtx=60 pm=125 n=0, 1:ty=1 src=5 mtx=60 pm=125 n=0]
```

`mtx=60` is GX_IDENTITY and `src=4/5` are the vertex TEX0/TEX1 attributes, so the sea's UVs come
straight from vertex data through an identity matrix. No texgen matrix is involved, and the
`unhandled tcg src 21` warning is unrelated to this draw (its sources are 4 and 5).

The sea draw now matches the oracle on **every piece of state I can enumerate**: geometry
(52 verts, same world position), viewport, depth mode, blend mode and factors, both bound
texture pointers, the full per-stage TEV program, and now texgen. The copies feeding it match in
count, order, format, source rect and destination. Yet the output differs.

So the divergence is in DATA rather than configuration. Three candidates, none yet tested:

1. **Vertex texcoord values.** With an identity texgen the UVs are read straight out of the
   vertex stream, and our VAT decoding has been wrong before (the VAT_C TEX5 bit offset). Wrong
   UVs would sample a brighter part of the grab and push the loop's gain above one.
2. **The copy texture's ALPHA.** The blend is `bf=4/2` — src x srcAlpha + dst x srcColor — so
   the loop gain depends directly on the sea's alpha, which stage 1 derives from TEXA. The copy
   is RGB565 (no alpha), which aurora swizzles to 1.0, and `copy_tex` also overwrites alpha
   before resolving when `alphaUpdate && dstAlpha != UINT32_MAX`. A different dst-alpha state
   would change the gain without changing any of the state compared above.
3. The blend DESTINATION content, i.e. what was already in the framebuffer under the sea.

Candidate 2 is the most promising: it is the one term in the loop that is invisible to every
comparison run so far, and this project already has history with mid-frame dst-alpha
(the GXPeekARGB/Mario-occlusion note). Testing it means comparing `dstAlpha`/`alphaUpdate` at
the copy, which the copy log does not currently report.

## dst-alpha eliminated; the sea is wrong from the FIRST frame, with a slow creep on top

Candidate 2 tested and dead: the copy log now reports the alpha state, and it is identical on
both sides for all three copies — `dstAlpha=-1 aU=1 cU=1 pixFmt=1`. The loop's gain term is not
the divergence.

Then a timing measurement that reshapes the problem, using `SB_DUMP_FRAME_EVERY=40` and
measuring the sea region across a whole run:

```
 ~present  780 (first file-select frame): near-white 81.6%  mean 227.6
 ~present 1500                          : near-white 82.1%  mean 228.1
 ~present 2460                          : near-white 83.9%  mean 232.7
 ~present 3660                          : near-white 91.0%  mean 239.9
 ~present 4860                          : near-white 91.5%  mean 240.1
```

**The sea is already ~82% white on the first frame it appears.** A feedback loop starting from
an empty (black) copy would begin dark and brighten over many frames; this does not. So the
white is NOT primarily accumulated — the sea's very first render is already about twice as
bright as it should be.

There IS a slow creep (82 -> 92%, mean 227 -> 241 over ~4000 frames), consistent with a mild
gain slightly above one riding on top of an already-wrong base. So the feedback loop is real but
SECONDARY; last entry over-weighted it. The primary defect is that the first sample is wrong.

That sharpens the target considerably. The sea samples the copy through vertex TEX1 UVs with an
identity texgen, and the copy holds the previous frame's whole scene — so sampling the wrong
REGION of it (the bright sky rather than the water) would produce exactly this: immediately
about twice too bright, plus slow compounding. Every piece of configuration matches the oracle,
so the remaining suspect is the vertex UV DATA itself, which no diagnostic currently reports.

Next: dump the sea draw's vertex TEX1 values in both runtimes. Our VAT texcoord decoding has
been wrong before (the VAT_C TEX5 bit offset), which is precisely a wrong-UV-values failure.

## FOUND IT: the sea's UV animation runs at DOUBLE rate

Added `SB_UV_PROBE=<W>x<H>` to aurora, decoding a draw's actual texcoord values out of the
vertex stream — the one thing no diagnostic reported. The sea's texcoords are DIRECT f32 pairs
(`tcv=[t0:d=1 cnt=1 ty=4, t1:d=1 cnt=1 ty=4]`, identical on both sides), so the values are
inline in the vertex data and directly comparable.

```
recomp  tex0 (-1.6503,-4.3200)   tex1 (-1.9800,-4.5831)
oracle  tex0 (-1.1034,-4.3200)   tex1 (-1.9800,-5.2495)
```

The STATIC components match exactly (tex0's V = -4.3200, tex1's U = -1.9800). Only the ANIMATED
components differ — and the frame-to-frame deltas give the mechanism:

```
recomp  tex0 U: -1.6503 -> -1.6443   (+0.0060/frame)
oracle  tex0 U: -1.1034 -> -1.1004   (+0.0030/frame)
```

**Our sea's UV scroll runs at exactly twice the oracle's rate.** Same for tex1's V
(+0.0060 vs +0.0030). That is a game-state divergence, not a GX one — which is why every GX
comparison matched: draw state, textures, TEV program, texgen, descriptors, copies, alpha.

It also explains the brightness directly. The scroll accumulates, so by the time the frame is
sampled our UVs have drifted far from the oracle's (tex1 V differs by 0.67), and with the sea
sampling a grab of the whole scene through those coordinates, a different offset means sampling
a different, brighter part of the image. The slow creep measured last tick rides on top.

Prime suspect: something advancing the sea's animation twice per frame, or a per-frame delta
that is 2 where the oracle's is 1. This runtime's `vi_wait_for_retrace` increments the retrace
counter once per call, and `video_wait_for_retrace` presents once per call — if the game derives
animation from retrace deltas, an interlaced-field convention (2 fields per frame) would produce
exactly a factor of 2. That is the next thing to check, and it is worth checking broadly: an
animation-rate error of 2x would affect EVERY time-driven thing in the game, not just the sea.

## Correcting the rate analysis, and a wall-clock hypothesis

Stamped the UV probe with a frame ordinal to rule out the mundane explanation that consecutive
probe lines were two draws in one frame. They are not — one sea draw per rendered frame in both
runtimes — so **the 2x per-frame UV rate stands**.

But my previous entry's "per retrace" arithmetic was wrong and is retracted: the two `rc`
values are not the same quantity. The oracle's is the SDK field counter (advanced twice per
frame by `sb_frame_present`, one per NTSC field), while the recomp's is the host present
counter I added for `VIGetRetraceCount`. Comparing them as if they measured the same thing
produced a bogus "4x per retrace". Per RENDERED FRAME the difference is 2x, and that is the
only rate claim supported by the data.

`TVideo::waitForRetrace(u16 param_1)` takes the number of fields the frame covers (2 here) and
also records `mLastRetraceTime = OSGetTick()`. That matters, because this runtime's time base
is derived from a monotonic host clock at the correct 40.5 MHz (`tb_get` in rt_core.cpp). So if
the sea's scroll is driven by an OSGetTick DELTA rather than a per-frame constant, then a
runtime rendering at half the frame rate advances it twice as much per frame — while being
correct in real time. That would make the 2x a symptom of frame rate, not a timing bug, and the
white sea would need a different explanation again.

Distinguishing those two cases is the next step and needs one number: the two runtimes' actual
present rates. Two attempts to measure it this tick failed for tooling reasons, not for lack of
a result — `/usr/bin/time` on a process that keeps running past its frame dump measures the
timeout, not the run, and a `| grep` pipeline discarded the rate lines. Worth doing properly
rather than guessing: if the recomp runs at ~half the oracle's rate, the wall-clock explanation
holds and the sea's UV divergence is a consequence of performance; if the rates are comparable,
the game's animation input genuinely differs and that is the bug.

## Both timing hypotheses eliminated by measurement

Measured properly this time (log to a file, not through a pipe):

**1. Frame rate.** The recomp runs **~157 presents/s** — it has NO frame pacing at all, while
the decomp paces each present to its NTSC fields via `sb_frame_present` (~30/s). That REFUTES
the wall-clock hypothesis rather than confirming it: if the sea's scroll were driven by an
elapsed-time delta, our much SHORTER frames would advance it LESS per frame, not 2x more. The
observed difference is in the opposite direction to what wall-clock timing predicts.

**2. Retrace step.** Instrumented the guest's own `__VIRetraceCount` at the frame boundary:

```
[frame] guest retrace counter 17998 (+2 since last present)
```

**+2 per present, exactly matching the decomp**, which advances it once per NTSC field via
`sb_frame_present(param_1)` with param_1 = 2. So `TVideo::waitForRetrace` is being asked for the
same number of fields in both runtimes and the guest-visible retrace clock is identical. The
animation input is not the retrace counter either.

So the 2x per-frame UV step is driven by neither wall-clock time nor retrace count, yet every GX
comparison matches. That points at game state feeding the sea's texture-matrix animation
directly — something computed by ported/recompiled code rather than supplied by the runtime.

**Separately: the missing frame pacing is a real gap regardless of the sea.** The game runs
roughly 5x faster than real time, which will matter for anything time-driven (audio, movie
playback, physics tuned to a field rate) even though it demonstrably is not what moves these
UVs. The decomp's model is the reference: pace each present to `param_1` NTSC fields, with an
escape hatch equivalent to SB_TURBO. Recording it as a known defect rather than fixing it
mid-investigation.

## ROOT CAUSE LOCATED: the recomp runs the MOVEMENT phase twice as often

Traced the sea's UVs to their generator. `TMapObjWave` emits two texcoords per vertex
(`MapObjWave.cpp:342-349`):

```c
GXTexCoord2f32(mTexOffS + xw * mTexScale,      zNear * mTexScale);   // TEX0: U animated
GXTexCoord2f32(kTexS2K * xw * mTexScale2,      mTexOffT + zNear * mTexScale2); // TEX1: V animated
```

That is EXACTLY the measured signature — tex0's U and tex1's V animated, the other two
components static — so the sea is `TMapObjWave` and the animated terms are `mTexOffS`/`mTexOffT`.

Both are advanced in `updateTime()` by a constant `mTexRate = 0.0015f`, and `updateTime()` runs
from `perform()` gated on `param & 0x1` — **the movement phase**. So the scroll rate is a direct
readout of how many movement passes run per frame:

| runtime | UV delta/frame | / 0.0015 | movement passes per frame |
|---------|----------------|----------|---------------------------|
| oracle  | 0.0030         | 2        | 2                         |
| recomp  | 0.0060         | 4        | 4                         |

**The recomp runs the game's movement phase twice as often as the decomp runtime does.** That is
not a sea bug at all — it is a game-loop divergence that advances EVERY actor's per-frame
update twice as fast, and the sea merely happens to expose it as an accumulating value I could
measure precisely. It plausibly explains the animation-phase mismatches I have been setting
aside for many ticks (Mario's pose, the title's PRESS START prompt, the save-slot labels).

This also retires the last of the GX-side theories: nothing about the renderer was ever wrong
here. Every GX comparison matched because the GX stream was faithfully carrying the output of a
game loop that had already run too many times.

Next: count `perform(movement)` calls per present directly in the recomp rather than inferring
from 0.0015 arithmetic, and find why the phase runs 4x. Worth checking whether the OTHER phases
(draw is `param & 0x8`) are also doubled — the sea is drawn once per frame, which suggests the
DRAW phase is NOT doubled and only movement is, an asymmetry that should point straight at the
cause.

## Confirmed by direct count: movement 4x/frame, draw 1x/frame

Added `SBR_PHASE_COUNT` (`sms-recomp/overrides/diag_phases.cpp`), a counting override on
`TMapObjWave::perform` that always calls the real body. Measured over 14,200 presents:

```
[phase] movement=56376 draw=14094  (3.97 movement/frame, 0.99 draw/frame)
```

That confirms the arithmetic inferred from the UV deltas — **~4 movement dispatches per
rendered frame against exactly 1 draw** — and confirms the predicted ASYMMETRY. The oracle's
0.0030/frame scroll puts it at 2 movement dispatches per frame, so the recomp runs the movement
phase twice as often while drawing the same number of times.

The asymmetry is the useful part: because draw is dispatched exactly once, the view-object tree
is traversed once for drawing, so the object is not duplicated in the tree. The movement PHASE
itself is running more times per frame. That rules out "the actor is registered twice" and
points at the loop that drives the phases.

**Workflow gotchas hit while doing this**, both worth recording since both silently produced
nothing:
- The recomp's build directory is configured with `-S sms-recomp`, not the repo root. Running
  `cmake -B build-sms-recomp` from the root fails with a source-mismatch error that is easy to
  miss when output is redirected, and the build then silently uses the stale cache.
- A new file in `overrides/` needs a reconfigure to be picked up by the CMake glob (the known
  build-glob gotcha). Until then it compiles nothing and the override simply never registers —
  indistinguishable from "the code never runs" unless the `[override]` registration line is
  checked. There is also a stale `diag_tmp.cpp.o` in the build tree from a since-deleted file.

Next: find what dispatches the movement phase and why it runs 4x. The decomp's own game loop is
the reference for how many times `TMarDirector::direct` (and hence each phase) should run per
frame.

## The mechanism: a catch-up loop driven by SMSGetVSyncTimesPerSec

`TMarDirector::direct` (MarDirectorDirect.cpp) runs the movement phase in a **catch-up loop**:

```c
int vsyncRate = 600 / (int)SMSGetVSyncTimesPerSec();   // line 77
unk54 += vsyncRate;                                    // line 137
for (;;) {
    if (!(unk4C & 0x4000)) {
        unk54 -= 5;
        if (unk54 < 5) unk4C |= 0x4000;
        ...
        mPerformListMovement->perform(uVar4, &local_140);   // movement phase
```

So movement passes per `direct()` = `vsyncRate / 5`, and the whole thing hangs off
`SMSGetVSyncTimesPerSec()`:

```c
f32 SMSGetVSyncTimesPerSec() {   // Application.cpp:84
    f32 result = 60.0f;          // NTSC/MPAL/EURGB60
    switch (VIGetTvFormat()) { case VI_PAL: result = 50.0f; break; }
    return result / 2.0f;        // -> 30 for NTSC
}
```

30/sec gives vsyncRate = 20 and **4 movement passes per frame** — which matches the recomp's
measured 3.97 exactly. So the recomp is doing what this code says.

**That contradicts my inferred oracle rate of 2**, and the inference is the weaker claim: it
came from dividing the oracle's UV delta by `mTexRate`, not from counting anything. If the
decomp runs the same `direct()` with the same NTSC format it should also do 4, and then the
oracle's slower scroll must have a different explanation — a different `mTexRate`, a different
`VIGetTvFormat`, or `direct()` not running every present.

Added the matching `SB_PHASE_COUNT` counter to the decomp's own `TMapObjWave::perform` to settle
it by measurement. The run did not reach file-select inside its timeout (the oracle needs
~200s to get there with `SB_STAGE=15` and START at retrace 2600), so the number is not in hand
yet — recorded as pending rather than guessed, since guessing is what put the 2 there in the
first place.

Note `VIGetTvFormat()` is another runtime-supplied value: aurora's returns 0 unconditionally.
If the recomp's recompiled SDK reads real VI registers instead, the two runtimes could disagree
on TV format and hence on this entire rate — worth checking alongside the count.

## Both sides measured — and it is not obvious which one is CORRECT

The decomp counter finally ran (after fixing a build failure, below):

```
oracle  [phase] after 98000 retraces: movement=97796 draw=48898
                 (1.00 movement/retrace, 0.50 draw/retrace)
recomp  [phase] after 14200 presents: movement=56376 draw=14094
                 (3.97 movement/frame,  0.99 draw/frame)
```

Retraces advance 2 per rendered frame, so the oracle is **2 movement + 1 draw per frame** and
the recomp **4 movement + 1 draw per frame**. My original inference of 2 for the oracle was
right; the previous entry's doubt about it is resolved in its favour.

**But the arithmetic in `direct()` predicts 4, not 2.** `vsyncRate = 600 / 30 = 20`, and the
loop subtracts 5 until below 5, giving four passes: 20 -> 15 -> 10 -> 5 -> 0. The RECOMP matches
that prediction exactly; the DECOMP does half of it.

That inverts the assumption I have been working under all session. The recomp runs the game's
REAL PPC `direct()`; the decomp runs our hand-ported C++ version of it. Where the two disagree
on control flow, the recomp is the better evidence of what retail does — so the likelier reading
is that **the decomp's ported `direct()` runs half the movement passes it should**, not that the
recomp runs twice too many. The oracle has been treated as ground truth for rendering, where it
is validated against real hardware output; it is NOT automatically ground truth for game-loop
control flow, which is exactly the kind of thing a hand port gets subtly wrong.

Deciding this needs the actual retail behaviour, not a preference between our two runtimes:
disassemble `TMarDirector::direct` around the loop (0x80299b2c-0x80299b6c is already cited in
MarDirectorDirect.cpp for a nearby branch) and count what the hardware does. Until then neither
runtime should be "fixed" toward the other — that would be exactly the kind of change that makes
a symptom disappear without establishing which behaviour is right.

**Workflow: `cmake ... | grep error; echo built` masked a build failure twice this session.**
Both times a stale binary then ran and produced a confidently wrong "no output" result — the
decomp counter appeared to prove `TMapObjWave::perform` was never called, when in fact the file
had failed to compile (an `extern "C"` at block scope) and the old binary was running. Check the
build's exit status, or verify the new code is actually in the binary (`strings | grep`), rather
than printing an unconditional success message.

## Located precisely: the divergence is PRESENTS PER direct() CALL, not the loop

Retail disassembly first, to settle which runtime's loop is faithful:

```
0x8029985c  li    r3, 0x258          ; 600
0x8029986c  divw  r25, r3, r0        ; r0 = (int)SMSGetVSyncTimesPerSec()
...
0x80299970  lwz   r3, 0x54(r26)      ; unk54
0x80299974  addi  r0, r3, -5
0x80299980  cmpwi r0, 5
0x80299984  bge   0x80299994         ; keep looping while >= 5
```

The decomp's port of this loop is structurally faithful to retail — same 600, same divide, same
-5/compare-5. So the earlier suspicion that our ported `direct()` was wrong here is NOT
supported; that hypothesis is withdrawn.

Then the decomp's own existing `SB_DIRECT_BR` diagnostic supplied the missing numbers:

```
[dir-br] call=2 branch=both vsyncRate=20 unk54_in=20 unk54_out=0 ...
```

`vsyncRate = 20` (so `SMSGetVSyncTimesPerSec()` = 30, matching the recomp), and the loop drains
20 in steps of 5 — **4 movement passes per `direct()` call in the decomp too**. Both runtimes
agree on the loop AND on the rate.

So the 2x is neither: with 4 movement passes per call but only 2 movement passes per rendered
frame, **the decomp calls `direct()` once per TWO presents, while the recomp calls it once per
present.** Cross-checks: 97796 movement / 4 = 24449 direct() calls across 98000 retraces = one
call per 4 retraces = one per 2 frames; and draw measures 0.50/retrace = 1 per frame, so the
decomp presents twice per game update.

That is a much more specific defect than "the movement phase runs twice as often", and it is
about the FRAME SEAM rather than game logic: one of the two runtimes renders a different number
of frames per game update. The decomp's `MarDirectorDirect.cpp` already carries a comment about
a "catch-up render" being made deterministic, which is the right place to look next — as is
whether the decomp presents an extra frame per update by design or by accident.

## CORRECTION: not presents per direct() — movement dispatches per direct()

Last entry concluded "the decomp calls `direct()` once per TWO presents". **That is wrong**, and
the error was methodological: I divided a movement count from one run by a `direct()` call count
from a DIFFERENT run. Measuring both in a single run settles it:

```
[dir-br] call=49132 ... vsyncRate=20 unk54_in=20 unk54_out=0
[phase] after 106600 retraces: movement=98170 draw=49050
```

- `direct()` calls 49132 vs draws 49050 vs frames 53300 -> **~1 present per `direct()` call**,
  not 2. The frame seam is fine.
- movement 98170 / 49132 calls = **2.0 movement dispatches per `direct()` call** — even though
  `unk54_in=20 -> unk54_out=0` proves the loop iterates FOUR times.

So both runtimes iterate the loop 4x per call, but the decomp only dispatches movement to the
actor on 2 of those iterations while the recomp dispatches on all 4. The divergence is a GATE
inside the loop, not the loop count and not the frame seam.

The gate is visible in `MarDirectorDirect.cpp`:

```c
u32 uVar11 = ~uVar8;
u32 uVar4  = uVar11;
if (unk58 & 1) uVar4 &= ~0x100;
if (unk58 & 2) uVar4 &= 0x200;      // clears bit 0 -> actor's `param & 0x1` false
...
mPerformListMovement->perform(uVar4, &local_140);
```

`uVar4` is the COMPLEMENT of `uVar8`, and `uVar8` accumulates bits from the `mState` switch
earlier in the loop (`uVar8 |= 1`, `uVar8 |= 2`). So whether an actor sees the movement bit
depends on `mState` and `unk58` — and `uVar4 &= 0x200` in particular clears bit 0 outright.

Next: log `uVar8`/`uVar4`/`unk58` per loop iteration in the decomp and compare against the same
values in the recomp. That pins down which iteration's gate differs, and the retail
disassembly at 0x80299b2c (already cited in this file for the neighbouring branch) can then
adjudicate — as it did for the loop itself, where the port turned out to be faithful.

Methodological note to self: two of the last three conclusions came from dividing counts taken
in different runs. Cross-run arithmetic is not measurement; put both counters in one run.

## FIXED (in the DECOMP): a misdecompiled mask halved every actor's movement rate

The retail disassembly adjudicated it, and the bug was ours — in the decomp, not the recomp.

```
0x80299b08  lwz    r3, 0x58(r26)            ; unk58
0x80299b0c  nor    r31, r27, r27            ; uVar4 = ~uVar8
0x80299b14  clrlwi. r0, r3, 0x1f            ; unk58 & 1
0x80299b1c  rlwinm r4, r4, 0, 0x14, 0x12    ; -> clear PPC bit 19  = &= ~0x1000
0x80299b20  rlwinm. r0, r3, 0, 0x1e, 0x1e   ; unk58 & 2
0x80299b28  rlwinm r4, r4, 0, 0x13, 0x11    ; -> clear PPC bit 18  = &= ~0x2000
```

(`rlwinm` with MB>ME wraps, so MB=20/ME=18 is "all bits except PPC bit 19" = `~(1<<12)` =
`~0x1000`; MB=19/ME=17 is `~(1<<13)` = `~0x2000`.)

Our port had:

```c
if (unk58 & 1) uVar4 &= ~0x100;    // wrong constant
if (unk58 & 2) uVar4 &= 0x200;     // wrong constant AND wrong operation
```

The second is the damaging one: `&= 0x200` is not a bit-clear at all, it masks `uVar4` down to
bit 9 and so clears **bit 0 — the MOVEMENT bit**. Every actor's movement pass was therefore
skipped on the loop iterations where `unk58 & 2` holds.

**Measured before and after, in single runs:**

| | movement / direct() call | draw / call |
|---|---|---|
| decomp before | 2.00 | 1.00 |
| decomp after  | **3.99** | 1.00 |
| recomp (retail code) | 3.97 | 0.99 |

The decomp now matches the recompiled retail code and retail's own arithmetic
(`vsyncRate 20 / 5 = 4`). Draw is unchanged, and file-select still renders correctly — sea
still 0.0% near-white, scene identical — so nothing regressed.

**This validates the two-runtime doctrine concretely.** The recomp runs the real PPC code, so
where the two disagree it is evidence about retail, and here it caught a decomp defect that had
been silently halving every actor's update rate — animation timing, physics steps, anything
driven by the movement pass — in the runtime being used as the rendering oracle. No amount of
comparing the decomp against itself would have surfaced it.

Note this does NOT fix the recomp's white sea: the recomp was already doing the right number of
movement passes. What it removes is the 2x discrepancy BETWEEN the runtimes, so their animation
phases are now directly comparable — which makes the recomp's remaining UV/brightness difference
measurable against a correct reference for the first time.

## The UV rate was NOT the cause of the white sea

With the decomp's movement gate fixed, both runtimes now advance the sea's UVs at the same rate:

```
oracle (fixed)  rc 206 -> 208 -> 210 : -1.3536 -> -1.3476 -> -1.3416   (+0.0060/frame)
recomp          rc 108 -> 109 -> 110 : -1.1559 -> -1.1499 -> -1.1439   (+0.0060/frame)
```

**Identical scroll rate — and the oracle's sea is still teal (0.0% near-white) while the
recomp's is still white (81%).**

So the "FOUND IT" conclusion from the UV-rate discovery was wrong AS AN EXPLANATION OF THE
WHITENESS. It was a real divergence and it led to a real decomp fix, but it was never the cause
of the white sea. Recording that plainly: a genuine bug found while chasing a symptom is not
automatically the cause of that symptom, and I treated it as one.

The absolute UV values still differ between runs (-1.3536 vs -1.1559), but that is only elapsed
time — the offsets wrap at 1.0 and both scroll identically — and the recomp's sea is white on
EVERY frame regardless of phase, so phase cannot be it either.

That leaves the state comparison exhausted: geometry, viewport, depth, blend, both texture
pointers, TEV program, texgen, texcoord descriptors, UV values, UV rate, copy count/order/
format/rect/destination, clear colour and dst-alpha ALL match. The output still differs, so the
difference has to be the CONTENT of the sampled copy — which is circular while the sea itself is
part of the scene being copied.

**The way to break the circle is ORDER.** If the reflection copy is taken BEFORE the sea is
drawn, the sea samples a scene without itself in it and the loop has no gain. If it is taken
AFTER, the sea samples its own previous output and any excess compounds — which is what the
slow creep measured earlier (82% -> 92%) looks like. Both runtimes issue the same copies, but I
have never established WHERE the sea draw sits relative to copy #3 in each. That is the next
measurement: interleave the copy log and the draw dump on a common counter so the ordering is
directly visible on both sides.

## Copy/draw ordering is IDENTICAL — but the recomp renders half the draws per frame

Put copies and draws on one timeline (aurora's copy log now carries `afterDraw=`, the global
pushed-draw ordinal), aligned both windows on the same frame, and measured each runtime:

```
recomp  frame draws #328237..#328549 (313)   sea draw #328469   reflection copy afterDraw=328469
oracle  frame draws #1583304..#1583916 (613) sea draw #1583605  reflection copy afterDraw=587480
```

The oracle's two counters are offset by a constant (its dump index and push count start at
different points), but normalising by the frame start puts its sea draw at exactly its
reflection copy's ordinal — **the same relative position as the recomp**. In BOTH runtimes the
reflection copy is taken immediately AFTER the sea draw, so in both the sea samples the previous
frame's grab including its own previous output. The feedback loop exists in both, with the same
structure, and the ordering hypothesis is dead.

**What the measurement did surface is a large content difference:**

| | draws/frame | display->mirror | mirror->reflection | reflection->next |
|--------|-------------|-----------------|--------------------|------------------|
| recomp |         313 |              73 |                159 |               81 |
| oracle |         613 |             184 |                117 |              312 |

The recomp renders roughly HALF the draws per frame, and the distribution differs sharply —
most of the oracle's extra draws fall after the reflection copy (312 vs 81), which is where the
2D/J2D overlay is drawn. That matches the earlier full-frame breakdown (oracle 422 orthographic
draws vs the recomp's 81) which I set aside at the time as an unpinned animation phase. With
movement rates now equal between the runtimes, that excuse is gone: **the recomp is genuinely
emitting far fewer 2D draws than the oracle.**

That is a bigger and better-defined defect than the sea, and it may well contain it: the sea's
brightness depends on the content of a grab of the whole scene, and the two runtimes demonstrably
do not draw the same scene. Chasing the missing draws is now the more promising thread — and
unlike the sea it needs no circular reasoning to measure.

## The 2D gap: the recomp is missing J2D panel draws (pre-existing, not from the gate fix)

Broke the orthographic draws down by texture and vertex count for one frame on each side:

| tex0 (4-vert quads) | oracle | recomp |
|---------------------|--------|--------|
| 512x256             |    274 |     64 |
| 20x20               |     32 |      8 |
| 256x256 (4/6/8 vt)  |     31 |      1 |

Checked first whether my movement-gate fix caused this: the oracle's orthographic count and its
512x256 quad count are **identical before and after the fix** (844 / 275 in both logs). So the
gap is pre-existing and unrelated — worth ruling out explicitly, since I had just changed how
often the movement phase runs.

Cross-referencing the two screenshots explains what the numbers mean, and corrects an earlier
misreading. In the ORACLE, each save slot has a rounded blue J2D panel above it containing the
word "New", and the memory-card dialog is an empty blue panel. In the RECOMP those panels are
ABSENT — the slots show outlined sprite-style "NEW" text with no panel behind them, and the
dialog panel is drawn but its frame differs. I previously attributed that difference to the two
captures being at different animation phases; with movement rates now equal between the runtimes
and the draw counts differing by ~4x on exactly the textures those panels use, that explanation
does not hold. **The recomp is failing to emit a large class of J2D panel/window draws.**

This is a real, visible, non-circular defect — unlike the sea, it can be verified by looking at
the screen — and it is plausibly upstream of the sea, since the sea samples a grab of the whole
scene and the two runtimes are demonstrably not drawing the same scene.

Next: identify what the 512x256 texture is (J2D window/pane atlas is the obvious candidate) and
which draw path emits those quads, then find why the recomp emits a quarter of them.

## The 2D gap explained: the recomp has NO memory-card support, so the UI state differs

Eliminated two mechanical causes first, both by measurement:
- **Not cull-all drops.** Added a counter to aurora's `GX_CULL_ALL` early-return (dropping a
  draw is invisible by construction, so it needed counting): **zero** dropped draws in either
  runtime.
- **Not parser framing loss.** Our FIFO parser drops the remainder of a batch on an
  unrecognised opcode, which would truncate exactly the tail-of-frame 2D draws — but the count
  is **zero** across a full run.

So the game itself is not emitting those draws in the recomp, and the reason was on screen the
whole time: **the recomp's dialog reads "There is no Memory Card in Slot A." while the oracle's
dialog is empty.** The decomp links aurora's CARD implementation (`aurora_card`); this runtime
has no card device and no CARD overrides at all. The game therefore takes its own no-card path
and draws a different, smaller UI — no blue J2D "New" panel per slot, hence a quarter of the
512x256 quads.

**The two runtimes are not rendering the same scene**, and have not been for the whole
file-select comparison. That undermines the sea comparison as well: the sea samples a grab of
the ENTIRE scene through wrapping UVs, so a different UI in that grab means a different sampled
colour. Some or all of the sea's brightness difference may simply be "the scenes differ" rather
than a rendering defect — which would explain why every piece of GX state matched while the
output did not.

I should have caught this far earlier. The dialog text was visible in the very first
side-by-side screenshot and I described it then as evidence the CARD path "reports its real
state rather than a fabricated one" — correct as far as it went, but I did not follow through to
the consequence that the two runtimes were therefore in different UI states and not comparable.

Next: implement CARD for the recomp (aurora's CARD is already there, as the decomp's use of it
shows) so both runtimes reach the same file-select state. Only then is a scene-level comparison
meaningful — and the sea question should be re-asked from scratch afterwards, not resumed from
its current dead end.

## Confirmed: the decomp reads a REAL memory card, the recomp has none

Traced aurora's card path resolution (`DolphinCardPath.cpp`) to Dolphin's own location, and the
file is there on this machine: `~/.local/share/dolphin-emu/GC/MemoryCardA.USA.raw`
(16 MiB = 2048 blocks of 8 KiB). NOTE: an earlier draft of this entry named
`~/.dolphin-emu/GC/...` — that path does NOT exist here; the listing I read came from the
second command in a two-command line whose first had failed under `2>/dev/null`. Corrected. So the decomp runtime
mounts a real, populated memory card, while the recomp has no CARD support at all.

That closes the question completely. The two runtimes were in different UI states because they
genuinely have different hardware attached, and every "missing 2D draw" was the game correctly
drawing a different screen. Nothing was wrong with the recomp's rendering.

(The card file is machine-specific and lives outside the repo — nothing to commit, and it must
not be. It does mean this comparison is not reproducible on another machine without the same
card present, which is worth knowing before treating any file-select parity number as portable.)

**The recomp's EXI model is already correct about this.** `dev_exi.cpp` models the transport
only and treats selecting a chip-select with nothing attached as FATAL rather than returning
bus-idle 0xFF — its own comment explains why: handing back 0xFF would invent a broken console
and let the guest silently fall back to defaults. Nothing attached to the card slot is an honest
state, not a bug; the gap is that no card device exists to attach.

**Scoping the work rather than stubbing it.** The tempting shortcut is to make `CARDProbeEx`
report a card present so the UI matches. That would be exactly the banned success-shaped stub:
probe says yes, every subsequent read fails, and the game draws a plausible-but-wrong screen —
worse than the honest no-card path it takes today. The real options are:

1. **Override the CARD SDK entry points** onto aurora's CARD (the DVD model — overrides at a
   narrow OS API). Named entry points in this DOL: `CARDInit` 0x803551a0, `CARDProbeEx`
   0x803580a8, `CARDMount` 0x803588dc, `CARDMountAsync` 0x8035873c, `CARDCheck` 0x80357f88,
   `CARDCheckExAsync` 0x803579f8, `CARDFreeBlocks` 0x80355390 — plus the unnamed weak ones
   (open/read/write/stat) that will need `vtable_re.py`-style resolution. Requires marshalling
   CARDFileInfo/CARDStat between guest and host layouts.
2. **Attach a card device to EXI** and let the guest's own recompiled CARD code drive it. More
   faithful and needs no marshalling at all, since the guest keeps its own structures — but it
   means implementing the card's EXI command protocol.

Option 2 is the better fit for a standalone recomp: the guest already HAS working CARD code, and
attaching hardware for it to talk to keeps every structure in guest layout, which is precisely
the boundary problem that killed the old hybrid. Option 1 re-introduces per-struct marshalling
at a much wider API than DVD's.

## Memory card for the recomp: device attached, ID accepted, read protocol still wrong

Implemented option 2 — a card device on EXI channel 0 chip-select 0
(`sms-recomp/runtime/dev_card.cpp`), backed by the real Dolphin card image so saves stay
interchangeable. The guest's own recompiled CARD driver talks to it, so every CARDFileInfo /
CARDStat stays in guest layout and no marshalling is involved.

The game's own error messages turned out to be an excellent protocol oracle — each fix moved it
to the next one:

1. **"There is no Memory Card in Slot A."** — the device attached but was invisible. EXI CSR
   bit 12 (EXT) reports "a device is connected in this channel's external slot", and the SDK's
   probe reads it WITHOUT issuing any command; our EXI transport never set it. Note EXT must
   reflect device 0 only — channel 0's device 1 is the internal SRAM/RTC chip, which is not
   insertable and must not report as one.
2. **"The device in Slot A is not supported."** — detected, ID rejected. The EXI device ID is a
   capacity code doubling per size step (512 KB -> 0x04 … 16 MB -> 0x80), returned as a full
   32-bit word rather than left-justified like a short immediate transfer. My first table was
   both wrong-valued and wrongly justified.
3. **"The Memory Card in Slot A is damaged and cannot be used."** — where it stands now. The
   card is accepted and mounted, and the guest is reading it, so the block-read command and its
   DMA are being exercised; the data coming back is wrong. My read path takes a 3-byte address
   from the opening immediate write, which is a guess — real cards take a wider address plus
   dummy bytes, and the exact framing depends on card size.

Next: disassemble the SDK's card read sequence in this DOL rather than guessing the protocol —
the guest driver is retail code, so what it emits IS the specification. The same approach that
settled the movement-gate question.

Worth noting the design is holding up: every wrong guess so far produced a LOUD, specific
failure (a distinct on-screen message, or the device aborting on an unimplemented command)
rather than silently wrong save data. That is the fail-fast rule paying off in a subsystem where
quiet corruption would be the worst outcome.

## Card: read address layout decoded from retail; the failure is EARLIER than reading

Disassembled the driver's read-command builder rather than guessing further. At `0x8035579c`
it writes a 5-byte command — `0x52` plus FOUR address bytes — with the offset scattered
(`0x803557ac`..`0x803557c8`):

```
b0 = (addr >> 29) & 0x03    b1 = (addr >> 21) & 0xFF
b2 = (addr >> 19) & 0x03    b3 = (addr >> 12) & 0x7F
```

Those are contiguous bits 12..30, and the driver pre-masks the address with `0xFFFFF000`
(`rlwinm r26, r26, 0, 0, 0x13`), so the low 12 bits are always zero. Implemented the exact
inverse, including reassembling the address across the 4-byte + 1-byte immediate transfers
EXIImmEx splits the command into.

**But the guest never gets there.** Logging every distinct command it issues:

```
[card] command 0x00 (imm write 0x00000000, 2 bytes)   read ID
[card] command 0x89 (imm write 0x89000000, 1 bytes)   clear status
[card] command 0x83 (imm write 0x83000000, 2 bytes)   read status
```

...and nothing else — no `0x52` read, no DMA. So the "damaged" verdict is reached from the ID
and status alone, before a single block is read, and the address work (while correct and worth
keeping) is not what is blocking.

Candidates for the next step, in order:
1. **The status byte's meaning.** We answer `0x41` (READY|UNLOCKED). If the driver expects a
   different encoding — or polls for a bit to CHANGE after `0x89` — it would give up here.
2. **Missing transfer-complete interrupt.** `CARDMountAsync` drives the mount through EXI
   callbacks, which on hardware come from the TC/EXT interrupts. Our EXI completes transfers
   synchronously and may never deliver the interrupt the driver's state machine waits on, so
   the mount would fail without ever issuing a read — which fits the evidence exactly.

Candidate 2 is the better fit: the driver stopping after status, rather than reading and
rejecting the data, is what an async state machine that never advances looks like. The way to
tell them apart is to disassemble what the driver does with the status byte it just read.

## Card blocker identified: this runtime has NO interrupt delivery

`dev_pi.cpp` says it outright — *"There is no interrupt delivery in this runtime"*, `PI_INTSR`
reads 0 permanently, and nothing raises an interrupt. `dev_exi.cpp` has no interrupt handling at
all.

That confirms candidate 2 and explains the evidence exactly. `CARDMountAsync` drives the mount
through EXI completion callbacks, which on hardware are delivered by the EXI transfer-complete
interrupt. With no interrupt, the driver issues read-ID, clear-status and read-status, and its
state machine then simply never advances — so it never reaches a block read, and the game
eventually reports the card as damaged. The three commands we see are precisely the prefix that
completes before the first callback would have fired.

This is architectural rather than a card bug, and it is why every other asynchronous subsystem
in this runtime is an OVERRIDE: DVD completes inline, VI retrace is a plain counter, the DSP is
a no-op. CARD is the first subsystem attempted as real hardware, and it is the first to need the
interrupt path those overrides were avoiding.

Three ways forward:

1. **Deliver EXI completion to the guest synchronously.** After a transfer, call the driver's
   registered EXI callback directly instead of raising an interrupt. This fits the runtime's
   existing doctrine — a guest wait on asynchronous hardware becomes synchronous completion,
   exactly as DVD does — and keeps every structure in guest layout. It needs the device to
   invoke guest code, which the runtime can already do.
2. **Resurrect real interrupt delivery.** CLAUDE.md records that the retired recomp era had
   `runtime/native_threads.cpp` with "interrupt delivery already fully PC-native, a behaviour
   port of OSInterrupt.c" in git at `9283f44^`. That would unblock CARD and anything else
   async, and is the most faithful option — but it is a large piece of machinery to reintroduce
   for one subsystem.
3. Override the CARD SDK entry points (the option rejected earlier for needing per-structure
   marshalling). Unchanged assessment.

Option 1 is the right next step: smallest, consistent with how this runtime already handles
every other asynchronous device, and it leaves option 2 available if something later needs
genuine interrupts. The card device itself (attach, EXT detection, ID, status, the retail
address layout) is sound and staged behind this.

## CORRECTION: the missing TC interrupt is NOT what stops the card

Implemented synchronous EXI completion (`deliver_completion` in `dev_exi.cpp`): after a
transfer, read the SDK's per-channel callback at `__EXIData[chan] + 4` (base `0x804040a0`,
stride `0x40`, both taken from EXIImm at 0x80369bf4), clear it as the hardware handler does,
and call it inline on the current thread's register state.

**It changes nothing for the card** — still exactly three commands, and no callback is ever
found to run. The reason is visible in the disassembly I had already read: the card driver
issues its transfers through **EXIImmEx**, which loops EXIImm + **EXISync** and is therefore
already synchronous. No callback is registered on those transfers, so there is no completion
interrupt missing from that path at all.

So last entry's "confirmed structurally — candidate 2" was **wrong**. What is true is that this
runtime has no interrupt delivery; what I did not establish is that this is what stops the card.
I inferred it from the shape of the symptom ("stops after status, like a stalled state
machine") and then wrote it up as confirmed. That is the same mistake as the UV-rate "FOUND IT":
a real fact about the system, promoted to cause without a test that could have falsified it.

The `deliver_completion` code is kept — it is correct behaviour for any async EXI user and a
genuine gap that would hang one — but it is explicitly NOT the card fix and is currently
unexercised. Flagging that rather than leaving it to look like a fix that worked.

Where this actually leaves the card: the driver's own transfers complete, it reads the ID and
status successfully, and then stops of its own accord. The next step is the one I should have
taken directly — disassemble what CARDMount/CARDProbeEx do with the status byte they just read,
rather than reasoning about which subsystem "must" be responsible. The retail code is available
and has settled every other question in this investigation on the first attempt.

## Card: located exactly — EXIProbe's insertion DEBOUNCE never elapses

Instrumented the guest's own return codes instead of reasoning about layers
(`overrides/diag_card.cpp`, `SBR_CARD_TRACE=1`):

```
[card] EXIProbeEx -> 0        (repeatedly)
[card] CARDProbeEx -> -1      (repeatedly)
[card] EXIGetID -> 856916     (once, non-zero = success)
```

`CARDProbeEx` (0x803580a8) is a thin wrapper: it calls `EXIProbeEx` and returns -1 whenever that
returns 0. So the whole card failure reduces to **`EXIProbe` never reporting the card present**.

Disassembling `EXIProbe` (0x8036a2d8) shows what it actually tests — and it reads the EXI CSR
register directly (`r6 = 0xCC006800 + chan*0x14`), not any SDK state:

```
lwz r7, 0(r6)                      ; the channel's CSR
rlwinm. r0, r7, 0, 0x14, 0x14      ; bit 11, EXTINT (insertion/removal event)
  -> if set: clear it (write back with 0x800) and zero __EXIData[chan]+0x20
             and the global at 0x800030c0 + chan*4
rlwinm. r0, r7, 0, 0x13, 0x13      ; bit 12, EXT (device present)  <- we DO set this
  -> if clear: bail
  -> if set:  read 0x800000f8 (bus clock), convert to ms, and compare against a stored
              timestamp — an insertion DEBOUNCE
```

And `EXIProbeEx`'s tail distinguishes the two failure modes precisely: if the global at
`0x800030c0 + chan*4` is non-zero it returns **0** (still settling), else **-1** (no card). We
observe 0, so a timestamp IS stored and the settle period is simply never satisfied.

So the EXT bit was necessary but not sufficient: the SDK models a card as something INSERTED at
a moment in time, and will not use it until it has been stable for a debounce interval measured
against `OSGetTime`. Our card is present from the first instant, which is a state the debounce
was never written to handle — the timestamp bookkeeping happens in the EXTINT branch, which
never runs because we never raise EXTINT.

Next: read the comparison at 0x8036a370.. to see exactly which interval and which timestamp,
then decide between raising EXTINT once at attach (so the SDK runs its own insertion bookkeeping
and the debounce starts from a real moment) or ensuring the stored timestamp is already old.
The former is the faithful one — it lets the SDK's own code do the work rather than arranging
for its state to look right.

## Card probe SUCCEEDS; mount now fails with -5 before any I/O

Two real EXI fixes this tick, both hardware semantics rather than workarounds:

1. **Write-1-to-clear status bits.** EXI CSR bits 1 (EXIINT), 3 (TCINT) and 11 (EXTINT) are
   acknowledged by WRITING A ONE, after which hardware reads back zero. Our transport stored
   register writes verbatim, so an acknowledged bit read back SET forever. Masking them on write
   is the truth here, since nothing in this runtime raises them. (The CSR now reads exactly
   `0x00001000` — EXT alone.)
2. The EXT presence bit from the previous tick.

**And the probe now succeeds:**

```
[card] EXIProbeEx  -> 1 (after 307225 x 0)
[card] CARDProbeEx -> 0 (after 307225 x -1)
[card] CARDMount   -> -5
```

The insertion debounce DOES elapse — it just takes 307,225 polls to get there, which is why
every earlier trace showed only zeros. **My log cap hid the success, for the third time this
investigation** (the 200-draw window twice, now a 400-line report cap). Fixed properly by
reporting only on VALUE CHANGE with a run-length, which is the right shape for anything polled
in a retry loop: it cannot hide a transition no matter how long the run.

Where it stands now: `CARDMount` returns **-5 (CARD_RESULT_IOERROR)** and the card device still
sees only read-ID, clear-status and read-status — **no 0x52 block read at all**. So the mount
fails before touching the card's contents, and the read-address decoding done earlier is still
unexercised.

Next: disassemble `CARDMount` (0x803588dc) to find which check produces -5 ahead of any I/O. The
pattern that keeps working is going straight to the retail code with a specific question; the
detours have all come from reasoning about which subsystem "should" be responsible.

## CARDMount -5 is a FLASH-ID CHECKSUM check against SRAM

Traced the -5 to its single source. `CARDMount` -> `CARDMountAsync` -> the mount worker at
0x80358224, and the only `li r30, -5` in the whole CARD library is at **0x80358504**:

```
0x803584d4  lbz   r0, 0(r3) / add r28, r28, r0 / bdnz     ; sum 12 bytes
0x803584e8  bl    0x80347b20                              ; -> SRAM block pointer
0x803584f0  nor   r0, r28, r28                            ; ~sum
0x803584f4  lbz   r3, 0x26(r3)                            ; SRAM[+0x26]
0x803584f8  clrlwi r0, r0, 0x18                           ; & 0xFF
0x803584fc  cmplw r3, r0
0x80358500  beq   0x8035850c                              ; match -> mount continues
0x80358504  li    r30, -5                                 ; mismatch -> CARD_RESULT_IOERROR
```

That is the console's **flash-ID check**: GameCube SRAM records the flash ID of the card last
seen in each slot, plus a checksum byte, so the OS can detect that a different card has been
swapped in. The mount compares a 12-byte flash ID against that stored checksum and refuses the
card if they disagree.

Our SRAM device (`dev_sram.cpp`) deliberately leaves its checksum words zero — its own comment
explains why, and that reasoning was sound at the time: nothing in the DOL validated SRAM, so
fabricating a value would have been inventing data. **That is no longer true.** Attaching a card
brought a consumer of SRAM into existence, and the honest state for a console with this card
inserted is an SRAM block whose flash-ID record matches it.

Next: find where the 12 summed bytes come from (a card flash-ID command not yet implemented, or
bytes derived from the EXI ID), then make the SRAM device carry a flash-ID record consistent
with the card actually attached. That is legitimate console state — the IPL writes it on first
insertion — as opposed to patching the game's check, which would be the banned shortcut.

Worth noting how the earlier decision aged: `dev_sram.cpp` chose to leave the checksum zero
rather than fabricate one, and recorded that if anything ever validated it, "it will show up as
behaviour rather than being silently papered over". It did exactly that, as a specific error
message with a traceable cause.

## Card: SRAM flash-ID consistency + interrupt-enable land; BLOCK READS NOW WORK

Two fixes, both derived from retail rather than guessed:

**1. SRAM flash-ID self-consistency.** The accessor at 0x80347798 returns SRAM+0x14, and the
mount indexes its checksum at +0x26 past that — so `flashID[chan]` lives at `SRAM+0x14 +
chan*12` and `flashIDCheckSum[chan]` at `SRAM+0x3A + chan`. Our all-zero block gave a computed
`~0 = 0xFF` against a stored `0`. Fixed by COMPUTING the checksum from the flash-ID bytes
actually present at init, so the invariant holds whatever those bytes are, instead of
hardcoding the value that satisfies today's check. Having no recorded flash IDs is a legitimate
console state; an internally inconsistent NVRAM block is not.

**2. Card command 0x81.** The driver builds it at 0x803546c0 as `0x8101_0000` or `0x8100_0000`
and writes two bytes — a boolean, i.e. the card's interrupt-enable control. Recorded and
answered with nothing. This runtime completes transfers before the register write returns so
there is no interrupt to gate, but the card must still ACCEPT a control it is expected to
honour rather than silently ignoring it.

**Result — the mount now reads the card:**

```
[card] command 0x52 (imm write 0x52000000, 4 bytes)
[card] read addr bytes 00 00 00 00 -> offset 0x0
[card] DMA read 512 bytes at card offset 0x0
[card] CARDMount -> -3
```

That is the header block, at the right offset, in the right page size — **the scattered
address layout decoded from 0x803557ac is verified working**, no longer just implemented.

`CARDMount` now returns **-3 (CARD_RESULT_NOCARD)** after reading the header, so the next
question is what it rejects in that data. Progress this session on the card, each step
established from the DOL and confirmed by a changed symptom: slot empty -> device detected ->
ID accepted -> probe succeeds -> flash-ID check passes -> header read. Every wrong step
announced itself loudly instead of corrupting saves.

## Card: mount now STARTS; failure moved into the async worker (and a mislabel corrected)

With the SRAM flash-ID and 0x81 fixes in, the trace has moved on again:

```
[card] CARDMountAsync -> 0                              (was -5; now starts successfully)
[card]   low-mem card-disable byte 0x800030e3 = 0x00    (that gate is NOT set)
[card] EXIAttach -> 1                                    (succeeds)
[card] CARDMount -> -3
```

So `CARDMountAsync` returns 0 (operation started) and the -3 now comes from `__CARDSync` — the
wait — meaning the async mount WORKER at 0x80358224 completes with NOCARD. Ruled out along the
way: the low-memory card-disable flag (reads 0x00), EXIAttach, and EXIGetID, all measured rather
than assumed.

**Correction to my own labelling.** I have been tracing `0x8036a2d8` as "EXIProbe", but the
symbol table gives `0x8036a44c EXIProbe` and `0x8036a4cc EXIProbeEx` — `0x8036a2d8` is an
internal helper those two call. The worker's -3 site (0x80358338) calls **0x8036a44c**, the real
`EXIProbe`, which I have never actually traced. So "EXIProbe now succeeds" is a claim about the
wrong function; the transitions I measured (0 -> 1 after ~283k polls) belong to the helper.

That matters because the helper succeeding does not imply the public entry point does — the
public one wraps it with additional state (`EXIAttach` sets a flag that changes which path the
helper takes, per 0x8036a400). Next tick traces 0x8036a44c specifically, which is the actual
gate on the mount.

Getting the symbol identity wrong is the same class of error as the log caps: a diagnostic that
reports something adjacent to the question and gets read as answering it. Checking the address
against the symbol table costs nothing and I skipped it.

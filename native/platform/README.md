# Native Platform Seam — Architecture & Phase-2 Plan (task #5)

This directory is the **platform abstraction layer** for the from-scratch native PC
port of Super Mario Sunshine. The game's C++ (`reference/sms` — JSystem + game logic)
compiles natively for x86-64 **and** arm64; the GameCube **SDK/hardware API** it calls
(`#include <dolphin/...>`) is reimplemented here with PC-native subsystems. **No
GameCube, no Dolphin, no PPC, no emulation** — the engine code runs native and these
seams stand in for the console's hardware/OS.

This is the **scaffold** (task #5: analysis + design + interface headers). Phase-2
engineers implement the seam bodies, one subsystem each, in parallel.

- API-surface inventory (what the game actually calls, grounded in the decomp): see
  [`api_surface.md`](api_surface.md).
- Per-subsystem interface headers: `*_seam.h` (signatures + `// TODO` bodies-to-come).
- Compile canary: `platform_stub.cpp` (`#include`s every seam header; compiles clean
  with `g++ -std=c++17 -fsyntax-only -Wall -Wextra`).

## Design principle

Every GC SDK subsystem maps onto a **native subsystem**, not an emulation of GC
hardware. Where the recomp build already has a working native subsystem (renderer,
audio), the seam targets that code/approach as the reference — we reuse the math and
design, re-pointed at native structs instead of guest-RAM layout.

| GC subsystem | Native target | Seam header | Existing reference to reuse |
|---|---|---|---|
| GX (graphics) | ngx native SMS renderer (read J3D/J2D object model, render natively; **no GX FIFO emulation**) | `gx_seam.h` | `runtime/render/` + `runtime/ngx/` |
| OS threads/mutex/cond/msg | host `std::thread`/`std::mutex`/`condition_variable` (or cooperative scheduler) | `os_seam.h` | `docs/native_threading.md` (token+condvar substrate) |
| OS interrupts | a recursive global "OS lock" (critical sections) | `os_seam.h` | — |
| OS time/alarm | host monotonic clock; ticks at `OS_TIME_SPEED` | `os_seam.h` | — |
| OS heap/arena | hand the game's JKR heaps one big native block; OSAlloc over malloc | `os_seam.h` | `JKRHeap`/`JKRExpHeap`/`JKRSolidHeap` (in `reference/sms`) |
| OS cache (DC/IC/LC) | no-ops (coherent host); LC → malloc scratch | `os_seam.h` | — |
| MTX/PSMTX (math) | portable scalar C++ (PSMTX → MTX) | `mtx_seam.h` | `runtime/ngx/ngx_project.h` |
| DVD (disc IO) | native FST + file IO; Yaz0/RARC decompress; BE→native asset swap at load | `dvd_seam.h` | `tools/jingle/` FST/Yaz0/RARC extractor |
| CARD (memcard) | host file-backed save image | `card_seam.h` | `runtime/overrides/native_card.cpp` |
| EXI (external bus) | thin shim (CARD/RTC go direct to host files; don't model the bus) | `exi_seam.h` | — |
| AI/DSP/AR (audio HW) | native_jas voice synth + host audio device; DSP inert; ARAM → native heap | `audio_seam.h` | `runtime/native_jas.cpp`, `runtime/native_audio.cpp` |
| VI (video/present) | host window swapchain + vsync pacing (the 60 Hz heartbeat) | `vi_seam.h` | `runtime/render/ngx_present.cpp` |
| PAD/SI (input) | host gamepad/keyboard → `PADStatus`; SI reports controller present | `pad_seam.h` | recomp input mapping (`main_sdl.cpp`) |
| THP (FMV) | native demux + DCT video decode + ADPCM audio → renderer + audio device | `thp_seam.h` | recomp THP DCT decode (dcbz/comb fix) |
| CRT/runtime | host C++ runtime; native `main()` replaces GC `__start` | `platform.h` | — |

### Why no GX FIFO / no EXI bus model

Two deliberate non-goals: (1) **GX** is not a register/FIFO emulator — the game already
builds a J3D/J2D object model and sets GX *state*; the seam records that state into a
native GX context and routes draws to ngx (the renderer the recomp build proved out).
(2) **EXI** is not a faithful bus — CARD and OS-RTC/SRAM, the only real EXI consumers,
are implemented directly on host files; EXI exists only as a link shim.

## API surface size (per subsystem)

From the decomp (`reference/sms`, real grep counts; `distinct` = functions to satisfy):

| Subsystem | distinct used | total calls | notes |
|---|---|---|---|
| GX | 192 | 3070 | hottest; but ~all are GXSet*/GXLoad* state pokes + GXBegin/Position/End draws — categories, not 192 novel impls |
| OS | ~195* | 1439 | hot path = interrupts (329 calls) + threads/mutex/msg |
| MTX/PSMTX | 26 | 335 | pure math, fully unit-testable, lowest risk |
| DVD | 48 | 190 | FST read + a streaming path |
| EXI | 26* | 165 | mostly inside replaced CARD/RTC; thin shim |
| AR | ~10 | ~29 real | (ARRAY_COUNT macro inflates the naive 144) |
| DSP | 11 | 104 | inert mailbox |
| CARD | 36 | 87 | native_card.cpp already does this |
| VI | 14 | 83 | small; the frame heartbeat |
| AI | 16 | 66 | host audio device + DTK stream |
| THP | 24 | 66 | self-contained FMV decode |
| PAD | 17 | 58 | host input |
| SI | 22 | 75 | folded into PAD |

\* OS/EXI distinct-counts include `OS_*`/`EXI_*` macros + internal helpers caught by
   grep; the public-function surface to implement is smaller. See `api_surface.md`.

## Boot order

Native `main()` (in the integration glue, not here) does what GC `__start` → `OSInit`
→ subsystem inits → game `main()` did. `PlatformInit()` (`platform.h`) stands the seams
up in dependency order:

1. **os** — clock base, default arena/heap, main thread, the OS-lock. (Everything else
   needs OS.)
2. **dvd** — open/parse the disc image FST. (Asset loading needs files.)
3. **card** — open/format the host save image.
4. **audio** — open the host audio device, start native_jas. (Audio is an independent
   clock; bring it up early so it's ready when the game pushes samples.)
5. **vi** — create the host window + swapchain (defines the present surface).
6. **gx** — create the native GX context + ngx renderer (needs the VI surface).
7. **pad** — open host gamepads.
8. **thp** — FMV player init (needs dvd + gx + audio).

Then call the game's `main()`. `PlatformPumpFrame()` runs once per presented frame
(host event pump + host-clock-driven advance); `vi::WaitForRetrace()` is the per-frame
heartbeat that presents and paces.

## Recommended phase-2 work breakdown (parallelizable, one seam per engineer)

Ordered by dependency and by "unblocks the most". Each item is a self-contained seam
with its own header already in place; engineers fill the `// TODO` bodies + add unit
tests (the project's `sunbright-render-test`-style TDD discipline applies — a seam fix
must move a number, not "look right").

**Tier 0 — foundation (must land first; everything depends on these)**
- **E1: OS seam** (`os_seam.h`). Threads/mutex/cond/message/interrupts/time/heap-arena/
  cache. Biggest structural risk: pick threading backend (preemptive + global OS-lock
  vs cooperative scheduler — see header note; recommend preemptive-first). Blocks
  nearly everything. **Pair this with the JKR-heap-over-arena wiring.**
- **E2: MTX/PSMTX seam** (`mtx_seam.h`). Pure math, no deps. Trivially parallel with
  E1; unit-test to bit-faithful. Unblocks any geometry/animation code that links math.

**Tier 1 — IO + assets (depend on OS)**
- **E3: DVD seam** (`dvd_seam.h`). FST parse + file read + Yaz0/RARC + BE→native asset
  swap. Reuse `tools/jingle/` extractor. Unblocks all asset loading → unblocks GX/audio
  content. **Highest-leverage Tier-1 item** (nothing renders without assets).
- **E4: CARD seam** (`card_seam.h`). Port `runtime/overrides/native_card.cpp`. Depends
  on OS (async-completion contract). Independent of DVD.

**Tier 2 — output (depend on OS, and on assets being loadable)**
- **E5: VI seam** (`vi_seam.h`). Host window/swapchain + vsync pacing + retrace
  callback. Small; do before GX (GX needs the present surface). The frame heartbeat.
- **E6: GX seam** (`gx_seam.h`). The big one — but mostly re-pointing ngx at native
  structs. Split internally: (a) state setters → native GX context; (b) draw verbs →
  ngx batches; (c) framebuffer/present/EFB-copy. Depends on VI (surface) + DVD (assets
  to draw) + MTX. Can start the context/setters subtask in parallel with E3/E5.
- **E7: Audio seam** (`audio_seam.h`). AI host device + native_jas + inert DSP + ARAM
  heap. Reuse `runtime/native_jas.cpp` + `runtime/native_audio.cpp`. Depends on OS;
  needs DVD for wave-bank/sequence assets. Independent of GX → fully parallel.

**Tier 3 — peripherals (depend on Tier 0–2)**
- **E8: PAD/SI seam** (`pad_seam.h`). Host input → PADStatus; sampling on VI retrace.
  Depends on OS + VI (cadence). Independent of GX/audio.
- **E9: THP seam** (`thp_seam.h`). FMV demux + DCT video + ADPCM audio. Depends on DVD
  (file), GX (present quad), Audio (PCM). Self-contained otherwise; reuse recomp DCT.
- **E10: EXI seam** (`exi_seam.h`). Thin link shims only; do last / opportunistic. If a
  real direct EXI caller appears outside replaced CARD/RTC, escalate (it signals an
  unported subsystem, not a missing EXI model).

**Critical path:** E1(OS) → E3(DVD) → E5(VI) → E6(GX) → first frame on screen.
Audio (E1→E7) and input (E5→E8) proceed on parallel tracks. MTX (E2) and CARD (E4) are
independent early wins. THP (E9) and EXI (E10) come last.

### Cross-cutting conventions
- All seams live under `sb::platform::<sub>` namespaces; shared scalar types in
  `platform_types.h` (keep the SDK u8/u16/.../f32/BOOL aliases so game headers compile).
- SDK struct layouts (OSThread, DVDFileInfo, CARDFileInfo, PADStatus, GXColor,
  GXRenderModeObj, …) **keep their on-disk SDK layout** — the game embeds them in its
  objects. The seam carries native backing state via opaque handles / internal maps.
  ⚠ SMS quirk (already learned in the recomp): several **GX color setters pass GXColor
  BY POINTER** in the arg (GXSetCopyClear, GXSetChanAmbColor, …) — `gx_seam.h` reflects
  this; honor it in phase-2.
- Architecture-independent: portable C++ only, no x86/arm intrinsics in the seams
  (autovectorization is fine; a faithful scalar impl is the correctness oracle).
- TDD: extract pure units (math, decode, combiner) into headers and unit-test against
  spec-computed truth, mirroring `runtime/render/render_test.cpp`. A seam is "done" when
  its units are green AND it round-trips a real asset/frame — not when it compiles.

## Files in this directory
- `README.md` — this design doc.
- `api_surface.md` — the grounded API inventory (counts + concrete function names).
- `platform_types.h` — shared scalar types + `Result`.
- `platform.h` — `PlatformInit/Shutdown/PumpFrame` + boot order.
- `os_seam.h`, `gx_seam.h`, `mtx_seam.h`, `dvd_seam.h`, `card_seam.h`, `audio_seam.h`,
  `vi_seam.h`, `pad_seam.h`, `thp_seam.h`, `exi_seam.h` — the per-subsystem seams.
- `platform_stub.cpp` — compile canary + skeletal PlatformInit/Shutdown/PumpFrame.

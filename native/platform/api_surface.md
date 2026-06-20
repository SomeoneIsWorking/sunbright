# GC SDK API Surface Inventory (task #5)

Grounded in `reference/sms/include/dolphin/*` (the SDK headers the game's C++ `#include`s)
and the actual call sites under `reference/sms/src/` (`grep` counts below are real, from
this tree on 2026-06-20). Counts are `calls` = total call sites in the decomp, `distinct`
= number of distinct SDK functions used. The native port must satisfy the **distinct used**
set per subsystem; the call counts indicate hot paths to get right first.

| Subsystem | header(s)                       | decls in hdr | distinct USED | total calls | native seam target              |
|-----------|---------------------------------|--------------|---------------|-------------|---------------------------------|
| GX        | `gx/*.h`, `gx.h`                | 256          | 192           | 3070        | ngx native renderer (`gx_seam.h`)   |
| OS        | `os/*.h`, `os.h`               | 167          | 195*          | 1439        | host threads/heaps/clock (`os_seam.h`) |
| MTX/PSMTX | `mtx.h`                         | ~44          | 26            | 335         | native vector math (`mtx_seam.h`)  |
| DVD       | `dvd.h`                         | 72           | 48            | 190         | native FST/file IO (`dvd_seam.h`)  |
| CARD      | `card/*.h`, `card.h`           | 50           | 36            | 87          | host save file (`card_seam.h`)     |
| EXI       | `exi.h`                         | 19           | 26*           | 165         | folded into CARD/RTC backends (`exi_seam.h`) |
| AI        | `ai.h`                          | 26           | 16            | 66          | host audio device (`audio_seam.h`) |
| DSP       | `dsp.h`                         | 15           | 11            | 104         | inert/native_jas (`audio_seam.h`)  |
| AR/ARAM   | `ar.h`                          | 21           | 10            | 144**       | native RAM heap (`audio_seam.h`)   |
| VI        | `vi/*.h`, `vi.h`               | 17           | 14            | 83          | host present/vsync (`vi_seam.h`)   |
| PAD       | `pad.h`                         | 26           | 17            | 58          | host gamepad (`pad_seam.h`)        |
| SI        | `si.h`                          | 26           | 22            | 75          | folded into PAD (`pad_seam.h`)     |
| THP       | `thp.h`                         | 24           | 24            | 66          | native THP/FMV decode (`thp_seam.h`) |

\* OS/EXI "distinct USED" exceeds header decl count because the grep also catches `OS_*`/`EXI_*`
   *macros* and `_OS*`/`__EXI*` internal helpers used in the decomp. The seam covers the public
   functions; macros expand in-tree.
\*\* `AR` call count (144) is inflated by `ARRAY_COUNT` (115), an unrelated `<dolphin>` array
   macro, not the AR/ARAM HW API. Real ARAM API usage is ~29 calls (ARInit/ARAlloc/ARStartDMA/ARQ*).

---

## GX — graphics (256 decl / 192 used / 3070 calls) — HOTTEST

The game builds J3D models and J2D screens, then submits geometry through immediate-mode
`GXBegin/GXPosition*/GXEnd` and sets pipeline state through `GXSet*`. We do **NOT** emulate
the GX FIFO — the ngx renderer reads the J3D/J2D object model and the per-draw GX *state* the
game has set, and renders natively. The GX seam therefore intercepts state-setters (record
into a native GX context) and draw verbs (route to ngx batches).

Hot setters/verbs (call count):
- Geometry/vtx: `GXPosition3f32(142)`, `GXTexCoord2f32(124)`, `GXSetVtxDesc(106)`,
  `GXSetVtxAttrFmt(103)`, `GXPosition3s16(75)`, `GXBegin(57)`, `GXEnd(54)`,
  `GXClearVtxDesc(50)`, `GXColor1u32(52)`, `GXColor4u8(42)` (`gx/GXGeometry.h`, `GXVert.h`).
- TEV/combiner: `GXSetTevOrder(124)`, `GXSetTevAlphaIn(58)`, `GXSetNumTevStages(58)`,
  `GXSetTevColorIn(56)`, `GXSetTevColorOp(50)`, `GXSetTevAlphaOp(50)`, `GXSetTevColor(42)`
  (`gx/GXTev.h`).
- Transform/lighting: `GXLoadPosMtxImm(59)`, `GXSetCurrentMtx(38)`, `GXSetChanCtrl(75)`,
  `GXSetNumChans(49)`, `GXSetChanMatColor(40)`, `GXSetNumTexGens(53)`, `GXSetTexCoordGen2(42)`,
  `GXSetProjection`, `GXSetViewport`, `GXLoadNrmMtxImm` (`gx/GXTransform.h`, `GXLighting.h`).
- Pixel/blend: `GXSetBlendMode(73)`, `GXSetZMode(54)`, `GXSetAlphaUpdate(42)`,
  `GXSetColorUpdate(39)`, `GXSetCullMode(53)`, alpha-compare (`gx/GXPixel.h`).
- Texture: `GXInitTexObj`, `GXLoadTexObj`, `GXInitTlutObj`, `GXLoadTlut` (`gx/GXTexture.h`).
- Framebuffer/present: `GXSetCopyClear`, `GXCopyDisp`, `GXCopyTex`, `GXSetViewport`,
  `GXSetScissor`, `GXSetDispCopySrc/Dst`, `GXSetTexCopySrc` (`gx/GXFrameBuffer.h`).
- Raw register writes: `GX_WRITE_BP_REG(183)` is a macro that pokes the BP register file — in
  the native build these become native GX-context state mutations, not FIFO writes.
- Management: `GXInit`, `GXFlush`, `GXDrawDone`, `GXSetDrawDone`, `GXAbortFrame`,
  `GXSetDrawSync`, `GXReadDrawSync` (`gx/GXManage.h`, `GXFifo.h`).

ngx already implements vertex decode, TEV combiner (`tev_eval.h`), lighting (`ngx_light.h`),
texgen, textures (`tex_decode.cpp`), indirect texturing (`ngx_indirect.h`), PE block, present
(`ngx_present.cpp`), J2D/HUD overlay (`j2d_walk.cpp`). The seam is a thin native re-expression
of the GX entry points onto that pipeline.

## OS — kernel (167 decl / 1439 calls)

The OS API divides cleanly:
- **Threads** (`os/OSThread.h`): `OSCreateThread(18)`, `OSResumeThread(16)`, `OSSuspendThread`,
  `OSExitThread`, `OSJoinThread`, `OSDetachThread`, `OSCancelThread`, `OSYieldThread`,
  `OSSleepThread(16)`, `OSWakeupThread(16)`, `OSInitThreadQueue(15)`, `OSGetCurrentThread`,
  `OS{Get,Set}ThreadPriority`, `OS{En,Dis}ableScheduler`. → host threads / cooperative scheduler.
- **Mutex/Cond** (`os/OSMutex.h`): `OSInitMutex`, `OSLockMutex(16)`, `OSUnlockMutex(21)`,
  `OSTryLockMutex`, `OSInitCond`, `OSWaitCond`, `OSSignalCond`. → `std::mutex`/`condition_variable`.
- **Message queue** (`os/OSMessage.h`): `OSInitMessageQueue(24)`, `OSSendMessage(29)`,
  `OSReceiveMessage(24)`, `OSJamMessage`. → bounded blocking queue.
- **Interrupts** (`os/OSInterrupt.h`): `OSDisableInterrupts(146)`, `OSRestoreInterrupts(183)`,
  `OSEnableInterrupts`, `OSGetInterruptMask`, `OSSetInterruptMask`. On real HW these gate a
  critical section; natively they map to a recursive global "OS lock" (or per-subsystem locks)
  — **the 146/183 call counts are almost all short critical sections around shared state**, so
  this must be cheap and correct (see `os_seam.h` notes).
- **Time/alarm** (`os/OSTime.h`, `OSAlarm.h`): `OSGetTime(28)`, `OSGetTick`, `OSCreateAlarm`,
  `OSSetAlarm`, `OSCancelAlarm`, the `OS_*_TO_TICKS`/`OS_TICKS_TO_*` macros (`OS_TIME_SPEED`
  = bus/4). → host monotonic clock; ticks are a fixed-rate counter.
- **Heap/alloc** (`os/OSAlloc.h`, `OSMemory.h`): `OSInitAlloc`, `OSCreateHeap`, `OSAllocFromHeap`,
  `OSFreeToHeap`, `OSAllocFromArenaLo/Hi`, `OSGetArenaLo/Hi`, `OSCheckHeap`,
  `OSRoundUp/Down32B`. The game's own **JKR heaps** (`JKRExpHeap`/`JKRSolidHeap`/`JKRHeap`)
  sit on top — the OS arena seam just hands JKR a big native block.
- **Cache** (`os/OSCache.h`, `OSLC.h`): `DCFlushRange`, `DCInvalidateRange`, `DCStoreRange`,
  `ICInvalidateRange`, `LCAlloc`, `LCLoadBlock`. → no-ops on a coherent host (DC/IC), or a
  scratch `malloc` for the locked-cache (LC) DMA scratch (THP uses LC as scratch).
- **Context/exception/font/reset/RTC** (`OSContext.h`, `OSException.h`, `OSFont.h`,
  `OSReset.h`, `OSRtc.h`): `OSClearContext(38)`, `OSSetCurrentContext(36)`,
  `OSGetCurrentContext`, `OSInitContext`, `OSPanic(36)`, `OSReport(112)`, `OSResetSystem`,
  `OSGetResetCode`, `OS{Get,Set}FontEncode`, `OSLoadFont`. → host-native: panic→abort+log,
  report→printf, context save/restore is mostly vestigial under host threads; font tables
  ship from SDK data; RTC→host clock + EXI SRAM backed file.
- **Module/link** (`os/OSModule.h`): `OSLink`, `OSSearchModule` — SMS does not ship `.rel`
  modules in the playable build; **likely unused at runtime** (stub).

## MTX / PSMTX — matrix/vector math (mtx.h / 335 calls)

`MTXConcat(96)`, `MTXCopy(59)`, `MTXMultVec(47)`, `MTXIdentity(41)`, `MTXTrans`, `MTXScale`,
`MTXInverse`, `MTXRotRad`, `MTXMultVecArray`; `PSMTX*` are the paired-single (Gekko-SIMD)
variants used in hot inner loops. **Pure math, no HW** — straight native C++ (or the existing
`runtime/ngx/ngx_project.h`-style math). `PSMTX*` and `MTX*` are bit-for-bit the same operation
at f32 precision on a host; PS variants just exist for the GC paired-single ISA. This is the
lowest-risk seam (no I/O, fully unit-testable).

## DVD — disc IO (72 decl / 48 used / 190 calls)

`DVDReadPrio(19)`, `DVDOpen(7)`, `DVDClose(15)`, `DVDConvertPathToEntrynum(9)`,
`DVDChangeDir(5)`, `DVDReadAsyncPrio`, `DVDGetCommandBlockStatus(5)`, `DVDCheckDisk`,
`DVDFastOpen`, `DVDInit(4)`, `DVDReset(6)`. Plus `DVDLow*` (raw drive) used by the streaming/
audio path (`DVDLowAudioStream`, `DVDLowRequestAudioStatus`) and BS (`DVDReadAbsAsyncForBS`).
The native seam reads the disc image's **FST** and serves files from host storage (sync or
faked-async), feeding `JKRDvdFile`/`JKRDvdArchive`. The recomp build already has FST + Yaz0 +
RARC decode tooling (`tools/jingle/`, the `sunbright-jingle` extractor) to reference.

## CARD — memory card (50 used / 87 calls)

`CARDMountAsync`, `CARDProbeEx(2)`, `CARDOpen(2)`, `CARDRead(2)`/`CARDReadAsync`,
`CARDWrite(2)`/`CARDWriteAsync`, `CARDGetStatus(3)`, `CARDSetStatus`, `CARDCheckExAsync(3)`,
`CARDClose(7)`, `CARDUnmount(4)`, `CARDFormat`, `CARDCreate`, `CARDDelete`, `CARDRand/Srand`,
`CARDSetIcon*`. The recomp build's `runtime/overrides/native_card.cpp` already serves
probe/mount/read-segment/write-page/erase-sector against a host `.raw` image — reference it for
the file format and the async-completion contract.

## EXI — external bus (26 used / 165 calls)

`EXILock(10)`/`EXIUnlock(26)`, `EXISelect(13)`/`EXIDeselect(19)`, `EXIImm(15)`/`EXIImmEx(11)`,
`EXIDma`, `EXISync(17)`, `EXIProbe(7)`, `EXIGetID`, `EXISetExiCallback`, `EXIAttach`/`EXIDetach`.
EXI is the **transport** under CARD (memcard on EXI0/1) and OS-RTC/SRAM (EXI0 chan2). In the
native port the CARD and RTC seams are implemented directly on host files; EXI itself does not
need a faithful bus model — the EXI seam exists only so any direct `EXI*` caller (mostly inside
CARD/OS-SRAM internals, which we replace) links. Most direct EXI calls live in SDK internals
that the CARD/RTC seams supersede; the seam can be a thin shim or unused.

## AI / DSP / AR — audio HW (16 / 11 / 10 used)

- **AI** (`ai.h`): `AIInit`, `AIInitDMA`, `AIStartDMA`/`AIStopDMA`, `AIRegisterDMACallback`,
  `AISetDSPSampleRate`, and the **streaming** API `AISetStream*`/`AIGetStream*` (DTK/ADPCM
  stream volume + state, `AISetStreamVolLeft/Right(11 each)`). → host audio device clock;
  DMA callback = buffer-refill servo. The recomp build's `runtime/native_audio.cpp` is the
  reference (SDL 48 kHz device drives the audio clock).
- **DSP** (`dsp.h`): `DSPSendMailToDSP(36)`, `DSPCheckMailToDSP(37)`, `DSPReadMailFromDSP`,
  `DSPInit`, `DSPAddTask`, `DSP_CreateMap`. On HW these drive the DSP microcode (the JAudio2
  AX/Zelda ucode). Natively the **DSP is inert** — `native_jas` synthesizes voices on the host
  CPU and writes PCM to the audio device, so the DSP mailbox seam can ack/no-op (the engine
  doesn't need a ucode). Keep the mailbox calls linkable + benign.
- **AR/ARAM** (`ar.h`): `ARInit`, `ARAlloc(7)`, `ARStartDMA(7)`, `ARQ*`, `ARGetSize`,
  `ARGetBaseAddress`. ARAM is the 16 MB aux audio RAM the game stages wave banks into. → a
  plain native heap (`malloc` a block); `ARStartDMA` = `memcpy`. native_jas decodes waves
  straight from the ROM, so ARAM staging is mostly bookkeeping.

## VI — video interface / present (17 used / 83 calls)

`VIInit(2)`, `VIConfigure(3)`, `VISetNextFrameBuffer(4)`, `VIFlush(5)`, `VIWaitForRetrace(16)`,
`VIGetRetraceCount(8)`, `VISet{Pre,Post}RetraceCallback`, `VIGetTvFormat(17)`, `VISetBlack(6)`,
`VIGetNextField(6)`, `VIGetCurrentLine`, `VIGetDTVStatus`. → host window swapchain: present the
ngx frame, pace at the display vsync, run the post-retrace callback once per presented frame
(this is the game's main 60 Hz heartbeat — many engine timers tick off `VIWaitForRetrace`).

## PAD / SI — input (17 / 22 used)

`PADInit(3)`, `PADRead(3)`, `PADReset(5)`, `PADControlMotor(10)` (rumble), `PADClamp`,
`PADSetSamplingCallback`, `PAD{En,Dis}able`, `PADRecalibrate`, `PADSetSpec`. SI
(`SITransfer`, `SIGetType`, polling handlers) is the serial transport under PAD — folded into
the PAD seam (host gamepad → `PADStatus` per channel; SI type = "standard controller present").

## THP — FMV playback (24 used / 66 calls)

`THPPlayerInit`, `THPPlayerOpen`/`Close`, `THPPlayerPrepare`, `THPPlayerPlay(7)`/`Stop`/`Pause`,
`THPPlayerDrawCurrentFrame`, `THPVideoDecode(2)`, `THPAudioDecode`, `THPPlayerGetState`,
`THPPlayerCalcNeedMemory`. THP is Nintendo's MJPEG-ish video + ADPCM audio container. Native
seam = a native THP demux + JPEG-style frame decode (the recomp build already DCT-decodes THP
in the dcbz/comb-fix work) feeding the renderer + audio device.

## CRT / runtime startup

`__start.h`, `__ppc_eabi_init.h`, `PowerPC_EABI_Support/` (Metrowerks runtime: `__init_cpp_exceptions`,
static-ctor tables, `memcpy`/`memset`, `__div2i` etc.). On a native build the host C++ runtime
provides all of this — the seam is just a native `main()` that does what GC `__start` →
`OSInit` → `main` did: init the platform subsystems in order, then call the game's entry. No
EABI runtime is reimplemented; we link the host libc++/compiler-rt.

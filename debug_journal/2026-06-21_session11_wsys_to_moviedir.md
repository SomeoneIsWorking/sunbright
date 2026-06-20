# Session 11 — native sms-boot: WSYS wave banks → opening MovieDirector

Continued the native PC rebuild's runtime bring-up (run→crash→root-cause→fix loop).
Boot advanced from the **WSYS wave-bank SEGV** (session-10 frontier) all the way
through audio init, the running game loop, the GC logo, the post-logo init, a heap
LP64 fix, and into the **opening movie director (TMovieDirector)**. **8 distinct root
causes fixed**, 7 commits (submodule fork `sunbright` + parent `main` gitlink bumps),
all pushed.

Build/run (boot exe, opt-in; ALWAYS pass the ISO):
```
cmake -B build-native -DSMS_BUILD_BOOT=ON -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build-native --target sms-boot -j$(nproc)
SUNBRIGHT_DISC=scratch/disc/sms.iso ./build-native/sms-boot
```
`ctest --test-dir build-native -E platform_test` = 21/21. ⚠ `sms-platform_test`
fails to LINK (undefined `JKRExpHeap::createRoot`) — **PRE-EXISTING at HEAD**, unrelated
to this session's changes (verified by stashing). It blocks the full `cmake --build`
all-target, so build `--target sms-boot` specifically and run ctest with `-E platform_test`.

## Crashes fixed this session (in order hit)

1. **WSYS wave-bank big-endian parse** (submodule JASWaveBankMgr.cpp). The cid-3 WSYS
   blobs are GC big-endian; WSParser reads them via host-endian struct overlay
   (JSUConvertOffsetToPtr) -> wild pointers. Added `sb_wsys_swap_to_host()` (in-place
   swap of the whole WSYS metadata graph: THeader/WBCT/SCNE/C-DF/TCtrlWave +
   WINF/TWaveArchive/TWave), called at the top of registWaveBankWS. No inline pointers
   in WSYS structs -> in-place swap is safe. Layout verified vs tools/jingle/jingle.py
   AND the live aaf (3 banks, all flag=2; bank0=w1stLoad SimpleWaveBank, bank2=22-group
   BasicWaveBank).

2. **unk54/unk60/unk64 wiring** (submodule JAIBasic.cpp). checkInitDataOnMemory now
   builds the unk54 wave-bank table from the cid-3 entries (decomp left it null);
   initBankWave allocates unk60/unk64 (decomp writes but never allocates them).

3. **Synchronous JAS DVD loaders** (submodule JASDvdThread.cpp). USER DIRECTIVE: "the
   game can be sync aside from rendering" — these threads are just background resource
   loaders. loadToDramDvdT/loadToAramDvdT/checkPassDvdT run their handlers INLINE under
   SMS_NATIVE_PLATFORM (no DVD worker thread), eliminating an init-ordering race
   (dvdProcInit used msgBuf that Dvd::init allocates on the audioproc thread -> torn
   under real host threads -> bcopy/OSSendMessage SEGV). ARAM path lazily inits its
   scratch buffers; ARQ DMA to ARAM is inert (native_jas plays from disc).
   ⚠ This sidestepped a deeper GC priority-scheduling gap (OSResumeThread of a
   higher-prio thread should run it until it blocks). We did NOT build that; the
   sync-loader approach is the intended PC architecture. If more init-ordering races
   surface, prefer making that loader synchronous over modeling priority preemption.

4. **ARQ callback pointer width** (submodule ar.h/JKRAramPiece + native ar_dsp_impl).
   ARQCallback userdata was u32; JKRAramPiece's leading-ARQRequest -> JKRAMCommand*
   round-trip truncated on LP64 -> doneDMA got a wild pointer (JKRAram archive DMA, the
   setup-thread SMSLoadArchiveARAM path). Added `ARQRequestRef` (= uintptr_t natively,
   u32 on GC); widened the typedef + doneDMA/aramDmaFinish + the native ARQPostRequest.

5. **GCLogoDir infinite view-tree recursion** (submodule GCLogoDir.cpp). setup() did
   `screen->assignViewObj(stageDisp)` -> the screen's camera connecter points back at
   its own owning stageDisp (stageDisp -> unk14 list -> screen -> camConnecter ->
   stageDisp) -> TViewConnecter::perform / TViewObjPtrListT::perform infinite-recurse
   (stack overflow; unkC perform-flag masks are all 0, nothing cuts it). Every other
   director (MovieDirector, MenuDir) assigns group2d here — DECOMP RECONSTRUCTION ERROR.
   Fixed to assignViewObj(group2d).

6. **JAIData::getInfoPointer reversed bounds comparison** (submodule JAIData.cpp). Read
   `unk2[thing] < tmp`; the original PPC (@80303f60 `cmpl r4,r0`, r4=id&0x3FF,
   r0=unk2[thing]) is `tmp < unk2[thing]`. Reversed, every tmp==0 lookup returned null
   — incl. the JAI init sound 0x80000800 (startSoundDirectID(0x80000800, &unk38)), so
   unk38 stayed null and JAIBasic::processFrameWork null-dereffed on the first game-loop
   frame. (Ground truth via `build-freshtest/sunbright-recomp <ROM> --disasm <addr>` —
   the kept offline analysis tool.)

7. **JSUInputStream big-endian** (submodule JSUInputStream.hpp/.cpp + JSURandomInputStream).
   JSU streams read GC-serialized assets (big-endian) but read()/readData() copy raw
   bytes -> every multi-byte scalar reader returned byteswapped values on LE. Misparsed
   the JDrama NameRef tree in stageArc.bin (TNameRefGen::load -> getNameRef got a garbage
   type string -> null root -> search() null-deref in initialize_nlogoAfter). Fixed
   (SMS_NATIVE_PLATFORM, JSU_BE16/32/64 macros): readU16/S16/U32/S32/U64 + read16b/read32b
   + peekU32 swap their result; readString()/readString(buf,len)/read(char*) swap the raw
   u16 length prefix.

8. **JKRExpHeap CMemBlock LP64 header size** (submodule JKRExpHeap.hpp/.cpp). The
   allocator assumes content (= block + sizeof(CMemBlock), getContent=this+1) is aligned
   to the max alloc alignment (0x10) — holds on GC (header==0x10, 4-byte links). On LP64
   the two link pointers grow CMemBlock to 0x18 -> content is 0x10+8 -> misaligned;
   allocating ~the entire free block (JKRSolidHeap::create(heap->getFreeSize()) at the end
   of initialize_nlogoAfter) overflows by the 8-byte alignment offset -> returns null ->
   sCurrentHeap became null -> `new TMovieDirector` returned null. Fix: pad CMemBlock to
   0x20 (a 0x10 multiple) on LP64; fix the two hardcoded 0x10 header literals (ctor
   initiate, getBlock) to sizeof(CMemBlock). getContent/getHeapBlock already use sizeof.

9. **J2DScreen raw FourCC/size/tag byteswap** (submodule J2DScreen.cpp). makeHiearachyPanes
   reads its magic ('SCRN'/'blo1'/'PAN1'...), block size, and old-format u16 tag via raw
   read()/peek() (NOT readU32/readU16, so not covered by #7). On LE the raw FourCC came
   back reversed -> is_ex mis-detected -> wrong old-format parse -> garbage u16 tag ->
   "unknown pane in SCRN resource" OSPanic. Swap the 4 raw scalar reads with JSU_BE16/32.

After all 9: boot runs PlatformInit → game main → initialize → gameLoop (GC logo via
TGCLogoDir, audio init sound playing, processFrameWork ticking) → joins gSetupThread
(loads mario.arc/common.arc/stageArc.bin + ARAM archives) → initialize_nlogoAfter (NameRef
stage-scenario tree, card option block, new game SolidHeap) → APP_STATE_MOVIE → constructs
TMovieDirector, parses its J2D movie-subtitle SCRN, and runs into decideNextMode.

## CURRENT FRONTIER — TMovieDirector (opening/attract movie, THP) threading

`SUNBRIGHT_DISC=scratch/disc/sms.iso ./build-native/sms-boot` now SEGVs on **Thread 12**:
```
#0 TMovieDirector::decideNextMode(int*)        ; this->unk20 (gamepad, +0x38) == NULL
#1 0x00007ffff3dd44a8 in ?? ()                 ; a STACK address, not code
#2 thread_trampoline
```
`this` (rdi) = 0x7ffff3a42640 — a **thread-stack** address (0x7ffff3xx range), NOT a JKR
heap object (the guest arena is ~0x7efd_xxxx). So a thread was created (thread_trampoline →
n->func) with a **garbage func pointer** (frame #1 is a stack address executed as code),
and it landed in decideNextMode with a bogus `this` whose unk20 reads null. This is the THP
movie director's worker-thread setup (TTHPRender / the movie decode/audio workers spawned in
rsetup, lines 109-130 of MovieDirector.cpp). decideNextMode itself is fine — the real bug is
a wild OSCreateThread (bad func/param or stack) in the THP movie path.

NEXT STEPS (for the next session):
- Find which OSCreateThread in the movie/THP path (rsetup → TTHPRender / TMovieSubTitle /
  TMovieRumble / TCardSave, or the THP decoder) gets a corrupted func/param. Suspect an
  LP64 pointer issue (a thread func/param stored in a struct field typed too narrow, like
  the file-overlay u32-vs-void* landmine from session 5) or a stack-size/overflow in the
  movie worker. Inspect with gdb: break at OSCreateThread, dump func/param/stack for each
  movie-path thread; compare to the decomp source signatures.
- Note the fastboot-era CLAUDE.md gotchas about TMovieDirector: it stores TWO vptrs
  (final vtable 0x803DFA50 on GC); THP workers crash if torn down mid-open. Those are
  recomp-era but the THP-worker fragility is relevant.
- Consider whether the opening movie even needs to PLAY to reach a scene — but don't
  skip it as a bandaid; fix the thread setup. (The user's "sync aside from rendering"
  directive may apply: the THP decode worker could be driven synchronously.)

## Notes / gotchas learned
- The kept offline tool `build-freshtest/sunbright-recomp "$SUNBRIGHT_ROM" --disasm 0xADDR`
  (ROM arg FIRST) is the ground-truth for "is the decomp faithful here?" — used it to
  confirm the getInfoPointer reversed comparison and the processFrameWork no-null-check.
  Function addresses: `reference/sms_gmse01_funcs.txt`.
- The audio/JDrama/J2D decomp has MANY incomplete/non-matching functions AND a pervasive
  big-endian-on-LE-host hazard. Pattern for BE asset parses: a dedicated in-place swapper
  (RARC/WSYS) OR fix the stream reader (JSUInputStream) OR per-field JSU_BE swaps for raw
  read()/peek() of scalars/FourCC. Char-literal FourCC ('SCRN') compared to a raw 4-byte
  read needs the read byteswapped (JSU_BE32), NOT the literal changed.
- LP64 struct-size hazards recur: any GC code that hardcodes a struct/header size (0x10,
  0x14, 0x18...) or stores a pointer in a field sized for 32-bit breaks on the 64-bit host.
  CMemBlock (0x10->0x20) and SDIFileEntry/ARQRequestRef are examples this/prior sessions.
- Working model unchanged (HARD): subagents for RE/porting on DISTINCT files, NEVER
  worktrees, never let agents build; YOU integrate + single build + drive the sequential
  loop. Always headless. Commit submodule fixes to the fork (origin=SomeoneIsWorking/sms
  @`sunbright`), bump the parent gitlink. Scratch in scratch/.

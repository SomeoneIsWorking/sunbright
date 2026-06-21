# Session 15 — THP decode pipeline ported (STAGE A): opening movie plays to its end

Continues session 14 (Hx_ wipe unblocked the movie to STATE_PLAYING). The movie was stuck
in STATE_PLAYING because THPPlayer was a state-field stub with no decode pipeline:
`THPPlayerGetState()` never reached 3 (Done) or 5 (Error), which `TMovieDirector::direct()`
waits on. This session ports the real THP decode pipeline so the movie advances to its end.

## DONE — native THP decode pipeline (STAGE A), faithful port

### Files
- `native/platform/thp_impl.cpp` — REWRITTEN as the faithful `THPPlayer.c` port: real
  disc demux (`THPPlayerOpen`, big-endian-swapped from on-disc bytes), `Prepare` spins the
  Read/Video/Audio decode threads + registers `PlayControl` on VI post-retrace, the
  `PlayControl` end-of-movie state machine, `MixAudio`, and a native audio-clock pump.
- `native/platform/thp_pipeline_impl.cpp` — NEW: the threaded workers (Reader /
  VideoDecoder / AudioDecoder) + their `OSMessageQueue`s, ported from THPRead.c /
  THPVideoDecode.c / THPAudioDecode.c, plus the **ADPCM audio codec** (THPAudio.c,
  faithful). Big-endian handled at every on-disc word (frame sizes, component offsets,
  ADPCM record header). (Build GLOB is `*_impl.cpp` — hence the `_impl` suffix.)
- `native/platform/os_impl.cpp` — added `OSSuspendThread` (+ runtime self-suspend support
  in `OSResumeThread`/`OSCancelThread`) so the decode threads can halt themselves at movie
  end exactly as the decomp does.
- `native/platform/platform_impl.cpp` — disc reader made **thread-safe**: switched
  `gcm_read` from `fseek`+`fread` on a shared `FILE*` to `pread()` (atomic positioned read,
  no shared offset). The engine reads the disc from multiple threads (THP Reader streams
  112 MB while a stage's `loadResource` runs); the old shared-offset path was a latent race.
- `reference/sms/src/System/MarDirector.cpp` — `setupThreadFunc` missing-return fix (same
  omitted-tail-call-return UB session 12 fixed in TMovieDirector; restores
  `return (void*)(intptr_t)...->loadResource()`).
- `native/platform/tests/thp_test.cpp` — REWRITTEN: installs a synthetic in-memory THP disc
  and tests the REAL big-endian demux + CalcNeedMemory/SetBuffer/SetVolume + state guards +
  the ADPCM codec (hand-derived vector). ctest `-E platform_test` → **23/23**, 70 checks.

### Entrance.thp ground truth (probed off the disc)
2816 frames, 640×448 video (videoType=0), **audio present** (2ch/32 kHz/2 tracks),
movieDataOffsets=0x60, firstFrameSize=26208. The end-of-movie gate is therefore
AUDIO-driven: `PlayControl` sets state=3 when `curAudioNumber == numFrames` and the last
audio buffer drains. `curAudioNumber` advances in `MixAudio`, which on GC is pulled by the
AI DMA interrupt; sms-boot has no audio device, so a host audio-clock pump
(`audio_pump`, 32 kHz cadence; `SB_THP_FAST=1` = uncapped test pacing) drives it — the
faithful equivalent. Decoded PCM is currently discarded (no host sink in boot yet).

### Verification (the mechanism is directly observed working)
`SUNBRIGHT_DISC=… SB_MOVIE_DBG=1 SB_THP_FAST=1 SB_THP_DBG=1 ./build-native/sms-boot`:
- Movie advances FADE_IN → PLAYING (thpState 1→2), `internalState` reaches 2.
- `[thp] play` trace shows **curAudio and curVideo climbing in lockstep toward
  numFrames=2816**: …2592/2581 → 2652/2641 → 2712/2701 → **2772/2761** — i.e. the decode
  pipeline streams + demuxes the real Entrance.thp and the end-detection counters advance
  correctly. The state→3 fire is ~44 frames further (see blocker below).
- Unit tests: BE demux fields + ADPCM PCM all match hand-derived truth.

## BLOCKER to observing the literal state→3 — NEW FRONTIER (gameplay stage load)
A **concurrent, deterministic** crash kills the process ~44 frames before the movie ends
(always at curAudio≈2772), so the final `state→3` print isn't reached yet. It is NOT in THP:

```
#0 JPAParticle::JPAParticle()                 (this = 0x8 — bad)
#1 JPAEmitterManager::JPAEmitterManager(...)   new (unkC,0) JPAParticle -> alloc returns ~8
#2 TMarDirector::loadResource()
#3 TMarDirector::setupThreadFunc(void*)        (setup thread, runs during the movie)
```
`new (solidHeap,0) JPAParticle` returns a near-null pointer → the (empty) ctor writes its
vtable to `*0x8` → SEGV. So `JKRSolidHeap`'s alloc returns garbage — a deterministic
`JKRSolidHeap` bring-up bug (suspect: an LP64 pointer-width issue in
`JKRSolidHeap::create`/`alloc`, `reference/sms/src/JSystem/JKernel/JKRSolidHeap.cpp`).
This is the gameplay-stage resource load (TMarDirector = the stage director, the handoff's
"loads the stage = first J3DModel = SLICE 3") and is the NEXT frontier. The DVD
thread-safety fix did NOT change it (crash is deterministic, not a disc race) — but that fix
is a correct keeper.

### JKRSolidHeap crash — investigation so far (for the next session)
- The crash is in the gameplay-stage particle setup: `TMarDirector::loadResource` ->
  `JPAEmitterManager` (called twice in `MarDirectorLoadResource.cpp:37,39`) -> three
  `JKRCreateSolidHeap` (particles/emitters/fields) -> `new (solidHeap,0) JPABaseParticle`
  where the solid-heap alloc returns ~8 (near-null) -> empty ctor writes vtable to `*8`.
- `JKRExpHeap::alloc` (the root/parent) DOES `lock()/unlock()` (per-heap OSMutex, native
  OSLockMutex works), so concurrent allocs from one heap are serialized — not a missing-lock.
- `JKRSolidHeap::create`/`allocFromHead` (JKRSolidHeap.cpp) are LP64-clean on inspection
  (uintptr_t + char* arithmetic). So the bad base must come from `create()`'s
  `ptr = JKRHeap::alloc(...)` (parent) returning a bad/near-null block, or the object being
  corrupted after construction.
- gdb (`break JKRSolidHeap::create; finish; x/24xg $rax`): the first two solid heaps
  (created on the MAIN thread) are clean; the **third, created on Thread 17 (the
  TMarDirector setup thread), has garbage in several object fields** (`0x8ff95d0e…`,
  `0x29e2550b…`, `0xd2daedbe…`) where the main-thread heaps read zero — i.e. either stale
  uninitialised block contents OR live corruption of the setup-thread's heap. Suspect a
  threading/cooperative-non-preemption assumption (os_impl note: host threads are
  preempted, GC was single-core) or a shared-global (sCurrentHeap?) race during the
  concurrent setup-thread + main-thread bring-up. Crash is DETERMINISTIC (same
  curAudio≈2772 every run), which argues against a pure data race.
- DIAGNOSTIC added (env `SB_JKR_DBG`, committed to the fork): prints in
  `JKRSolidHeap::create` (size/parent/ptr/dataPtr/expHeapSize) and in `allocFromHead`
  when it returns a degenerate (<0x10000) pointer. Output: the JPAEmitterManager creates
  three solid heaps with sizes **0x151800 (particles), 0x361ad2c (~56 MB emitters!),
  0x2f60 (fields)** — i.e. `0x100 * sizeof(JPABaseEmitter)` ≈ 56 MB (~221 KB/emitter),
  an LP64-inflated heap-sizing. The degenerate-`allocFromHead` print NEVER fired, so the
  `this=8` is NOT a normal solid-heap alloc return — suspect a corrupted vtable/struct on
  a heap whose backing was clobbered, OR the ~56 MB request exhausting the 64 MB arena.
- ARENA TEST (decisive): bumping `kArenaSize` 64 MB -> 512 MB **moves the crash PAST the
  JPAParticle near-null alloc** to a later `JKRExpHeap::alloc -> OSPanic` on the MAIN
  thread. So memory exhaustion IS implicated, but the gameplay heap-sizing is an LP64
  CASCADE (fix one OOM, hit the next), not a single bug. Reverted to 64 MB (known state)
  pending a proper pass over the JPA/JKR heap-sizing under LP64.
- NEXT: work the gameplay-load heap-sizing as its own task — either (a) right-size the
  arena AND chase each subsequent JKRExpHeap panic, or (b) audit JPABaseParticle/Emitter/
  Field sizeof under LP64 (are the ~221 KB/emitter and the heap-sizing math correct, or is
  a struct over-inflated?). This is the gate to both observing the literal THP state->3 and
  to the first stage J3DModel (renderer SLICE 3). It is a SEPARATE frontier from THP.

### Gameplay-load cascade — fixes landed this session (the LP64 heap/asset bring-up)
Working the crash forward (each is a real bug; the crash moves to the next stage):
1. **JKRSolidHeap header reservation** (`JPAEmitterManager`): `+0x80` was the GC 32-bit
   `expHeapSize`; on LP64 the JKRSolidHeap object is 0xf0, so the data region was short →
   last alloc near-null. Now `+ ALIGN_NEXT(sizeof(JKRSolidHeap),0x10)`.
2. **Particle element size** (`JPAEmitterManager`): the loop allocates `new JPAParticle`
   (264 B) but the heap was budgeted with `sizeof(JPABaseParticle)` (128 B) — under-sized.
   Now `sizeof(JPAParticle)`. (Emitters/fields allocate their base type → unchanged.)
   → crash moved JPAParticle → `JKRMemArchive::open`/`sb_rarc_swap_to_host`.
3. **RARC host side-array alloc** (`sb_rarc_swap_to_host`): the host-only 0x18-stride
   file-entry side array is alloc'd from the archive buffer's own (GC-sized, no-slack)
   heap → fails for tight per-resource heaps. The root heap is no help: a "claim the rest
   of the arena" gameplay solid heap (size scales with the arena: 56 MB at 64 MB arena,
   246 MB at 256 MB) drains root to 0. FIX: fall back to `getCurrentHeap()` (that big
   gameplay heap, which has room), then root; frees via `findFromRoot(nullptr)`.
   → crash moved to `JPAResourceManager::load` → `loadParticleMario` (next stage:
   JKRGetResource/JPAEmitterLoaderDataBase on the loaded .jpa).
- **Arena 64→256 MB** (`platform_impl.cpp`): the gameplay load needs more than the GC
  24 MB+ARAM budget on LP64 (bigger structs + the "claim the rest" heap). Lets more of the
  load run. NOT a fix for the cascade — the per-stage LP64/BE bugs are.
- Diagnostics (env `SB_JKR_DBG`, committed): `[jpa] EmitterMgr` sizes, `[jkr]
  SolidHeap::create`, `[rarc] swap` (heap/root-free/side-ptr).
- ⚠ Observed a NONDETERMINISTIC `terminate called without an active exception` in some
  plain (non-gdb) runs — the concurrent setup-thread gameplay load + THP threads on real
  preemptive host threads expose threading fragility (os_impl note: host threads are
  preempted, GC was single-core cooperative). Watch for this; may need cooperative gating.

## NEXT
1. Continue the gameplay-load cascade from `JPAResourceManager::load` (JKRGetResource /
   JPAEmitterLoaderDataBase on the .jpa resources). It is a SYSTEMIC LP64/BE asset+heap
   bring-up — each gameplay subsystem needs its 64-bit pass. This is the path to both
   observing THP state→3 (the movie completes once the concurrent load stops crashing) and
   the first stage J3DModel (renderer SLICE 3). The earlier (now historical) NEXT:
   the movie completes → observe state→3 → decideNextMode → GAMEPLAY, and progress toward
   the first stage J3DModel (renderer SLICE 3).
2. STAGE B: port the THP video codec (THPDec.c paired-single IDCT) + THPPlayerDrawCurrentFrame
   to put the decoded frames on screen (PPM-verifiable). The pipeline + `dispTextureSet`
   lifecycle are already in place; only `THPVideoDecode` is a documented stub.

## Build/run
```
cmake -S native -B build-native -DSMS_BUILD_BOOT=ON -DCMAKE_BUILD_TYPE=Release  # reconfigure: new files
cmake --build build-native --target sms-boot -j$(nproc)
ctest --test-dir build-native -E platform_test   # 23/23
SUNBRIGHT_DISC=scratch/disc/sms.iso SB_MOVIE_DBG=1 SB_THP_FAST=1 SB_THP_DBG=1 ./build-native/sms-boot
```

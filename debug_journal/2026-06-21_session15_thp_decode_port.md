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

## NEXT
1. Fix the `JKRSolidHeap` alloc-returns-~8 bug (LP64?) so the gameplay preload succeeds →
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

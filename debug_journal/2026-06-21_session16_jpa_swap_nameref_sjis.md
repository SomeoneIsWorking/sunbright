# Session 16 — .jpa BE swap + NameRef Shift-JIS; movie reaches state->3; pivot to sync threading

Continues session 15 (THP decode ported). Goal: clear the concurrent gameplay-stage-load
cascade so the opening movie completes (THP `state->3`) -> decideNextMode -> GAMEPLAY.

## Landed (all committed + pushed; parent main @a7308e0+, fork sunbright @0cf5f8e)

### 1. Native BE->host .jpa (JPA1) swap — particle load no longer fails
`JPAEmitterLoaderDataBase::load` OSPanic'd on the BE magic (`4646454a 3161706a` = 'JEFF'/'jpa1'
byte-reversed). New `reference/sms/include/JSystem/JParticle/JPASwap.h` `sb_jpa_swap_to_host`
swaps one .jpa in place. KEY DESIGN: it is POSITION-AWARE, not a blanket swap, because the
blocks are read two different ways:
- BSP1/ESP1/SSP1/ETX1 (raw `*(T*)(data+off)` casts) + KFA1 (raw f32 keyframe table) + BSP1's
  nested JPAColorRegAnmKey arrays -> swap all their multi-byte fields.
- BEM1/FLD1 are read via `JSUMemoryInputStream`, which ALREADY byteswaps its typed readers
  (readS16/readU16) but NOT raw `read(&x,N)` byte-copies -> swap ONLY the raw-read fields,
  LEAVE the auto-swap fields (else double-swap). => zero loader edits needed.
- GXColor bytes + the TEX1 TIMG (texture path) are not touched. Idempotent; non-JPA1 left alone
  so the caller's fail-fast still fires. TDD: `native/platform/tests/jpa_swap_test.cpp` (asserts
  swapped offsets == the loaders' read offsets; no double-swap; idempotency; bad-magic no-op).
Verified: 240 particle resources parse, 0 nil.

### 2. NameRef search works on native = Shift-JIS string literals (GENERAL fix)
Crash advanced to a NULL `TApplication::unk30` deref in `mountStageArchive` (setup thread).
unk30 is set in `initialize_nlogoAfter` by `TNameRefGen::search<...>("ステージ毎シナリオアーカイブ名群")`.
ROOT CAUSE (confirmed by dumping loaded node bytes): the JDrama NameRef tree LOADS fine (ASCII
type strings), but the search matches a node by NAME via calcKeyCode(hash)+strcmp. The asset
stores the node name in SHIFT-JIS (`83 58 83 65 81 5b ...`, key 0x5fcd) but the decomp source
literal is UTF-8 -> never matches -> search returns null -> unk30 null. NOT a race (the search
result is deterministic; ordering confirmed via [app]/[mardir] markers).
FIX: compile `sms-native` with `-fexec-charset=SHIFT_JIS` (`native/CMakeLists.txt`) so narrow
string literals are emitted as Shift-JIS, exactly as the original Shift-JIS-source build did.
ASCII identical in SJIS, so type strings / printf formats unaffected. unk30 now resolves
non-null. This is GENERAL — every Japanese-named NameRef search in gameplay needed it.

### 3. operator new malloc-fallback on JKR exhaustion (partial; superseded by sync threading)
`reference/sms/.../JKRHeap.cpp`: plain `new` falls back to malloc when JKR heap full/absent
(fail-loud, SB_JKR_DBG-counted); explicit-heap placement-new stays STRICT (fail-fast on a real
solid-heap sizing bug). Catches EXHAUSTION only. Does NOT fix host allocations that succeed into
a JKR heap then get wiped by `freeAll()/destroy()` while a host thread runs (dangling -> the
`OSCreateThread` garbage-ptr crash). That needs the threading rewrite below.

## Result + crash analysis
With #1+#2, the movie reaches `[thp] END-OF-MOVIE: state->3 (audio) curAudioNumber=2816
numFrames=2816` and progresses past it in many runs. Remaining crashes are NONDETERMINISTIC
(across runs: SIGSEGV null-unk30 [pre-#2], OSCreateThread garbage-ptr [host alloc dangling],
std::thread `terminate`, OSPanic JKRHeap:694 [real heap full]) — all symptoms of REAL host
threads (os_impl `std::thread` per OSThread) concurrently touching game state/heap.

## PIVOT (user directive) — make ALL game threading SYNCHRONOUS, no host threads
User: "the game doesn't use threads for gameplay, it just uses them for async resources... make
all of those sync." Chosen scope: fully serial, no host threads. Recommended impl =
cooperative single-real-thread ucontext-fiber scheduler in `native/platform/os_impl.cpp`
(its header already names this "option B / cooperative-fiber backend"). Full design + the one
wrinkle (THP audio-clock pump must be driven from VIWaitForRetrace yielding to the scheduler)
in `scratch/handoff_sync_threading.md`. NEXT SESSION implements it (large, delicate; handed to
a fresh context). Goal after: serial boot -> state->3 -> loadResource -> GAMEPLAY -> first stage
J3DModel = renderer SLICE 3.

## Build/run (headless)
```
cmake -S native -B build-native -DSMS_BUILD_BOOT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --target sms-boot -j$(nproc)
ctest --test-dir build-native -E platform_test   # 24/24
SUNBRIGHT_DISC=scratch/disc/sms.iso SB_MOVIE_DBG=1 SB_THP_FAST=1 SB_JKR_DBG=1 ./build-native/sms-boot
# grep -a (logs have NUL bytes); pkill -9 -x sms-boot after each run; gdb -batch for det. bt
```

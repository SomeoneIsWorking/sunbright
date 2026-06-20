# Session 10 — native sms-boot: RARC → font → DVD → audio init (7 root causes)

Continued the native PC rebuild's runtime bring-up (run→crash→root-cause→fix loop).
Boot advanced from the **RARC archive SEGV** (session-9 frontier) all the way through
font parsing, the DVD-loader thread, and the whole JAI/JAL audio-init data path, to the
**WSYS wave-bank loading** in the logo setup thread. **7 distinct root causes fixed.**
All committed/pushed (parent `main` + submodule fork `sunbright`).

Build/run (boot exe, opt-in; ALWAYS pass the ISO):
```
cmake -B build-native -DSMS_BUILD_BOOT=ON -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build-native --target sms-boot -j$(nproc)
SUNBRIGHT_DISC=scratch/disc/sms.iso ./build-native/sms-boot
```
`ctest --test-dir build-native` = 22/22. ⚠ Running WITHOUT `SUNBRIGHT_DISC` falls back to
the RVZ ($SUNBRIGHT_ROM) which the native reader can't parse → garbage FST → misleading
"file not found" crashes. ALWAYS set `SUNBRIGHT_DISC=scratch/disc/sms.iso`.

## Crashes fixed this session (in order hit)

1. **JKRMemArchive RARC big-endian + LP64 file-entry stride** (submodule
   JKRMemArchive.cpp). RARC archives are GC big-endian; the decomp parses them via
   host-endian struct overlay (header_length 0x20 read as 0x20000000 → wild
   mArcInfoBlock → SEGV in mountFixed/open). Added `sb_rarc_swap_to_host()`: swaps
   SArcHeader/SArcDataInfo/all SDIDirEntry in place to host endian AND builds a
   host-stride (0x18) SDIFileEntry **side array** from the packed 0x14 BE file table
   (mData is a real per-resource heap pointer, can't narrow → indexing the in-blob
   0x14 table with the 0x18 host stride is wrong for i>0). mFileEntries → side array,
   freed in unmountFixed/~JKRMemArchive. Both open() overloads. String table + file
   data stay BE (their own loaders convert).

2. **JUTResFont (.bfn) big-endian** (submodule JUTResFont.cpp). ResFONT is GC
   big-endian; countBlock walked it host-endian (numBlocks/block mType+mSize/glyph+
   map+width fields) → getNext() ran off the buffer → SEGV. Added `bfn_swap_to_host()`:
   swaps header + each block's metadata in place (idempotent via first-block tag guard;
   glyph texture bytes + WID1 width pairs untouched). Called at top of protected_initiate.

3. **JASystem::Dvd::openDvd missing return** (submodule JASDvdThread.cpp). The decomp
   matched MWERKS by falling off the end with no `return` (on PPC r3 retained entryNum
   by luck). On host -O2 the missing-return UB fell THROUGH into the adjacent function,
   re-entering loadToDramDvdTMain → ~9700-deep recursion → **stack overflow** (rsp at the
   guard page; `call strlen` push faulted). Found via scanning the overflowed stack for
   the most-repeated return address. Fix: `return entryNum;` (the documented intent).
   ⚠ This is the pervasive missing-return class (session-9 #5) — expect more.

4. **OSSleepThread missing seam** (native os_impl.cpp). Surfaced once #3 stopped GCC from
   dead-code-eliminating dvdReadMutex's pause path. Added OSSleepThread (block on an
   OSThreadQueue until OSWakeupThread; gen-counter condvar matching OSWakeupThread).

5. **JAIBasic::checkInitDataOnMemory — AAF init-data parse** (submodule JAIBasic.cpp).
   THE big one. The decomp reconstruction is non-matching/incomplete (Fabricated structs,
   "sick of it" TODO) AND read the GC big-endian mSound.aaf host-endian, so the in-memory
   init-data never parsed → unk88 (seInfo) null → getParamSeCategoryMax()==0 → the
   initSeParaLinkBuffer loop bound `seRegistMax*0 - 1` **unsigned-underflowed to
   0xFFFFFFFF** → runaway write off the arena. Rewrote it to parse the REAL AAF format
   (verified vs tools/jingle/jingle.py AND a live walk of the in-memory aaf):
   ```
   u32 cid; cid==0 ends.
   cid in {1,4,5,6,7}: ONE (offset,size,flag) triplet.
   cid in {2,3}:       LIST of (offset,size,flag) triplets until offset==0.
   ```
   All scalars BE; offsets are byte offsets from the aaf base. **SMS aaf layout (live):**
   cid1 single off=0x130 size=0x7c40 = **SE info → unk88**; cid2 multi n=13 = IBNK banks;
   cid3 multi n=3 = WSYS wave banks; cid4-7 small singles; cid0 end. unk4C (the in-memory
   aaf) is NOT freed for unk13==4, so we point directly into it (no transInitDataFile copy).
   seCategoryMax → 9. **Bank/wave registration (unk50/unk54) intentionally left null** (the
   game null-guards them) — see FRONTIER.

6. **JAIData::initInfoDataWork — seInfo per-category table big-endian** (submodule
   JAIData.cpp). The seInfo blob (aaf chunk 1) is BE; the per-category {u16 count; u16 idx}
   entries (stride 4 from offset 6) were read host-endian → the byteswapped `count` drove a
   huge `new u16[count]` in JALSystem::TFlagManager's ctor → SEGV. Read them explicitly BE.

7. (env-gated diag) JAIData SB_JAI_DBG seCategory print.

After all 7: boot runs PlatformInit → game main → initialize → gameLoop (joins the boot
setup thread) → initialize_bootAfter (mounts nintendo.szs RARC, loads the .bfn font, builds
MSound: JAIBasic::initInterfaceMain parses the aaf, JALSystem::init builds TFlagManager) →
**the LOGO setup thread** (setupThreadFuncLogo on its own host thread).

## CURRENT FRONTIER — WSYS wave-bank loading (the next task)

`SUNBRIGHT_DISC=scratch/disc/sms.iso ./build-native/sms-boot` now SEGVs on **Thread 11**
(the logo setup thread):
```
#0 JAIBasic::checkSceneWaveOnMemory(int,int)   (reads unk60[param1] — null)
#1 TApplication::setupThreadFuncLogo()
```
`setupThreadFuncLogo` spins `while(!gpMSound->checkWaveOnAram(MS_WAVE_UNK0)) OSYieldThread();`
→ checkSceneWaveOnMemory reads `unk60[param1]`/`unk64[param1]`. Those arrays (JAIBasic
`s32* unk60/unk64`, the per-bank "wave loaded?" state) are **never allocated** in the decomp
and were only ever written inside `if (unk54){...}` — and I left unk54 (the WSYS wave-bank
list) null in fix #5. So they're null → deref crash.

**This is the wave-bank subsystem, now on the critical path.** To clear it faithfully:
- **Build unk54** (FabricatedUnk54Struct{void* unk0; u32 unk4; u32 unk8}, null-terminated)
  from the **cid-3 WSYS** entries in checkInitDataOnMemory (n=3, each unk0 = aaf+offset,
  unk8 = flag). (And optionally unk50 from cid-2 IBNK, FabricatedUnk50Struct{void* unk0;
  char[4]; int unk8}.)
- **Allocate unk60/unk64** sized to the bank count (init to -1 / 0; the decomp does this in
  the `if(unk54)` loop in initBankWave but never allocates the arrays — the reconstruction
  is incomplete here, same as #5).
- **registWaveBankWS** then parses the **BE WSYS** blob (WaveBankMgr) — another big-endian
  asset layer (WSYS/WINF/.aw wave table; format documented in docs/audio_data_formats.md
  §WSYS). And the setup thread WAITS for waves to actually report "on aram"
  (checkWaveOnAram → true), so the wave LOAD must really complete (or be faithfully modeled),
  not just be allocated — else the while-loop hangs instead of crashing.
- There is NO ARAM natively. The old recomp-era native_jas decoded AFC from the .aw on
  demand (PC-native, no ARAM) — REUSE that knowledge/approach. docs/audio_data_formats.md +
  tools/jingle/jingle.py have the verified WSYS/.aw/AFC parse.

This subsystem (BE WSYS parse + .aw wave loading + ARAM-free wave model + checkWaveOnAram
completion) is sizable — good fan-out for subagents (RE WSYS/WaveBankMgr/.aw on distinct
files), but YOU integrate + build + drive the loop (per the working model).

## Notes / gotchas learned
- `getenv` does NOT resolve in JAIBasic.cpp (a JAS shim shadows <cstdlib>; `std::getenv`
  also fails). It DOES work in JAIData.cpp / Application.cpp with `#include <cstdlib>`. For
  JAIBasic diagnostics use an unconditional OSReport (temporary) or another gate.
- The decomp's audio reconstruction has MANY incomplete/non-matching functions (Fabricated*
  structs, getParamInitDataPointer=null stub, setInfoDataPointer=empty stub,
  checkInitDataOnMemory). Treat audio-init functions as "own it natively / verify against the
  real format", not "trust the decomp body".
- Working model unchanged (HARD): subagents for RE/porting on DISTINCT files, NEVER worktrees,
  never let agents build; YOU integrate + single build + drive the sequential loop. Always
  headless. Commit submodule fixes to the fork (origin=SomeoneIsWorking/sms@`sunbright`), bump
  the parent gitlink. Scratch in scratch/.

# Session 9 — native sms-boot runtime loop: heap-init crash → RARC archive parsing

Continued the native PC rebuild's runtime bring-up (run→crash→root-cause→fix loop). Boot
advanced from the JKRExpHeap::createRoot/initArena heap SEGV (session-8 frontier) all the
way through to RARC archive parsing inside TApplication::initialize_bootAfter. **8 distinct
root causes fixed + disc provisioned.** All committed/pushed (parent + submodule fork).

Build/run (boot exe, opt-in):
```
cmake -B build-native -DSMS_BUILD_BOOT=ON -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build-native --target sms-boot -j$(nproc)
SUNBRIGHT_DISC=scratch/disc/sms.iso ./build-native/sms-boot
```
`ctest --test-dir build-native` still green. Default build (boot OFF) unaffected.

## Crashes fixed this session (in order hit)

1. **JKRHeap::initArena GC physical-low-mem read** (submodule JKRHeap.cpp). `*(u32*)
   (OSPhysicalToCached(0)+0x28)` = `__OSPhysicalMemSize` at 0x80000028, unmapped natively.
   Guarded behind `SMS_NATIVE_PLATFORM` → report host arena size instead (fields are
   informational, only written). New marker macro in `native/shim/dolphin/types.h`.

2. **GX write-gather pipe** (submodule dolphin/gx/GXVert.h). `GXCmd*`/`GXPosition*` write
   to `0xCC008000` MMIO (nonexistent). Redirect `GXFIFO_ADDR` to a host sink
   `sb_gx_wgfifo_sink[32]` (defined in `native/platform/gx_impl.cpp`) under
   SMS_NATIVE_PLATFORM. The native renderer reads the object model, not the FIFO stream.

3. **JGadget::TVector::InsertRaw inverted capacity test** (submodule std-vector.hpp). The
   in-place-shift branch ran when capacity was INSUFFICIENT (`mCapacity <= count+size()`),
   so `mCallbacks(5)` on an empty vector (cap=0) skipped allocation and filled at null
   `pBegin_`. Corrected to `count + size() <= mCapacity`. (Upstream doldecomp has the same
   inverted line — template-header matching is best-effort; behaviorally it must allocate.)

4. **Missing-return UB → cross-function .cold fall-through** (build flag). The decomp is
   full of value-returning functions that fall off the end with no `return` (MWERKS-ism,
   result unused). GCC -O2 hot/cold PARTITIONING placed `JUTGamePad::read`'s `.cold`
   adjacent to `JUTTexture::storeTIMG.cold`, so the fall-through ran into a foreign
   `_Unwind_Resume` landing pad → crash with a fake unwind backtrace. **Fix:**
   `-fno-reorder-blocks-and-partition` on sms-native (keeps blocks within their function).
   ⚠ This is a *partial* mitigation — see #5.

5. **Missing-return UB → intra-function miscompile** (submodule JUTGamePad.cpp,
   MarioGamePad.cpp). Even with #4, `u32 JUTGamePad::read()` with no return let GCC -O2
   treat the function end as unreachable and miscompile the post-loop control flow (the
   pad-list loop re-entered its body with a null link → null deref). **Fix:** added the
   missing returns (`return reset_mask;` / `return 0;`). NOTE: this is a PERVASIVE class —
   expect more missing-return functions on the boot path; fix each as hit (or do a
   `-Werror=return-type` sweep of the boot TUs if it recurs often).

6. **Disc not loaded** (platform). `.env` SUNBRIGHT_ROM points to an RVZ; the native
   reader (`sb_platform_open_gcm`) opens plain GCM/ISO only. Converted once:
   `build/Binaries/dolphin-tool convert -f iso -i "<rvz>" -o scratch/disc/sms.iso`
   (build dolphin-tool target first). Run with `SUNBRIGHT_DISC=scratch/disc/sms.iso`.
   The 1.46 GB ISO is in gitignored scratch/.

7. **JKR root heap created too late** (platform_impl.cpp). The decomp's global `operator new`
   = `JKRHeap::alloc(sCurrentHeap)`. Platform seams allocate (e.g. the DVD FST std::vector
   in sb_dvd_init_from_disc) BEFORE the game's createRoot → null heap → SEGV. **Fix:**
   `PlatformInit` now calls `JKRExpHeap::createRoot(1,false)` right after arena setup.

8. **JKRExpHeap::createRoot not idempotent** (submodule JKRExpHeap.cpp). With the root
   pre-created (#7), the game's own `createRoot(1,false)` left the local `heap` null (only
   set inside the `!sRootHeap` branch) → `heap->mIsRoot = true` null-deref. **Fix:** load
   `heap` from `sRootHeap` when already set (idempotent, returns existing root).

Env-gated heap diagnostics added: `SB_HEAP_DBG=1` (createRoot free size; alloc-fail size).

## CURRENT FRONTIER — RARC archive byte-endianness + LP64 layout (the next task)

`SUNBRIGHT_DISC=scratch/disc/sms.iso ./build-native/sms-boot` now SEGVs in
`JKRMemArchive::mountFixed` → inlined `JKRMemArchive::open(void*, u32, JKRMemBreakFlag)`
(reference/sms/src/JSystem/JKernel/JKRMemArchive.cpp:143). The archive IS loaded and
decompressed (first word at the buffer = bytes "RARC"). The crash is **pure GC
big-endianness**: `mArcHeader->header_length` is read as native little-endian = 0x20000000
(the field is BE 0x00000020 = 32), so `mArcInfoBlock = mArcHeader + 0x20000000` = wild ptr
→ `mov 0x4(%rax)` faults. Verified buffer bytes: `52 41 52 43 | 00 08 4b 60 | 00 00 00 20
| 00 00 02 e0 ...` (BE).

**Two problems to solve together (the asset-data boundary):**
- **Endianness:** the game's JKRMemArchive/JKRArchive read RARC metadata as host-endian,
  but it's GC big-endian. Established pattern = byteswap the asset STRUCTURE to host endian
  at load (like `native/assets/bmd_swap.cpp` does for BMD). Fields to swap: SArcHeader
  (signature/file_length/header_length/file_data_offset/file_data_length...), SArcDataInfo
  (num_nodes/node_offset/num_file_entries/file_entry_offset/string_table_length/
  string_table_offset/nextFreeFileID), all SDIDirEntry (mType/mOffset/_08/mNum/mFirstIdx),
  all SDIFileEntry scalar fields (mFileID/mHash/mFlagsAndNameOffset/mDataOffset/mSize).
  String table + file data stay BE (their own loaders handle them).
- **LP64 layout:** `SDIFileEntry` (JKRArchive.hpp:55) has a trailing `void* mData` (_10),
  so sizeof = 0x18 on LP64 but the RARC FILE packs entries at 0x14. The game indexes
  `mFileEntries[i]` with sizeof stride → wrong for i>0. `mData` CANNOT be narrowed to u32:
  it holds a real runtime heap pointer (DvdArchive/AramArchive store separately-allocated
  buffers there; findPtrResource (JKRArchivePri.cpp) compares it to a host pointer).
  SDIDirEntry is 0x10 on both archs (no pointer) — fine in place.

**Design options for the file-entry table (pick one, verify-first):**
- (A) **Side array in open():** under SMS_NATIVE_PLATFORM, in BOTH JKRMemArchive::open
  overloads, allocate a host `SDIFileEntry[num_file_entries]` (0x18 stride) from the heap,
  populate from the BE 0x14 file table (swap fields, mData=0), point `mFileEntries` at it;
  swap header/datainfo/dir-entries in place; keep mStrTable/mArchiveData pointing into the
  blob. Must free the side array in unmountFixed/unmount. Most localized (no loadToMainRAM
  surgery, no shared-struct change). RECOMMENDED.
- (B) **Expand the blob** to 0x18 stride (realloc + memmove string table & file data,
  bump string_table_offset and header.file_data_offset by num_file_entries*4). Correct but
  needs a bigger buffer + careful offset relocation + ownership handling across the
  loadToMainRAM/open(void*) boundary. More invasive.
- Do NOT narrow mData (breaks Dvd/Aram archives + findPtrResource).

After RARC mounts, the next requirements continue the loop (more managers' init, more
missing-return functions, likely J3D BMD load which already has bmd_swap support, then a
real scene populating a J3DModel = the first Dolphin-oracle-verifiable color frame).

## Working model (unchanged, HARD): SUBAGENTS for RE/porting on DISTINCT files, NEVER
worktrees, never let agents build. YOU integrate + run the single build + drive the
sequential run→crash→fix loop. Always headless. Commit submodule fixes to the fork
(origin=SomeoneIsWorking/sms@sunbright), bump the parent gitlink. Scratch in scratch/.
